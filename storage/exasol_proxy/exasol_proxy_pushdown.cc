/* !!! For inclusion into ha_exasol_proxy.cc */

#include "exasol_proxy_pushdown.h"
#include "exasol_proxy_sql_generator.h"
#include <cstring>

namespace
{

void set_query_from_generated_sql(String *query,
                                  std::string *query_generation_error,
                                  const exasol_proxy::SqlGenerationResult &generated)
{
  if (!generated.supported())
  {
    *query_generation_error= generated.unsupported_reason;
    return;
  }
  query_generation_error->clear();
  query->length(0);
  query->append(generated.sql.c_str(), generated.sql.size());
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
  if (!query_generation_error.empty())
  {
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR,
             query_generation_error.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }

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
  auto generated= exasol_proxy::generate_exasol_sql(thd_arg, derived_arg->derived);
  set_query_from_generated_sql(&query, &query_generation_error, generated);
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
  auto generated= exasol_proxy::generate_exasol_sql(thd_arg, lex_unit);
  set_query_from_generated_sql(&query, &query_generation_error, generated);
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
    auto generated= exasol_proxy::generate_exasol_sql(thd_arg, sel_lex->master_unit());
    set_query_from_generated_sql(&stage_query, &query_generation_error, generated);

    auto order_by= exasol_proxy::generate_exasol_order_sql(thd_arg, sel_lex->order_list.first);
    set_query_from_generated_sql(&staged_order_by, &query_generation_error, order_by);
    return;
  }

  auto generated= get_pushdown_type() == select_pushdown_type::SINGLE_SELECT ?
      exasol_proxy::generate_exasol_sql(thd_arg, sel_lex->master_unit()) :
      exasol_proxy::generate_exasol_sql(thd_arg, sel_lex);
  set_query_from_generated_sql(&query, &query_generation_error, generated);
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
