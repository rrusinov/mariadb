#ifndef EXASOL_SESSIONGW_CLIENT_INCLUDED
#define EXASOL_SESSIONGW_CLIENT_INCLUDED

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace exasol_gw
{

enum class SessionGwErrorCategory: std::uint16_t
{
  protocol_error= 1,
  authentication_failed= 2,
  not_authorized= 3,
  object_not_found= 4,
  unsupported_type= 5,
  unsupported_operation= 6,
  transaction_conflict= 7,
  constraint_violation= 8,
  resource_limit= 9,
  cursor_not_found= 10,
  internal_error= 11,
  transport_error= 12,
  // Client-local category: the completion request may have been applied, but
  // no authoritative response was received. This is not a server wire value.
  outcome_unknown= 13
};

class SessionGwError: public std::runtime_error
{
public:
  explicit SessionGwError(const std::string &message)
    : std::runtime_error(message), category_(SessionGwErrorCategory::transport_error)
  {}
  SessionGwError(SessionGwErrorCategory category, const std::string &message)
    : std::runtime_error(message), category_(category)
  {}

  SessionGwErrorCategory category() const noexcept { return category_; }

private:
  SessionGwErrorCategory category_;
};

struct SessionGwOptions
{
  std::string host= "127.0.0.1";
  std::uint16_t port= 8563;
  std::string user= "sys";
  std::string password= "exasol";
  std::string tls_mode= "skip_verify"; // verify, skip_verify, plain
  std::string ca_file;
  // Zero selects projection-aware sizing in the storage engine.
  std::uint32_t fetch_rows= 0;
  bool instrumentation_enabled= false;
};

struct SessionGwClientStatistics
{
  std::uint64_t requests= 0;
  std::uint64_t request_bytes= 0;
  std::uint64_t response_bytes= 0;
  std::uint64_t network_nanoseconds= 0;
  std::uint64_t insert_batches= 0;
  std::uint64_t insert_rows= 0;
  std::uint64_t update_batches= 0;
  std::uint64_t update_rows= 0;
  std::uint64_t delete_batches= 0;
  std::uint64_t delete_rows= 0;
  std::uint64_t native_write_bytes= 0;
  std::uint64_t transaction_conflicts= 0;
};

struct SessionGwOpenCursorResult
{
  std::uint64_t cursor_id= 0;
  std::vector<std::uint8_t> arrow_schema;
};

struct SessionGwRowHandle
{
  std::uint64_t row_number= 0;
};

struct SessionGwFetchResult
{
  std::uint64_t cursor_id= 0;
  bool end_of_cursor= false;
  std::vector<std::uint8_t> arrow_batch;
  std::vector<SessionGwRowHandle> row_handles;
};

enum class SessionGwNativeKind: std::uint8_t
{
  boolean= 1,
  int64= 2,
  double64= 3,
  decimal128= 4,
  date32= 5,
  timestamp_ns= 6,
  utf8= 7,
  decimal32= 8,
  decimal64= 9
};

struct SessionGwNativeColumn
{
  SessionGwNativeKind kind{};
  std::int32_t scale= 0;
  const std::uint8_t *nulls= nullptr;
  std::size_t nulls_size= 0;
  const std::uint8_t *fixed_data= nullptr;
  std::size_t fixed_data_size= 0;
  const std::uint8_t *sizes= nullptr;
  std::size_t sizes_size= 0;
  const std::uint8_t *variable_data= nullptr;
  std::size_t variable_data_size= 0;
  std::vector<std::uint64_t> variable_offsets;
};

struct SessionGwNativeFetchResult
{
  std::uint64_t cursor_id= 0;
  bool end_of_cursor= false;
  std::uint32_t row_count= 0;
  std::vector<SessionGwNativeColumn> columns;
  std::vector<SessionGwRowHandle> row_handles;
  // Retains the SDK-owned borrowed views above.
  std::shared_ptr<void> owner;
};

struct SessionGwDescribeTableResult
{
  std::string schema_name;
  std::string table_name;
  std::string table_version;
  std::vector<std::uint8_t> arrow_schema;
};

struct SessionGwOpenOperationResult
{
  std::uint64_t operation_id= 0;
  std::vector<std::uint8_t> accepted_schema;
};

SessionGwOptions options_from_environment();
void execute_sql(const SessionGwOptions &options, const std::string &sql);

class SessionGwConnection
{
public:
  SessionGwConnection();
  ~SessionGwConnection();

  SessionGwConnection(const SessionGwConnection &)= delete;
  SessionGwConnection &operator=(const SessionGwConnection &)= delete;

  void connect_and_enter(const SessionGwOptions &options);
  void execute_sql_command(const SessionGwOptions &options, const std::string &sql);
  void close();

  SessionGwDescribeTableResult describe_table(const std::string &schema,
                                              const std::string &table);
  std::string get_table_version(const std::string &schema, const std::string &table);
  SessionGwOpenCursorResult open_pushed_query(const std::string &sql);
  // Opens a forward scan; explicit positions use fetch_positioned_rows_native().
  SessionGwOpenCursorResult open_table_scan(const std::string &schema,
                                            const std::string &table,
                                            const std::vector<std::string> &columns,
                                            bool include_row_handles= false);
  SessionGwOpenOperationResult open_table_insert(const std::string &schema,
                                                 const std::string &table,
                                                 const std::vector<std::string> &columns,
                                                 std::uint32_t max_rows_per_batch,
                                                 const std::vector<std::uint8_t> &arrow_schema);
  std::uint64_t insert_rows(std::uint64_t operation_id,
                            std::uint32_t row_count,
                            const std::vector<std::uint8_t> &native_batch);
  SessionGwOpenOperationResult open_table_update(const std::string &schema,
                                                 const std::string &table,
                                                 const std::vector<std::string> &columns,
                                                 std::uint32_t max_rows_per_batch,
                                                 const std::vector<std::uint8_t> &arrow_schema);
  std::uint64_t update_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles,
                            const std::vector<std::uint8_t> &native_batch);
  SessionGwOpenOperationResult open_table_delete(const std::string &schema,
                                                 const std::string &table,
                                                 std::uint32_t max_rows_per_batch);
  std::uint64_t delete_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles);
  void close_operation(std::uint64_t operation_id);
  void set_autocommit(bool enabled);
  void commit();
  void rollback();
  // Fetches only the next sequential Arrow batch from an open cursor.
  SessionGwFetchResult fetch(std::uint64_t cursor_id,
                             std::uint32_t max_rows,
                             std::uint32_t max_bytes= 0);
  // Native batches avoid Arrow data IPC and string conversion in MariaDB.
  SessionGwNativeFetchResult fetch_native(std::uint64_t cursor_id,
                                          std::uint32_t max_rows,
                                          std::uint32_t max_bytes= 0);
  SessionGwNativeFetchResult fetch_positioned_rows_native(
      std::uint64_t cursor_id, const std::vector<SessionGwRowHandle> &row_handles,
      std::uint32_t max_bytes= 0);
  // Arrow remains available for compatibility and generic consumers.
  SessionGwFetchResult fetch_positioned_rows(
      std::uint64_t cursor_id, const std::vector<SessionGwRowHandle> &row_handles,
      std::uint32_t max_bytes= 0);
  void close_cursor(std::uint64_t cursor_id);
  const SessionGwClientStatistics &statistics() const noexcept;

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace exasol_gw

#endif
