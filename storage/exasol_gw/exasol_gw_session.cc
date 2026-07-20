#define MYSQL_SERVER 1
#include <my_global.h>

#include "handler.h"
#include "sql_class.h"
#include "log.h"

#include "exasol_gw_session.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <inttypes.h>
#include <stdexcept>
#include <thread>

extern handlerton *exasol_gw_hton;

namespace exasol_gw
{

namespace
{
std::string trim_identity_token(const std::string &value)
{
  const std::size_t first= value.find_first_not_of(" \t\r\n");
  if (first == std::string::npos)
    return {};
  const std::size_t last= value.find_last_not_of(" \t\r\n");
  return value.substr(first, last - first + 1U);
}

std::string authenticated_principal(const THD *thd)
{
  const char *authenticated_user= thd->main_security_ctx.user;
  const char *authenticated_host= thd->main_security_ctx.host_or_ip;
  const std::string user= authenticated_user == nullptr ? "" : authenticated_user;
  const std::string host= authenticated_host == nullptr ? "" : authenticated_host;
  return user + "@" + host;
}

bool identity_is_allowed(const std::string &allowed, const std::string &user,
                         const std::string &principal)
{
  std::size_t offset= 0;
  while (offset <= allowed.size())
  {
    const std::size_t comma= allowed.find(',', offset);
    const std::string token= trim_identity_token(
        allowed.substr(offset, comma == std::string::npos ? std::string::npos : comma - offset));
    if (token == "*" || token == user || token == principal)
      return true;
    if (comma == std::string::npos)
      break;
    offset= comma + 1U;
  }
  return false;
}

std::string audit_identity_component(const std::string &value)
{
  static constexpr char hex[]= "0123456789ABCDEF";
  std::string result;
  result.reserve(value.size());
  for (const unsigned char byte: value)
  {
    const bool safe= (byte >= 'a' && byte <= 'z') ||
                     (byte >= 'A' && byte <= 'Z') ||
                     (byte >= '0' && byte <= '9') || byte == '_' ||
                     byte == '-' || byte == '.' || byte == ':';
    if (safe)
      result.push_back(static_cast<char>(byte));
    else
    {
      result.push_back('%');
      result.push_back(hex[byte >> 4U]);
      result.push_back(hex[byte & 0x0fU]);
    }
  }
  return result;
}

std::string audit_client_name(const std::string &user, const std::string &host)
{
  std::string result= "ExasolGateway MariaDB " + audit_identity_component(user) +
                      "@" + audit_identity_component(host);
  constexpr std::size_t max_client_name_bytes= 255U;
  if (result.size() > max_client_name_bytes)
    result.resize(max_client_name_bytes);
  return result;
}

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

SessionGwThdContext::SessionGwThdContext(THD *thd): thd_(thd)
{
  const char *mode= std::getenv("EXASOL_SESSIONGW_IDENTITY_MODE");
  if (mode == nullptr || std::string(mode) != "service_account")
  {
    throw SessionGwError(
        SessionGwErrorCategory::not_authorized,
        "Exasol Gateway identity mode is not configured; set "
        "EXASOL_SESSIONGW_IDENTITY_MODE=service_account explicitly");
  }

  const char *authenticated_user= thd_->main_security_ctx.user;
  const char *authenticated_host= thd_->main_security_ctx.host_or_ip;
  const std::string user= authenticated_user == nullptr ? "" : authenticated_user;
  const std::string host= authenticated_host == nullptr ? "" : authenticated_host;
  authenticated_principal_= authenticated_principal(thd_);
  const char *allowed= std::getenv("EXASOL_SESSIONGW_ALLOWED_MARIADB_USERS");
  if (allowed == nullptr || !identity_is_allowed(allowed, user, authenticated_principal_))
  {
    throw SessionGwError(
        SessionGwErrorCategory::not_authorized,
        "MariaDB principal '" + authenticated_principal_ +
            "' is not authorized to use the Exasol Gateway service account");
  }
  options_.client_name= audit_client_name(user, host);
}

void SessionGwThdContext::validate_authenticated_principal(const THD *thd) const
{
  if (authenticated_principal(thd) != authenticated_principal_)
  {
    throw SessionGwError(
        SessionGwErrorCategory::not_authorized,
        "MariaDB authenticated principal changed while an Exasol Gateway "
        "context was active; reconnect before using EXASOL tables");
  }
}

SessionGwThdContext::~SessionGwThdContext()
{
  if (connected_ && transaction_active_ && remote_autocommit_known_ && !remote_autocommit_)
  {
    try
    {
      connection_.rollback();
    }
    catch (...)
    {
      // Session teardown still closes the transport; the server rolls back
      // every uncommitted transaction during session cleanup.
    }
  }
  if (!options_.instrumentation_enabled)
    return;
  const SessionGwClientStatistics &client= connection_.statistics();
  sql_print_information(
      "SessionGW performance: connection_attempts=%" PRIu64
      " connection_retries=%" PRIu64 " requests=%" PRIu64
      " request_bytes=%" PRIu64 " response_bytes=%" PRIu64
      " network_ns=%" PRIu64 " transport_read_calls=%" PRIu64
      " transport_read_iterations=%" PRIu64
      " transport_read_bytes=%" PRIu64 " transport_read_ns=%" PRIu64
      " websocket_header_calls=%" PRIu64 " websocket_header_ns=%" PRIu64
      " websocket_payload_calls=%" PRIu64 " websocket_payload_ns=%" PRIu64
      " frame_decode_ns=%" PRIu64 " native_fetch_ns=%" PRIu64
      " metadata_hits=%" PRIu64
      " metadata_misses=%" PRIu64 " metadata_refreshes=%" PRIu64
      " cursors_opened=%" PRIu64
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
      client.network_nanoseconds, client.transport_read_calls,
      client.transport_read_iterations, client.transport_read_bytes,
      client.transport_read_nanoseconds,
      client.websocket_header_read_calls, client.websocket_header_read_nanoseconds,
      client.websocket_payload_read_calls, client.websocket_payload_read_nanoseconds,
      client.frame_decode_nanoseconds, client.native_fetch_nanoseconds,
      statistics_.metadata_cache_hits,
      statistics_.metadata_cache_misses, statistics_.metadata_refreshes,
      statistics_.cursors_opened,
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
    remote_autocommit_known_= false;
  }
  return connection_;
}

void SessionGwThdContext::synchronize_autocommit(const bool enabled)
{
  SessionGwConnection &session= connection();
  if (remote_autocommit_known_ && remote_autocommit_ == enabled)
    return;
  session.set_autocommit(enabled);
  remote_autocommit_= enabled;
  remote_autocommit_known_= true;
}

void SessionGwThdContext::participate_in_statement(const bool explicit_transaction)
{
  trans_register_ha(thd_, false, exasol_gw_hton, 0);
  if (explicit_transaction)
    trans_register_ha(thd_, true, exasol_gw_hton, 0);
  synchronize_autocommit(!explicit_transaction);
  transaction_active_= true;
}

void SessionGwThdContext::finish_transaction_boundary()
{
  open_cursors_= 0;
  open_operations_= 0;
  statement_tables_= 0;
  read_transaction_pending_= false;
  metadata_cache_.clear();
  transaction_active_= false;
}

void SessionGwThdContext::commit_transaction(const bool all)
{
  const bool explicit_transaction= remote_autocommit_known_ && !remote_autocommit_;
  if (!connected_ || !transaction_active_ || (!all && explicit_transaction))
    return;
  // MariaDB invokes the autocommit statement callback before releasing table
  // locks. DML contexts and read cursors close during that later unlock; let
  // CloseOperation/finish_idle_read_transaction own the remote boundary.
  if (!all && (open_operations_ != 0 || open_cursors_ != 0 || statement_tables_ != 0))
    return;
  try
  {
    connection_.commit();
    finish_transaction_boundary();
  }
  catch (...)
  {
    reset();
    throw;
  }
}

void SessionGwThdContext::rollback_transaction(const bool all)
{
  const bool explicit_transaction= remote_autocommit_known_ && !remote_autocommit_;
  if (!connected_ || !transaction_active_)
    return;
  if (!all && explicit_transaction)
    thd_->mark_transaction_to_rollback(true);
  try
  {
    connection_.rollback();
    finish_transaction_boundary();
  }
  catch (...)
  {
    reset();
    throw;
  }
}

SessionGwDescribeTableResult SessionGwThdContext::describe_table(
    const std::string &schema, const std::string &table)
{
  constexpr auto statistics_freshness= std::chrono::seconds(5);
  SessionGwConnection &session= connection();
  const auto now= std::chrono::steady_clock::now();
  for (CachedMetadata &cached: metadata_cache_)
  {
    if (cached.value.schema_name != schema || cached.value.table_name != table)
      continue;
    if (options_.instrumentation_enabled)
      ++statistics_.metadata_cache_hits;
    const std::string current_version= session.get_table_version(schema, table);
    const bool statistics_expired= now - cached.statistics_refreshed >= statistics_freshness;
    if (current_version != cached.value.table_version || statistics_expired)
    {
      cached.value= session.describe_table(schema, table);
      cached.statistics_refreshed= now;
      if (options_.instrumentation_enabled)
        ++statistics_.metadata_refreshes;
    }
    return cached.value;
  }

  if (options_.instrumentation_enabled)
    ++statistics_.metadata_cache_misses;
  SessionGwDescribeTableResult described= session.describe_table(schema, table);
  metadata_cache_.push_back(CachedMetadata{described, now});
  return described;
}

void SessionGwThdContext::read_cursor_opened()
{
  ++open_cursors_;
  transaction_active_= true;
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
  transaction_active_= true;
  if (options_.instrumentation_enabled)
    ++statistics_.operations_opened;
}

void SessionGwThdContext::operation_closed()
{
  if (open_operations_ > 0)
    --open_operations_;
  if (open_operations_ == 0)
  {
    read_transaction_pending_= false;
    // Row-count statistics change independently of the schema generation.
    // Refresh metadata after every local mutation boundary.
    metadata_cache_.clear();
    if (remote_autocommit_known_ && remote_autocommit_)
      transaction_active_= false;
  }
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
      statement_tables_ == 0 && remote_autocommit_known_ && remote_autocommit_)
  {
    connection_.commit();
    read_transaction_pending_= false;
    transaction_active_= false;
  }
}

void SessionGwThdContext::reset() noexcept
{
  // Invalidate local ownership before best-effort transport cleanup. A broken
  // connection must not leave this THD looking connected or trigger replay.
  const bool explicit_transaction= remote_autocommit_known_ && !remote_autocommit_;
  if (explicit_transaction && transaction_active_ && thd_)
    thd_->mark_transaction_to_rollback(true);
  connected_= false;
  open_cursors_= 0;
  open_operations_= 0;
  statement_tables_= 0;
  read_transaction_pending_= false;
  remote_autocommit_known_= false;
  transaction_active_= false;
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
    context= new SessionGwThdContext(thd);
    thd_set_ha_data(thd, exasol_gw_hton, context);
  }
  context->validate_authenticated_principal(thd);
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
