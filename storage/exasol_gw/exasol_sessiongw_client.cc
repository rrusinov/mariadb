#include "exasol_sessiongw_client.h"

#include <sessiongw/c_api.h>

#include <algorithm>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace exasol_gw
{
namespace
{
std::string env_or_default(const char *name, const char *fallback)
{
  const char *value= std::getenv(name);
  return value != nullptr && *value != '\0' ? value : fallback;
}

std::uint16_t env_port_or_default(const char *name, std::uint16_t fallback)
{
  const char *value= std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char *end= nullptr;
  errno= 0;
  const unsigned long parsed= std::strtoul(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed == 0 || parsed > 65535)
    throw SessionGwError(SessionGwErrorCategory::protocol_error,
                         std::string("Invalid SessionGW port in ") + name);
  return static_cast<std::uint16_t>(parsed);
}

std::uint32_t env_u32_or_default(const char *name, std::uint32_t fallback)
{
  const char *value= std::getenv(name);
  if (value == nullptr || *value == '\0') return fallback;
  char *end= nullptr;
  errno= 0;
  const unsigned long long parsed= std::strtoull(value, &end, 10);
  if (errno != 0 || end == value || *end != '\0' || parsed > std::numeric_limits<std::uint32_t>::max())
    throw SessionGwError(SessionGwErrorCategory::protocol_error,
                         std::string("Invalid SessionGW integer in ") + name);
  return static_cast<std::uint32_t>(parsed);
}

sessiongw_c_options c_options(const SessionGwOptions &options)
{
  return {options.host.c_str(), options.port, options.user.c_str(), options.password.c_str(),
          options.tls_mode.c_str(), options.ca_file.c_str(), options.instrumentation_enabled};
}

[[noreturn]] void throw_sdk_error()
{
  const auto category= static_cast<SessionGwErrorCategory>(sessiongw_c_last_error_category());
  throw SessionGwError(category, sessiongw_c_last_error_message());
}

void check_sdk(const int result)
{
  if (result != 0) throw_sdk_error();
}

std::vector<const char *> column_ptrs(const std::vector<std::string> &columns)
{
  std::vector<const char *> result;
  result.reserve(columns.size());
  for (const std::string &column : columns) result.push_back(column.c_str());
  return result;
}

std::vector<std::uint64_t> row_numbers(const std::vector<SessionGwRowHandle> &handles)
{
  std::vector<std::uint64_t> result;
  result.reserve(handles.size());
  for (const SessionGwRowHandle handle : handles) result.push_back(handle.row_number);
  return result;
}

template <typename T>
std::vector<std::uint8_t> copy_bytes(const T *data, const std::size_t size)
{
  if (size == 0) return {};
  if (data == nullptr)
    throw SessionGwError(SessionGwErrorCategory::protocol_error,
                         "SessionGW SDK returned a null byte view");
  return {data, data + size};
}
} // namespace

SessionGwOptions options_from_environment()
{
  SessionGwOptions options;
  options.host= env_or_default("EXASOL_SESSIONGW_HOST", options.host.c_str());
  options.port= env_port_or_default("EXASOL_SESSIONGW_PORT", options.port);
  options.user= env_or_default("EXASOL_SESSIONGW_USER", options.user.c_str());
  options.password= env_or_default("EXASOL_SESSIONGW_PASSWORD", options.password.c_str());
  options.tls_mode= env_or_default("EXASOL_SESSIONGW_TLS", options.tls_mode.c_str());
  options.ca_file= env_or_default("EXASOL_SESSIONGW_CA_FILE", "");
  options.fetch_rows= env_u32_or_default("EXASOL_SESSIONGW_FETCH_ROWS", options.fetch_rows);
  options.instrumentation_enabled= env_or_default("EXASOL_SESSIONGW_INSTRUMENTATION", "0") != "0";
  return options;
}

class SessionGwConnection::Impl
{
public:
  ~Impl() { close_noexcept(); }

  void connect_and_enter(const SessionGwOptions &options)
  {
    close_noexcept();
    const sessiongw_c_options converted= c_options(options);
    check_sdk(sessiongw_c_connect(&converted, &session_));
  }

  void execute_sql(const SessionGwOptions &options, const std::string &sql)
  {
    const sessiongw_c_options converted= c_options(options);
    check_sdk(sessiongw_c_execute_sql(&converted, sql.c_str()));
  }

  void close()
  {
    if (session_ == nullptr) return;
    check_sdk(sessiongw_c_close(session_));
    refresh_statistics();
    release();
  }

  SessionGwDescribeTableResult describe_table(const std::string &schema,
                                               const std::string &table)
  {
    sessiongw_c_metadata *metadata= nullptr;
    check_sdk(sessiongw_c_describe_table(require_session(), schema.c_str(), table.c_str(), &metadata));
    std::unique_ptr<sessiongw_c_metadata, decltype(&sessiongw_c_metadata_destroy)> owner(
        metadata, sessiongw_c_metadata_destroy);
    SessionGwDescribeTableResult result;
    result.schema_name= sessiongw_c_metadata_schema_name(metadata);
    result.table_name= sessiongw_c_metadata_table_name(metadata);
    result.table_version= sessiongw_c_metadata_version(metadata);
    std::size_t size= 0;
    const std::uint8_t *schema_bytes= sessiongw_c_metadata_schema_ipc(metadata, &size);
    result.arrow_schema= copy_bytes(schema_bytes, size);
    return result;
  }

  std::string get_table_version(const std::string &schema, const std::string &table)
  {
    sessiongw_c_metadata *metadata= nullptr;
    check_sdk(sessiongw_c_get_table_version(require_session(), schema.c_str(), table.c_str(), &metadata));
    std::unique_ptr<sessiongw_c_metadata, decltype(&sessiongw_c_metadata_destroy)> owner(
        metadata, sessiongw_c_metadata_destroy);
    return sessiongw_c_metadata_version(metadata);
  }

  SessionGwOpenCursorResult open_pushed_query(const std::string &sql)
  {
    sessiongw_c_cursor *cursor= nullptr;
    check_sdk(sessiongw_c_open_pushed_query(require_session(), sql.c_str(), &cursor));
    return keep_cursor(cursor);
  }

  SessionGwOpenCursorResult open_table_scan(const std::string &schema,
                                             const std::string &table,
                                             const std::vector<std::string> &columns,
                                             bool include_row_handles)
  {
    const auto pointers= column_ptrs(columns);
    sessiongw_c_cursor *cursor= nullptr;
    check_sdk(sessiongw_c_open_table_scan(require_session(), schema.c_str(), table.c_str(),
                                          pointers.data(), pointers.size(), include_row_handles, &cursor));
    return keep_cursor(cursor);
  }

  SessionGwFetchResult fetch(std::uint64_t cursor_id, std::uint32_t max_rows,
                             std::uint32_t max_bytes)
  {
    sessiongw_c_fetch *fetch= nullptr;
    check_sdk(sessiongw_c_fetch_rows(require_session(), cursor(cursor_id), max_rows, max_bytes, &fetch));
    return take_fetch(fetch);
  }

  SessionGwNativeFetchResult fetch_native(std::uint64_t cursor_id, std::uint32_t max_rows,
                                           std::uint32_t max_bytes)
  {
    sessiongw_c_native_fetch *fetch= nullptr;
    check_sdk(sessiongw_c_fetch_native(require_session(), cursor(cursor_id), max_rows, max_bytes, &fetch));
    return take_native_fetch(fetch);
  }

  SessionGwNativeFetchResult fetch_positioned_rows_native(
      std::uint64_t cursor_id, const std::vector<SessionGwRowHandle> &row_handles,
      std::uint32_t max_bytes)
  {
    const auto rows= row_numbers(row_handles);
    sessiongw_c_native_fetch *fetch= nullptr;
    check_sdk(sessiongw_c_fetch_positioned_native(require_session(), cursor(cursor_id),
                                                  rows.data(), rows.size(), max_bytes, &fetch));
    return take_native_fetch(fetch);
  }

  SessionGwFetchResult fetch_positioned_rows(
      std::uint64_t cursor_id, const std::vector<SessionGwRowHandle> &row_handles,
      std::uint32_t max_bytes)
  {
    const auto rows= row_numbers(row_handles);
    sessiongw_c_fetch *fetch= nullptr;
    check_sdk(sessiongw_c_fetch_positioned(require_session(), cursor(cursor_id), rows.data(), rows.size(),
                                           max_bytes, &fetch));
    return take_fetch(fetch);
  }

  void close_cursor(std::uint64_t cursor_id)
  {
    auto found= cursors_.find(cursor_id);
    if (found == cursors_.end())
      throw SessionGwError(SessionGwErrorCategory::cursor_not_found, "Unknown SessionGW cursor");
    check_sdk(sessiongw_c_close_cursor(require_session(), found->second));
    sessiongw_c_cursor_destroy(found->second);
    cursors_.erase(found);
  }

  SessionGwOpenOperationResult open_table_insert(
      const std::string &schema, const std::string &table,
      const std::vector<std::string> &columns, std::uint32_t max_rows,
      const std::vector<std::uint8_t> &arrow_schema)
  {
    const auto pointers= column_ptrs(columns);
    sessiongw_c_operation *operation= nullptr;
    check_sdk(sessiongw_c_open_table_insert(require_session(), schema.c_str(), table.c_str(),
                                            pointers.data(), pointers.size(), max_rows,
                                            arrow_schema.data(), arrow_schema.size(), &operation));
    return keep_operation(operation);
  }

  SessionGwOpenOperationResult open_table_update(
      const std::string &schema, const std::string &table,
      const std::vector<std::string> &columns, std::uint32_t max_rows,
      const std::vector<std::uint8_t> &arrow_schema)
  {
    const auto pointers= column_ptrs(columns);
    sessiongw_c_operation *operation= nullptr;
    check_sdk(sessiongw_c_open_table_update(require_session(), schema.c_str(), table.c_str(),
                                            pointers.data(), pointers.size(), max_rows,
                                            arrow_schema.data(), arrow_schema.size(), &operation));
    return keep_operation(operation);
  }

  SessionGwOpenOperationResult open_table_delete(const std::string &schema,
                                                  const std::string &table,
                                                  std::uint32_t max_rows)
  {
    sessiongw_c_operation *operation= nullptr;
    check_sdk(sessiongw_c_open_table_delete(require_session(), schema.c_str(), table.c_str(), max_rows,
                                            &operation));
    return keep_operation(operation);
  }

  std::uint64_t insert_rows(std::uint64_t operation_id, std::uint32_t row_count,
                            const std::vector<std::uint8_t> &native_batch)
  {
    std::uint64_t affected= 0;
    check_sdk(sessiongw_c_insert_rows(require_session(), operation(operation_id), native_batch.data(),
                                      native_batch.size(), &affected));
    ++statistics_.insert_batches;
    statistics_.insert_rows += row_count;
    statistics_.native_write_bytes += native_batch.size();
    return affected;
  }

  std::uint64_t update_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles,
                            const std::vector<std::uint8_t> &native_batch)
  {
    const auto rows= row_numbers(row_handles);
    std::uint64_t affected= 0;
    try
    {
      check_sdk(sessiongw_c_update_rows(require_session(), operation(operation_id), rows.data(), rows.size(),
                                        native_batch.data(), native_batch.size(), &affected));
    }
    catch (const SessionGwError &error)
    {
      if (error.category() == SessionGwErrorCategory::transaction_conflict)
        ++statistics_.transaction_conflicts;
      throw;
    }
    ++statistics_.update_batches;
    statistics_.update_rows += affected;
    statistics_.native_write_bytes += native_batch.size();
    return affected;
  }

  std::uint64_t delete_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles)
  {
    const auto rows= row_numbers(row_handles);
    std::uint64_t affected= 0;
    try
    {
      check_sdk(sessiongw_c_delete_rows(require_session(), operation(operation_id), rows.data(), rows.size(),
                                        &affected));
    }
    catch (const SessionGwError &error)
    {
      if (error.category() == SessionGwErrorCategory::transaction_conflict)
        ++statistics_.transaction_conflicts;
      throw;
    }
    ++statistics_.delete_batches;
    statistics_.delete_rows += affected;
    return affected;
  }

  void close_operation(std::uint64_t operation_id)
  {
    auto found= operations_.find(operation_id);
    if (found == operations_.end())
      throw SessionGwError(SessionGwErrorCategory::protocol_error, "Unknown SessionGW operation");
    check_sdk(sessiongw_c_close_operation(require_session(), found->second));
    sessiongw_c_operation_destroy(found->second);
    operations_.erase(found);
  }

  void set_autocommit(bool enabled)
  { check_sdk(sessiongw_c_set_autocommit(require_session(), enabled)); }
  void commit() { check_sdk(sessiongw_c_commit(require_session())); }
  void rollback() { check_sdk(sessiongw_c_rollback(require_session())); }

  const SessionGwClientStatistics &statistics() noexcept
  {
    refresh_statistics();
    return statistics_;
  }

