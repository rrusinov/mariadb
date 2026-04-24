/* !!! For inclusion into ha_exasol_proxy.cc */

#include "exasol_proxy_pushdown.h"
#include <cstring>
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

void append_exasol_order_clause(String *query,
                                ORDER *order,
                                enum_query_type query_type)
{
  for (; order; order= order->next)
  {
    if (order->counter_used)
    {
      char buffer[20];
      size_t length= my_snprintf(buffer, sizeof(buffer), "%d", order->counter);
      query->append(buffer, static_cast<uint>(length));
    }
    else
    {
      if (order->item[0]->is_order_clause_position())
        query->append(STRING_WITH_LEN("''"));
      else
        (*order->item)->print(query, query_type);
    }

    if (order->direction == ORDER::ORDER_DESC)
      query->append(STRING_WITH_LEN(" DESC NULLS LAST"));
    else
      query->append(STRING_WITH_LEN(" NULLS FIRST"));

    if (order->next)
      query->append(STRING_WITH_LEN(", "));
  }
}

bool should_use_staged_distinct_pushdown(SELECT_LEX *sel_lex)
{
  return sel_lex &&
         (sel_lex->options & SELECT_DISTINCT) &&
         sel_lex->order_list.elements &&
         !sel_lex->limit_params.select_limit;
}

} // namespace

int ha_exasol_proxy_pushdown_handler_base::init_scan_(THD *thd_arg,
                                                      TABLE *table_arg,
                                                      const char *query_text,
                                                      bool clear_temporary_tables_on_close)
{
  if (!exasol_proxy_core_abi || !exasol_proxy_core_abi->openPushedQuery)
    return HA_ERR_INTERNAL_ERROR;

  char error_buffer[512]= {0};
  cursor= exasol_proxy_core_abi->openPushedQuery(thd_arg,
                                                 table_arg,
                                                 query_text,
                                                 clear_temporary_tables_on_close ? 1 : 0,
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

int ha_exasol_proxy_pushdown_handler_base::next_row_(TABLE *table_arg)
{
  if (!cursor || !exasol_proxy_core_abi || !exasol_proxy_core_abi->fetchPushedQueryRow)
    return HA_ERR_END_OF_FILE;

  char error_buffer[512]= {0};
  const int rc= exasol_proxy_core_abi->fetchPushedQueryRow(cursor,
                                                           table_arg,
                                                           table_arg->record[0],
                                                           error_buffer,
                                                           sizeof(error_buffer));
  if (rc != 0 && rc != HA_ERR_END_OF_FILE)
  {
    my_error(ER_GET_ERRNO, MYF(0), rc,
             error_buffer[0] ? error_buffer : "failed to fetch EXASOL pushed query row");
  }
  return rc;
}

int ha_exasol_proxy_pushdown_handler_base::end_scan_()
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

ha_exasol_proxy_derived_handler::ha_exasol_proxy_derived_handler(THD *thd_arg,
                                                                 TABLE_LIST *derived_arg,
                                                                 TABLE *tbl_arg)
  : derived_handler(thd_arg, exasol_proxy_hton),
    ha_exasol_proxy_pushdown_handler_base(tbl_arg),
    query(thd_arg->charset())
{
  derived= derived_arg;
  query.length(0);
  derived_arg->derived->print(&query, PRINT_QUERY_TYPE);
  rewrite_query_for_exasol(&query);
}

ha_exasol_proxy_derived_handler::~ha_exasol_proxy_derived_handler()= default;

ha_exasol_proxy_select_handler::ha_exasol_proxy_select_handler(
    THD *thd_arg, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd_arg, exasol_proxy_hton, lex_unit),
    ha_exasol_proxy_pushdown_handler_base(tbl),
    query(thd_arg->charset()),
    stage_query(thd_arg->charset()),
    staged_order_by(thd_arg->charset()),
    uses_staged_distinct_pushdown(false)
{
  query.length(0);
  stage_query.length(0);
  staged_order_by.length(0);
  lex_unit->print(&query, PRINT_QUERY_TYPE);
  rewrite_query_for_exasol(&query);
}

ha_exasol_proxy_select_handler::ha_exasol_proxy_select_handler(
    THD *thd_arg, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd_arg, exasol_proxy_hton, sel_lex, lex_unit),
    ha_exasol_proxy_pushdown_handler_base(tbl),
    query(thd_arg->charset()),
    stage_query(thd_arg->charset()),
    staged_order_by(thd_arg->charset()),
    uses_staged_distinct_pushdown(false)
{
  query.length(0);
  stage_query.length(0);
  staged_order_by.length(0);

  uses_staged_distinct_pushdown= should_use_staged_distinct_pushdown(sel_lex);
  if (uses_staged_distinct_pushdown)
  {
    sel_lex->master_unit()->print(&stage_query, PRINT_QUERY_TYPE);
    rewrite_query_for_exasol(&stage_query);
    append_exasol_order_clause(&staged_order_by,
                               sel_lex->order_list.first,
                               PRINT_QUERY_TYPE);
    return;
  }

  if (get_pushdown_type() == select_pushdown_type::SINGLE_SELECT)
    sel_lex->master_unit()->print(&query, PRINT_QUERY_TYPE);
  else
    sel_lex->print(thd_arg, &query, PRINT_QUERY_TYPE);
  rewrite_query_for_exasol(&query);
}

ha_exasol_proxy_select_handler::~ha_exasol_proxy_select_handler()
{
  (void) end_scan_();
}

int ha_exasol_proxy_select_handler::init_scan()
{
  if (uses_staged_distinct_pushdown)
  {
    char error_buffer[512]= {0};
    if (!exasol_proxy_core_abi->stagePushedQueryResult)
      return HA_ERR_INTERNAL_ERROR;

    char qualified_name[512]= {0};
    const int stage_rc= exasol_proxy_core_abi->stagePushedQueryResult(
        thd,
        stage_query.ptr(),
        qualified_name,
        sizeof(qualified_name),
        error_buffer,
        sizeof(error_buffer));
    if (stage_rc != 0)
    {
      my_error(ER_GET_ERRNO, MYF(0), stage_rc,
               error_buffer[0] ? error_buffer : "failed to stage EXASOL pushed query");
      return stage_rc;
    }

    query.length(0);
    query.append(STRING_WITH_LEN("SELECT * FROM "));
    query.append(qualified_name, static_cast<uint>(std::strlen(qualified_name)));
    if (staged_order_by.length() > 0)
    {
      query.append(STRING_WITH_LEN(" ORDER BY "));
      query.append(staged_order_by.ptr(), staged_order_by.length());
    }
  }

  return init_scan_(thd, table, query.ptr(), uses_staged_distinct_pushdown);
}

int ha_exasol_proxy_select_handler::next_row()
{
  return next_row_(table);
}

int ha_exasol_proxy_select_handler::end_scan()
{
  return end_scan_();
}
