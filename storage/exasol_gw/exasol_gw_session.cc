#define MYSQL_SERVER 1
#include <my_global.h>

#include "handler.h"
#include "sql_class.h"
#include "log.h"

#include "exasol_gw_session.h"

#include <algorithm>
#include <chrono>
#include <inttypes.h>
#include <stdexcept>
#include <thread>

extern handlerton *exasol_gw_hton;

namespace exasol_gw
{

namespace
{
std::uint64_t connect_with_retry(SessionGwConnection &connection, const SessionGwOptions &options)
{
  // Only bootstrap of a fresh physical session is repeated here. No cursor or
  // write request has been issued, and active SessionGW work is never replayed.
  constexpr int max_attempts= 40;
  for (int attempt= 0; ; ++attempt)
  {
    try
    {
      connection.connect_and_enter(options);
      return static_cast<std::uint64_t>(attempt + 1);
    }
    catch (const SessionGwError &error)
    {
      connection.close();
      if (error.category() != SessionGwErrorCategory::transport_error ||
          attempt + 1 >= max_attempts)
        throw;
      std::this_thread::sleep_for(
          std::chrono::milliseconds(std::min(250, 25 * (attempt + 1))));
    }
  }
}
} // namespace

SessionGwThdContext::~SessionGwThdContext()
{
  if (!options_.instrumentation_enabled)
    return;
  const SessionGwClientStatistics &client= connection_.statistics();
  sql_print_information(
      "SessionGW performance: connection_attempts=%" PRIu64
      " connection_retries=%" PRIu64 " requests=%" PRIu64
      " request_bytes=%" PRIu64 " response_bytes=%" PRIu64
      " network_ns=%" PRIu64 " metadata_hits=%" PRIu64
      " metadata_misses=%" PRIu64 " cursors_opened=%" PRIu64
      " cursors_closed=%" PRIu64 " operations=%" PRIu64 " fetch_batches=%" PRIu64
      " fetched_rows=%" PRIu64 " positioned_cache_hits=%" PRIu64
      " positioned_fetches=%" PRIu64 " positioned_rows=%" PRIu64 " arrow_bytes=%" PRIu64
      " native_read_bytes=%" PRIu64 " projected_columns=%" PRIu64 " available_columns=%" PRIu64
      " arrow_decode_ns=%" PRIu64 " row_materialize_ns=%" PRIu64
      " native_buffer_ns=%" PRIu64 " native_encode_ns=%" PRIu64
      " insert_batches=%" PRIu64 " insert_rows=%" PRIu64
      " update_batches=%" PRIu64 " update_rows=%" PRIu64
      " delete_batches=%" PRIu64 " delete_rows=%" PRIu64
      " native_write_bytes=%" PRIu64 " transaction_conflicts=%" PRIu64,
      statistics_.connection_attempts, statistics_.connection_retries,
      client.requests, client.request_bytes, client.response_bytes,
      client.network_nanoseconds, statistics_.metadata_cache_hits,
      statistics_.metadata_cache_misses, statistics_.cursors_opened,
      statistics_.cursors_closed, statistics_.operations_opened, statistics_.fetch_batches,
      statistics_.fetched_rows, statistics_.positioned_cache_hits,
      statistics_.positioned_fetches, statistics_.positioned_rows, statistics_.arrow_bytes,
      statistics_.native_read_bytes, statistics_.projected_columns, statistics_.available_columns,
      statistics_.arrow_decode_nanoseconds,
      statistics_.row_materialize_nanoseconds,
      statistics_.native_buffer_nanoseconds, statistics_.native_encode_nanoseconds,
      client.insert_batches,
      client.insert_rows, client.update_batches, client.update_rows,
      client.delete_batches, client.delete_rows, client.native_write_bytes,
      client.transaction_conflicts);
}

SessionGwConnection &SessionGwThdContext::connection()
{
  if (!connected_)
  {
    const std::uint64_t attempts= connect_with_retry(connection_, options_);
    if (options_.instrumentation_enabled)
    {
      statistics_.connection_attempts += attempts;
      statistics_.connection_retries += attempts - 1U;
    }
    connected_= true;
  }
  return connection_;
}

SessionGwDescribeTableResult SessionGwThdContext::describe_table(
    const std::string &schema, const std::string &table)
{
  SessionGwConnection &session= connection();
  for (SessionGwDescribeTableResult &cached: metadata_cache_)
  {
    if (cached.schema_name != schema || cached.table_name != table)
      continue;
    if (options_.instrumentation_enabled)
      ++statistics_.metadata_cache_hits;
    const std::string current_version= session.get_table_version(schema, table);
    if (current_version != cached.table_version)
      cached= session.describe_table(schema, table);
    return cached;
  }

  if (options_.instrumentation_enabled)
    ++statistics_.metadata_cache_misses;
  SessionGwDescribeTableResult described= session.describe_table(schema, table);
  metadata_cache_.push_back(described);
  return described;
}

void SessionGwThdContext::read_cursor_opened()
{
  ++open_cursors_;
  if (options_.instrumentation_enabled)
    ++statistics_.cursors_opened;
  read_transaction_pending_= true;
}

void SessionGwThdContext::read_cursor_closed()
{
  if (open_cursors_ > 0)
  {
    --open_cursors_;
    if (options_.instrumentation_enabled)
      ++statistics_.cursors_closed;
  }
  finish_idle_read_transaction();
}

void SessionGwThdContext::operation_opened()
{
  ++open_operations_;
  if (options_.instrumentation_enabled)
    ++statistics_.operations_opened;
}

void SessionGwThdContext::operation_closed()
{
  if (open_operations_ > 0)
    --open_operations_;
  if (open_operations_ == 0)
    read_transaction_pending_= false;
}

void SessionGwThdContext::statement_table_opened()
{
  ++statement_tables_;
}

void SessionGwThdContext::statement_table_closed()
{
  if (statement_tables_ > 0)
    --statement_tables_;
  finish_idle_read_transaction();
}

void SessionGwThdContext::record_projection(std::size_t projected_columns,
                                             std::size_t available_columns)
{
  if (!options_.instrumentation_enabled)
    return;
  statistics_.projected_columns += projected_columns;
  statistics_.available_columns += available_columns;
}

void SessionGwThdContext::record_fetch(std::size_t rows, std::size_t native_read_bytes,
                                       std::uint64_t decode_nanoseconds, bool positioned)
{
  if (!options_.instrumentation_enabled)
    return;
  ++statistics_.fetch_batches;
  statistics_.fetched_rows += rows;
  if (positioned)
  {
    ++statistics_.positioned_fetches;
    statistics_.positioned_rows += rows;
  }
  statistics_.native_read_bytes += native_read_bytes;
  statistics_.arrow_decode_nanoseconds += decode_nanoseconds;
}

void SessionGwThdContext::record_positioned_cache_hit()
{
  if (options_.instrumentation_enabled)
    ++statistics_.positioned_cache_hits;
}

void SessionGwThdContext::record_row_materialize(std::uint64_t nanoseconds)
{
  if (options_.instrumentation_enabled)
    statistics_.row_materialize_nanoseconds += nanoseconds;
}

void SessionGwThdContext::record_native_buffer(std::uint64_t nanoseconds)
{
  if (options_.instrumentation_enabled)
    statistics_.native_buffer_nanoseconds += nanoseconds;
}

void SessionGwThdContext::record_native_encode(std::uint64_t nanoseconds)
{
  if (options_.instrumentation_enabled)
    statistics_.native_encode_nanoseconds += nanoseconds;
}

bool SessionGwThdContext::instrumentation_enabled() const noexcept
{
  return options_.instrumentation_enabled;
}

void SessionGwThdContext::finish_idle_read_transaction()
{
  if (connected_ && read_transaction_pending_ && open_cursors_ == 0 && open_operations_ == 0 &&
      statement_tables_ == 0)
  {
    connection_.commit();
    read_transaction_pending_= false;
  }
}

void SessionGwThdContext::reset() noexcept
{
  // Invalidate local ownership before best-effort transport cleanup. A broken
  // connection must not leave this THD looking connected or trigger replay.
  connected_= false;
  open_cursors_= 0;
  open_operations_= 0;
  statement_tables_= 0;
  read_transaction_pending_= false;
  metadata_cache_.clear();
  try
  {
    connection_.close();
  }
  catch (...)
  {
  }
}

SessionGwThdContext &session_for_thd(THD *thd)
{
  if (!thd)
    throw std::runtime_error("SessionGW requires a MariaDB THD context");
  auto *context= static_cast<SessionGwThdContext *>(thd_get_ha_data(thd, exasol_gw_hton));
  if (!context)
  {
    context= new SessionGwThdContext();
    thd_set_ha_data(thd, exasol_gw_hton, context);
  }
  return *context;
}

void destroy_session_for_thd(THD *thd)
{
  if (!thd)
    return;
  auto *context= static_cast<SessionGwThdContext *>(thd_get_ha_data(thd, exasol_gw_hton));
  if (!context)
    return;
  delete context;
  thd_set_ha_data(thd, exasol_gw_hton, nullptr);
}

} // namespace exasol_gw
