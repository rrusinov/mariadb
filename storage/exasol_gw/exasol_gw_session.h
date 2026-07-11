#ifndef EXASOL_GW_SESSION_INCLUDED
#define EXASOL_GW_SESSION_INCLUDED

#include "exasol_sessiongw_client.h"

#include <cstddef>
#include <string>
#include <vector>

class THD;

namespace exasol_gw
{

class SessionGwThdContext
{
public:
  SessionGwConnection &connection();
  SessionGwDescribeTableResult describe_table(const std::string &schema,
                                               const std::string &table);
  void read_cursor_opened();
  void read_cursor_closed();
  void operation_opened();
  void operation_closed();
  void statement_table_opened();
  void statement_table_closed();
  void reset();

private:
  void finish_idle_read_transaction();

  SessionGwOptions options_= options_from_environment();
  SessionGwConnection connection_;
  bool connected_= false;
  std::size_t open_cursors_= 0;
  std::size_t open_operations_= 0;
  std::size_t statement_tables_= 0;
  bool read_transaction_pending_= false;
  std::vector<SessionGwDescribeTableResult> metadata_cache_;
};

SessionGwThdContext &session_for_thd(THD *thd);
void destroy_session_for_thd(THD *thd);

} // namespace exasol_gw

#endif
