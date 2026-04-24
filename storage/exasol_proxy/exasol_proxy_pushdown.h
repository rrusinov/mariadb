#ifndef HA_EXASOL_PROXY_PUSHDOWN_INCLUDED
#define HA_EXASOL_PROXY_PUSHDOWN_INCLUDED

#include "sql_string.h"
#include "select_handler.h"

class ha_exasol_proxy;

class ha_exasol_proxy_select_handler: public select_handler
{
public:
  ha_exasol_proxy_select_handler(THD *thd_arg, SELECT_LEX_UNIT *sel_unit, TABLE *tbl);
  ha_exasol_proxy_select_handler(THD *thd_arg, SELECT_LEX *sel_lex,
                                 SELECT_LEX_UNIT *sel_unit, TABLE *tbl);
  ~ha_exasol_proxy_select_handler() override;

  int init_scan() override;
  int next_row() override;
  int end_scan() override;

private:
  TABLE *query_table;
  StringBuffer<512> query;
  StringBuffer<512> stage_query;
  StringBuffer<256> staged_order_by;
  ExasolMariaDBPushedQueryCursor *cursor;
  bool uses_staged_distinct_pushdown;

  static constexpr auto PRINT_QUERY_TYPE=
      enum_query_type(QT_VIEW_INTERNAL | QT_SELECT_ONLY |
                      QT_ITEM_ORIGINAL_FUNC_NULLIF | QT_PARSABLE);
};

#endif
