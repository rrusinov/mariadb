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
  constexpr int max_attempts= 40;
  for (int attempt= 0; ; ++attempt)
  {
    try
    {
      connection.connect_and_enter(options);
      return;
    }
    catch (...)
    {
      connection.close();
      if (attempt + 1 >= max_attempts)
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

void SessionGwThdContext::finish_idle_read_transaction()
{
  if (connected_ && read_transaction_pending_ && open_cursors_ == 0 && open_operations_ == 0)
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
  read_transaction_pending_= false;
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
