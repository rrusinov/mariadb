/* !!! For inclusion into ha_exasol_proxy.cc */

#include "exasol_proxy_pushdown.h"
#include <string>

namespace
{

void rewrite_query_for_exasol(String *query)
{
  if (!query)
    return;

  std::string rewritten(query->ptr(), query->length());
  for (char &ch : rewritten)
  {
    if (ch == '`')
      ch= '"';
  }
  query->length(0);
  query->append(rewritten.c_str(), rewritten.size());
}

} // namespace

ha_exasol_proxy_select_handler::ha_exasol_proxy_select_handler(
    THD *thd_arg, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd_arg, exasol_proxy_hton, lex_unit),
    query_table(tbl),
    query(thd_arg->charset()),
    cursor(nullptr)
{
  query.length(0);
  lex_unit->print(&query, PRINT_QUERY_TYPE);
  rewrite_query_for_exasol(&query);
}

ha_exasol_proxy_select_handler::ha_exasol_proxy_select_handler(
    THD *thd_arg, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd_arg, exasol_proxy_hton, sel_lex, lex_unit),
    query_table(tbl),
    query(thd_arg->charset()),
    cursor(nullptr)
{
  query.length(0);
  if (get_pushdown_type() == select_pushdown_type::SINGLE_SELECT)
    sel_lex->master_unit()->print(&query, PRINT_QUERY_TYPE);
  else
    sel_lex->print(thd_arg, &query, PRINT_QUERY_TYPE);
  rewrite_query_for_exasol(&query);
}

ha_exasol_proxy_select_handler::~ha_exasol_proxy_select_handler()
{
  if (cursor && exasol_proxy_core_abi && exasol_proxy_core_abi->closePushedQuery)
  {
    char error_buffer[512]= {0};
    exasol_proxy_core_abi->closePushedQuery(cursor, error_buffer, sizeof(error_buffer));
    cursor= nullptr;
  }
}

int ha_exasol_proxy_select_handler::init_scan()
{
  if (!exasol_proxy_core_abi || !exasol_proxy_core_abi->openPushedQuery)
    return HA_ERR_INTERNAL_ERROR;

  char error_buffer[512]= {0};
  cursor= exasol_proxy_core_abi->openPushedQuery(thd,
                                                 table,
                                                 query.ptr(),
                                                 error_buffer,
                                                 sizeof(error_buffer));
  if (!cursor)
  {
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR,
             error_buffer[0] ? error_buffer : "failed to open EXASOL pushed query");
    return HA_ERR_INTERNAL_ERROR;
  }
  return 0;
}

int ha_exasol_proxy_select_handler::next_row()
{
  if (!cursor || !exasol_proxy_core_abi || !exasol_proxy_core_abi->fetchPushedQueryRow)
    return HA_ERR_END_OF_FILE;

  char error_buffer[512]= {0};
  const int rc= exasol_proxy_core_abi->fetchPushedQueryRow(cursor,
                                                           table,
                                                           table->record[0],
                                                           error_buffer,
                                                           sizeof(error_buffer));
  if (rc != 0 && rc != HA_ERR_END_OF_FILE)
  {
    my_error(ER_GET_ERRNO, MYF(0), rc,
             error_buffer[0] ? error_buffer : "failed to fetch EXASOL pushed query row");
  }
  return rc;
}

int ha_exasol_proxy_select_handler::end_scan()
{
  if (cursor && exasol_proxy_core_abi && exasol_proxy_core_abi->closePushedQuery)
  {
    char error_buffer[512]= {0};
    const int rc=
        exasol_proxy_core_abi->closePushedQuery(cursor, error_buffer, sizeof(error_buffer));
    cursor= nullptr;
    if (rc != 0)
    {
      my_error(ER_GET_ERRNO, MYF(0), rc,
               error_buffer[0] ? error_buffer : "failed to close EXASOL pushed query");
      return rc;
    }
  }
  return 0;
}
