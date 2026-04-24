#ifndef HA_EXASOL_PROXY_PUSHDOWN_INCLUDED
#define HA_EXASOL_PROXY_PUSHDOWN_INCLUDED

#include "derived_handler.h"
#include "sql_string.h"
#include "select_handler.h"

class ha_exasol_proxy;

class ha_exasol_proxy_pushdown_handler_base
{
protected:
  explicit ha_exasol_proxy_pushdown_handler_base(TABLE *tbl_arg)
    : query_table(tbl_arg), cursor(nullptr)
  {
  }

  int init_scan_(THD *thd_arg,
                 TABLE *table_arg,
                 const char *query_text,
                 bool clear_temporary_tables_on_close);
  int next_row_(TABLE *table_arg);
  int end_scan_();

  TABLE *query_table;
  ExasolMariaDBPushedQueryCursor *cursor;
};

class ha_exasol_proxy_derived_handler: public derived_handler,
                                       public ha_exasol_proxy_pushdown_handler_base
{
public:
  ha_exasol_proxy_derived_handler(THD *thd_arg, TABLE_LIST *derived_arg, TABLE *tbl_arg);
  ~ha_exasol_proxy_derived_handler() override;

  int init_scan() override { return init_scan_(thd, table, query.ptr(), false); }
  int next_row() override { return next_row_(table); }
  int end_scan() override { return end_scan_(); }

private:
  StringBuffer<512> query;

  static constexpr auto PRINT_QUERY_TYPE=
      enum_query_type(QT_VIEW_INTERNAL | QT_SELECT_ONLY |
                      QT_ITEM_ORIGINAL_FUNC_NULLIF | QT_PARSABLE);
};

class ha_exasol_proxy_select_handler: public select_handler,
                                      public ha_exasol_proxy_pushdown_handler_base
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
  StringBuffer<512> query;
  StringBuffer<512> stage_query;
  StringBuffer<256> staged_order_by;
  bool uses_staged_distinct_pushdown;

  static constexpr auto PRINT_QUERY_TYPE=
      enum_query_type(QT_VIEW_INTERNAL | QT_SELECT_ONLY |
                      QT_ITEM_ORIGINAL_FUNC_NULLIF | QT_PARSABLE);
};

#endif