private:
  void refresh_statistics() noexcept
  {
    if (session_ == nullptr) return;
    sessiongw_c_statistics sdk{};
    if (sessiongw_c_statistics_get(session_, &sdk) == 0)
    {
      statistics_.requests= sdk.requests;
      statistics_.request_bytes= sdk.request_bytes;
      statistics_.response_bytes= sdk.response_bytes;
      statistics_.network_nanoseconds= sdk.network_nanoseconds;
    }
  }

  sessiongw_c_session *require_session() const
  {
    if (session_ == nullptr)
      throw SessionGwError(SessionGwErrorCategory::transport_error, "SessionGW is not connected");
    return session_;
  }

  sessiongw_c_cursor *cursor(std::uint64_t id) const
  {
    const auto found= cursors_.find(id);
    if (found == cursors_.end())
      throw SessionGwError(SessionGwErrorCategory::cursor_not_found, "Unknown SessionGW cursor");
    return found->second;
  }

  sessiongw_c_operation *operation(std::uint64_t id) const
  {
    const auto found= operations_.find(id);
    if (found == operations_.end())
      throw SessionGwError(SessionGwErrorCategory::protocol_error, "Unknown SessionGW operation");
    return found->second;
  }

  SessionGwOpenCursorResult keep_cursor(sessiongw_c_cursor *cursor_handle)
  {
    std::unique_ptr<sessiongw_c_cursor, decltype(&sessiongw_c_cursor_destroy)> owner(
        cursor_handle, sessiongw_c_cursor_destroy);
    SessionGwOpenCursorResult result;
    result.cursor_id= sessiongw_c_cursor_id(cursor_handle);
    std::size_t size= 0;
    const std::uint8_t *schema_bytes= sessiongw_c_cursor_schema_ipc(cursor_handle, &size);
    result.arrow_schema= copy_bytes(schema_bytes, size);
    cursors_.emplace(result.cursor_id, owner.release());
    return result;
  }

  SessionGwNativeFetchResult take_native_fetch(sessiongw_c_native_fetch *fetch_handle)
  {
    std::shared_ptr<void> owner(fetch_handle, [](void *value) {
      sessiongw_c_native_fetch_destroy(static_cast<sessiongw_c_native_fetch *>(value));
    });
    SessionGwNativeFetchResult result;
    result.cursor_id= sessiongw_c_native_fetch_cursor_id(fetch_handle);
    result.end_of_cursor= sessiongw_c_native_fetch_end(fetch_handle) != 0;
    result.row_count= sessiongw_c_native_fetch_row_count(fetch_handle);
    const std::size_t column_count= sessiongw_c_native_fetch_column_count(fetch_handle);
    result.columns.reserve(column_count);
    for (std::size_t column= 0; column < column_count; ++column)
    {
      SessionGwNativeColumn view;
      view.kind= static_cast<SessionGwNativeKind>(
          sessiongw_c_native_fetch_column_kind(fetch_handle, column));
      view.scale= sessiongw_c_native_fetch_column_scale(fetch_handle, column);
      view.nulls= sessiongw_c_native_fetch_column_nulls(fetch_handle, column, &view.nulls_size);
      view.fixed_data= sessiongw_c_native_fetch_column_fixed_data(
          fetch_handle, column, &view.fixed_data_size);
      view.sizes= sessiongw_c_native_fetch_column_sizes(fetch_handle, column, &view.sizes_size);
      view.variable_data= sessiongw_c_native_fetch_column_variable_data(
          fetch_handle, column, &view.variable_data_size);
      if (view.kind == SessionGwNativeKind::utf8)
      {
        view.variable_offsets.reserve(result.row_count + 1U);
        view.variable_offsets.push_back(0U);
        std::uint64_t offset= 0;
        for (std::uint32_t row= 0; row < result.row_count; ++row)
        {
          std::uint64_t size= 0;
          for (unsigned byte= 0; byte < 8U; ++byte)
            size= (size << 8U) | view.sizes[static_cast<std::size_t>(row) * 8U + byte];
          offset += size;
          view.variable_offsets.push_back(offset);
        }
      }
      result.columns.push_back(std::move(view));
    }
    std::size_t count= 0;
    const std::uint64_t *rows= sessiongw_c_native_fetch_row_locations(fetch_handle, &count);
    result.row_handles.reserve(count);
    for (std::size_t index= 0; index < count; ++index) result.row_handles.push_back({rows[index]});
    result.owner= std::move(owner);
    return result;
  }

  SessionGwFetchResult take_fetch(sessiongw_c_fetch *fetch_handle)
  {
    std::unique_ptr<sessiongw_c_fetch, decltype(&sessiongw_c_fetch_destroy)> owner(
        fetch_handle, sessiongw_c_fetch_destroy);
    SessionGwFetchResult result;
    result.cursor_id= sessiongw_c_fetch_cursor_id(fetch_handle);
    result.end_of_cursor= sessiongw_c_fetch_end(fetch_handle) != 0;
    std::size_t size= 0;
    const std::uint8_t *row_bytes= sessiongw_c_fetch_rows_ipc(fetch_handle, &size);
    result.arrow_batch= copy_bytes(row_bytes, size);
    std::size_t count= 0;
    const std::uint64_t *rows= sessiongw_c_fetch_row_locations(fetch_handle, &count);
    result.row_handles.reserve(count);
    for (std::size_t index= 0; index < count; ++index) result.row_handles.push_back({rows[index]});
    return result;
  }

  SessionGwOpenOperationResult keep_operation(sessiongw_c_operation *operation_handle)
  {
    std::unique_ptr<sessiongw_c_operation, decltype(&sessiongw_c_operation_destroy)> owner(
        operation_handle, sessiongw_c_operation_destroy);
    SessionGwOpenOperationResult result;
    result.operation_id= sessiongw_c_operation_id(operation_handle);
    std::size_t size= 0;
    const std::uint8_t *schema_bytes= sessiongw_c_operation_schema_ipc(operation_handle, &size);
    result.accepted_schema= copy_bytes(schema_bytes, size);
    operations_.emplace(result.operation_id, owner.release());
    return result;
  }

  void release() noexcept
  {
    for (auto &entry : cursors_) sessiongw_c_cursor_destroy(entry.second);
    for (auto &entry : operations_) sessiongw_c_operation_destroy(entry.second);
    cursors_.clear();
    operations_.clear();
    sessiongw_c_session_destroy(session_);
    session_= nullptr;
  }

  void close_noexcept() noexcept
  {
    if (session_ != nullptr)
    {
      (void)sessiongw_c_close(session_);
      refresh_statistics();
    }
    release();
  }

  sessiongw_c_session *session_= nullptr;
  std::unordered_map<std::uint64_t, sessiongw_c_cursor *> cursors_;
  std::unordered_map<std::uint64_t, sessiongw_c_operation *> operations_;
  SessionGwClientStatistics statistics_;
};

