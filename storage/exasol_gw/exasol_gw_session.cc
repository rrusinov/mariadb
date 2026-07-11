#define MYSQL_SERVER 1
#include <my_global.h>

#include "handler.h"
#include "sql_class.h"

#include "exasol_gw_session.h"

#include <algorithm>
#include <chrono>
#include <stdexcept>
#include <thread>

extern handlerton *exasol_gw_hton;

namespace exasol_gw
{

namespace
{
void connect_with_retry(SessionGwConnection &connection, const SessionGwOptions &options)
{
  // Only bootstrap of a fresh physical session is repeated here. No cursor or
  // write request has been issued, and active SessionGW work is never replayed.
  constexpr int max_attempts= 40;
  for (int attempt= 0; ; ++attempt)
  {
    try
    {
      connection.connect_and_enter(options);
      return;
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

SessionGwConnection &SessionGwThdContext::connection()
{
  if (!connected_)
  {
    connect_with_retry(connection_, options_);
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
    const std::string current_version= session.get_table_version(schema, table);
    if (current_version != cached.table_version)
      cached= session.describe_table(schema, table);
    return cached;
  }

  SessionGwDescribeTableResult described= session.describe_table(schema, table);
  metadata_cache_.push_back(described);
  return described;
}

void SessionGwThdContext::read_cursor_opened()
{
  ++open_cursors_;
  read_transaction_pending_= true;
}

void SessionGwThdContext::read_cursor_closed()
{
  if (open_cursors_ > 0)
    --open_cursors_;
  finish_idle_read_transaction();
}

void SessionGwThdContext::operation_opened()
{
  ++open_operations_;
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

void SessionGwThdContext::finish_idle_read_transaction()
{
  if (connected_ && read_transaction_pending_ && open_cursors_ == 0 && open_operations_ == 0 &&
      statement_tables_ == 0)
  {
    connection_.commit();
    read_transaction_pending_= false;
  }
}

void SessionGwThdContext::reset()
{
  connection_.close();
  connected_= false;
  open_cursors_= 0;
  open_operations_= 0;
  statement_tables_= 0;
  read_transaction_pending_= false;
  metadata_cache_.clear();
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