SessionGwConnection::SessionGwConnection(): impl_(new Impl()) {}
SessionGwConnection::~SessionGwConnection()= default;

void execute_sql(const SessionGwOptions &options, const std::string &sql)
{
  SessionGwConnection connection;
  connection.execute_sql_command(options, sql);
}
void SessionGwConnection::connect_and_enter(const SessionGwOptions &options) { impl_->connect_and_enter(options); }
void SessionGwConnection::execute_sql_command(const SessionGwOptions &options, const std::string &sql) { impl_->execute_sql(options, sql); }
void SessionGwConnection::close() { impl_->close(); }
SessionGwDescribeTableResult SessionGwConnection::describe_table(const std::string &schema, const std::string &table) { return impl_->describe_table(schema, table); }
std::string SessionGwConnection::get_table_version(const std::string &schema, const std::string &table) { return impl_->get_table_version(schema, table); }
SessionGwOpenCursorResult SessionGwConnection::open_pushed_query(const std::string &sql) { return impl_->open_pushed_query(sql); }
SessionGwOpenCursorResult SessionGwConnection::open_table_scan(const std::string &schema, const std::string &table, const std::vector<std::string> &columns, bool include) { return impl_->open_table_scan(schema, table, columns, include); }
SessionGwFetchResult SessionGwConnection::fetch(std::uint64_t id, std::uint32_t rows, std::uint32_t bytes) { return impl_->fetch(id, rows, bytes); }
SessionGwNativeFetchResult SessionGwConnection::fetch_native(std::uint64_t id, std::uint32_t rows, std::uint32_t bytes) { return impl_->fetch_native(id, rows, bytes); }
SessionGwNativeFetchResult SessionGwConnection::fetch_positioned_rows_native(std::uint64_t id, const std::vector<SessionGwRowHandle> &rows, std::uint32_t bytes) { return impl_->fetch_positioned_rows_native(id, rows, bytes); }
SessionGwFetchResult SessionGwConnection::fetch_positioned_rows(std::uint64_t id, const std::vector<SessionGwRowHandle> &rows, std::uint32_t bytes) { return impl_->fetch_positioned_rows(id, rows, bytes); }
void SessionGwConnection::close_cursor(std::uint64_t id) { impl_->close_cursor(id); }
SessionGwOpenOperationResult SessionGwConnection::open_table_insert(const std::string &schema, const std::string &table, const std::vector<std::string> &columns, std::uint32_t rows, const std::vector<std::uint8_t> &arrow) { return impl_->open_table_insert(schema, table, columns, rows, arrow); }
std::uint64_t SessionGwConnection::insert_rows(std::uint64_t id, std::uint32_t rows, const std::vector<std::uint8_t> &batch) { return impl_->insert_rows(id, rows, batch); }
SessionGwOpenOperationResult SessionGwConnection::open_table_update(const std::string &schema, const std::string &table, const std::vector<std::string> &columns, std::uint32_t rows, const std::vector<std::uint8_t> &arrow) { return impl_->open_table_update(schema, table, columns, rows, arrow); }
std::uint64_t SessionGwConnection::update_rows(std::uint64_t id, const std::vector<SessionGwRowHandle> &rows, const std::vector<std::uint8_t> &batch) { return impl_->update_rows(id, rows, batch); }
SessionGwOpenOperationResult SessionGwConnection::open_table_delete(const std::string &schema, const std::string &table, std::uint32_t rows) { return impl_->open_table_delete(schema, table, rows); }
std::uint64_t SessionGwConnection::delete_rows(std::uint64_t id, const std::vector<SessionGwRowHandle> &rows) { return impl_->delete_rows(id, rows); }
void SessionGwConnection::close_operation(std::uint64_t id) { impl_->close_operation(id); }
void SessionGwConnection::set_autocommit(bool enabled) { impl_->set_autocommit(enabled); }
void SessionGwConnection::commit() { impl_->commit(); }
void SessionGwConnection::rollback() { impl_->rollback(); }
const SessionGwClientStatistics &SessionGwConnection::statistics() const noexcept { return impl_->statistics(); }

} // namespace exasol_gw
