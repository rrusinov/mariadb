#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql/plugin.h>

#include "handler.h"
#include "field.h"
#include "table.h"
#include "sql_class.h"
#include "sql_lex.h"

#include "exasol_gw_pushdown.h"
#include "exasol_gw_sql_generator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <sstream>

extern handlerton *exasol_gw_hton;

namespace
{

void copy_error(char *buffer, unsigned long buffer_size, const char *message) noexcept
{
  if (!buffer || buffer_size == 0)
    return;
  const char *text= message ? message : "unknown error";
  const std::size_t size= std::min<std::size_t>(buffer_size - 1U, std::strlen(text));
  std::memcpy(buffer, text, size);
  buffer[size]= '\0';
}

int copy_current_exception(char *buffer,
                           unsigned long buffer_size,
                           const char *operation) noexcept
{
  try
  {
    throw;
  }
  catch (const std::bad_alloc &)
  {
    char message[256];
    std::snprintf(message, sizeof(message), "%s: out of memory", operation);
    copy_error(buffer, buffer_size, message);
    return HA_ERR_OUT_OF_MEM;
  }
  catch (const std::exception &ex)
  {
    char message[512];
    std::snprintf(message, sizeof(message), "%s: %.400s", operation, ex.what());
    copy_error(buffer, buffer_size, message);
    return HA_ERR_INTERNAL_ERROR;
  }
  catch (...)
  {
    char message[256];
    std::snprintf(message, sizeof(message), "%s: unknown C++ exception", operation);
    copy_error(buffer, buffer_size, message);
    return HA_ERR_INTERNAL_ERROR;
  }
}

int report_pushdown_exception(const char *operation) noexcept
{
  char message[512]= {0};
  const int rc= copy_current_exception(message, sizeof(message), operation);
  my_error(ER_GET_ERRNO, MYF(0), rc, message);
  return rc;
}

class DbugWriteSetGuard
{
 public:
  explicit DbugWriteSetGuard(TABLE *table_arg)
    : table(table_arg), saved(dbug_tmp_use_all_columns(table, &table->write_set))
  {}

  ~DbugWriteSetGuard()
  {
    dbug_tmp_restore_column_map(&table->write_set, saved);
  }

 private:
  TABLE *table;
  MY_BITMAP *saved;
};

std::string apply_decimal_scale(const std::string &integer_value, uint scale)
{
  if (scale == 0 || integer_value.empty())
    return integer_value;
  bool negative= integer_value[0] == '-';
  std::string digits= negative ? integer_value.substr(1) : integer_value;
  while (digits.size() <= scale)
    digits.insert(digits.begin(), '0');
  digits.insert(digits.end() - static_cast<std::ptrdiff_t>(scale), '.');
  return negative ? "-" + digits : digits;
}

std::string cell_value_for_field(Field *field, const exasol_gw::ArrowCell &cell)
{
  if (field->type() == MYSQL_TYPE_DECIMAL || field->type() == MYSQL_TYPE_NEWDECIMAL)
    return apply_decimal_scale(cell.value, field->decimals());
  return cell.value;
}

void set_query_from_generated_sql(String *query,
                                  std::string *query_generation_error,
                                  const exasol_gw::SqlGenerationResult &generated)
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

std::string lex_string_to_std(const LEX_CSTRING &value)
{
  return value.str ? std::string(value.str, value.length) : std::string();
}

std::string table_schema_name(TABLE *table_arg)
{
  if (table_arg && table_arg->s && table_arg->s->db.str)
    return lex_string_to_std(table_arg->s->db);
  return std::string();
}

std::string table_object_name(TABLE *table_arg)
{
  if (table_arg && table_arg->s && table_arg->s->table_name.str)
    return lex_string_to_std(table_arg->s->table_name);
  return std::string();
}

std::string field_name(Field *field)
{
  if (!field || !field->field_name.str)
    return std::string();
  return std::string(field->field_name.str, field->field_name.length);
}

exasol_gw::ArrowColumnKind kind_for_field(Field *field)
{
  if (!field)
    return exasol_gw::ArrowColumnKind::utf8;
  switch (field->type())
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_YEAR:
    return exasol_gw::ArrowColumnKind::signed_int64;
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    return exasol_gw::ArrowColumnKind::float64;
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
    return exasol_gw::ArrowColumnKind::date32;
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    return exasol_gw::ArrowColumnKind::timestamp_ns;
  case MYSQL_TYPE_BIT:
    return exasol_gw::ArrowColumnKind::boolean;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
    return exasol_gw::ArrowColumnKind::decimal128_as_integer_string;
  default:
    return exasol_gw::ArrowColumnKind::utf8;
  }
}

} // namespace

ha_exasol_gw_cursor::ha_exasol_gw_cursor(THD *thd_arg)
  : session(&exasol_gw::session_for_thd(thd_arg)),
    connection(nullptr),
    options(exasol_gw::options_from_environment()),
    cursor_id(0),
    cursor_registered(false),
    current_row(0),
    end_of_cursor(false)
{
  DBUG_EXECUTE_IF("exasol_gw_cursor_constructor_oom", throw std::bad_alloc(););
}

ha_exasol_gw_cursor::~ha_exasol_gw_cursor()
{
  char ignored[1]= {0};
  (void) close(ignored, sizeof(ignored));
}

void ha_exasol_gw_cursor::initialize_column_kinds(TABLE *table_arg)
{
  column_kinds.clear();
  selected_field_indices.clear();
  if (!table_arg)
    return;
  std::size_t field_index= 0;
  for (Field **field= table_arg->field; *field; ++field, ++field_index)
  {
    column_kinds.push_back(kind_for_field(*field));
    selected_field_indices.push_back(field_index);
  }
}

std::vector<std::string> ha_exasol_gw_cursor::initialize_table_scan_columns(TABLE *table_arg)
{
  column_kinds.clear();
  selected_field_indices.clear();
  std::vector<std::string> columns;
  if (!table_arg)
    return columns;
  bool full_row_required= false;
  if (table_arg->write_set)
  {
    for (Field **field= table_arg->field; *field; ++field)
    {
      if (bitmap_is_set(table_arg->write_set, (*field)->field_index))
      {
        full_row_required= true;
        break;
      }
    }
  }
  std::size_t field_index= 0;
  for (Field **field= table_arg->field; *field; ++field, ++field_index)
  {
    if (!full_row_required && table_arg->read_set &&
        !bitmap_is_set(table_arg->read_set, (*field)->field_index))
      continue;
    columns.push_back(field_name(*field));
    column_kinds.push_back(kind_for_field(*field));
    selected_field_indices.push_back(field_index);
  }
  if (columns.empty() && table_arg->field[0])
  {
    columns.push_back(field_name(table_arg->field[0]));
    column_kinds.push_back(kind_for_field(table_arg->field[0]));
    selected_field_indices.push_back(0);
  }
  return columns;
}

int ha_exasol_gw_cursor::open_pushed_query(TABLE *table_arg,
                                              const char *query_text,
                                              char *error_buffer,
                                              unsigned long error_buffer_size)
{
  try
  {
    connection= &session->connection();
    initialize_column_kinds(table_arg);
    exasol_gw::SessionGwOpenCursorResult opened=
        connection->open_pushed_query(query_text ? std::string(query_text) : std::string());
    cursor_id= opened.cursor_id;
    session->read_cursor_opened();
    cursor_registered= true;
    current_batch= exasol_gw::ArrowRowBatch();
    current_row= 0;
    end_of_cursor= false;
    return 0;
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "opening EXASOL pushed-query cursor");
  }
}

int ha_exasol_gw_cursor::open_table_scan(TABLE *table_arg,
                                            char *error_buffer,
                                            unsigned long error_buffer_size,
                                            bool include_row_handles)
{
  try
  {
    connection= &session->connection();
    const std::vector<std::string> columns= initialize_table_scan_columns(table_arg);
    exasol_gw::SessionGwOpenCursorResult opened=
        connection->open_table_scan(table_schema_name(table_arg), table_object_name(table_arg), columns, include_row_handles);
    cursor_id= opened.cursor_id;
    session->read_cursor_opened();
    cursor_registered= true;
    current_batch= exasol_gw::ArrowRowBatch();
    current_row= 0;
    end_of_cursor= false;
    return 0;
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "opening EXASOL table-scan cursor");
  }
}

int ha_exasol_gw_cursor::open_table_scan_by_row_handle(TABLE *table_arg,
                                                        const exasol_gw::SessionGwRowHandle &row_handle,
                                                        char *error_buffer,
                                                        unsigned long error_buffer_size)
{
  try
  {
    connection= &session->connection();
    const std::vector<std::string> columns= initialize_table_scan_columns(table_arg);
    exasol_gw::SessionGwOpenCursorResult opened=
        connection->open_table_scan(table_schema_name(table_arg),
                                   table_object_name(table_arg),
                                   columns,
                                   false,
                                   {row_handle});
    cursor_id= opened.cursor_id;
    session->read_cursor_opened();
    cursor_registered= true;
    current_batch= exasol_gw::ArrowRowBatch();
    current_row= 0;
    end_of_cursor= false;
    return 0;
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "opening EXASOL positioned cursor");
  }
}

int ha_exasol_gw_cursor::fetch_next_batch(char *error_buffer, unsigned long error_buffer_size)
{
  try
  {
    while (!end_of_cursor)
    {
      exasol_gw::SessionGwFetchResult fetched= connection->fetch(cursor_id, options.fetch_rows, 0);
      end_of_cursor= fetched.end_of_cursor;
      current_batch= exasol_gw::decode_arrow_record_batch(fetched.arrow_batch, column_kinds);
      current_row_handles= fetched.row_handles;
      current_row= 0;
      if (current_batch.rows > 0)
        return 0;
      if (end_of_cursor)
        return HA_ERR_END_OF_FILE;
    }
    return HA_ERR_END_OF_FILE;
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "fetching EXASOL cursor batch");
  }
}

int ha_exasol_gw_cursor::materialize_current_row(TABLE *table_arg,
                                                    unsigned char *,
                                                    char *error_buffer,
                                                    unsigned long error_buffer_size)
{
  if (!table_arg || current_row >= current_batch.rows)
    return HA_ERR_END_OF_FILE;
  try
  {
    DbugWriteSetGuard write_set_guard(table_arg);
    if (current_batch.columns.size() != selected_field_indices.size())
      throw std::runtime_error("SessionGW Arrow column count does not match projected MariaDB fields");
    for (std::size_t column= 0; column < selected_field_indices.size(); ++column)
    {
      Field *field= table_arg->field[selected_field_indices[column]];
      const exasol_gw::ArrowCell &cell= current_batch.columns[column][current_row];
      if (cell.is_null)
      {
        field->set_null();
      }
      else
      {
        field->set_notnull();
        const std::string value= cell_value_for_field(field, cell);
        field->store(value.data(), value.size(), &my_charset_bin);
      }
    }
    if (current_row < current_row_handles.size())
      last_row_handle_= current_row_handles[current_row];
    ++current_row;
    return 0;
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "materializing EXASOL cursor row");
  }
}

int ha_exasol_gw_cursor::fetch_row(TABLE *table_arg,
                                      unsigned char *record,
                                      char *error_buffer,
                                      unsigned long error_buffer_size)
{
  if (current_row >= current_batch.rows)
  {
    const int rc= fetch_next_batch(error_buffer, error_buffer_size);
    if (rc != 0)
      return rc;
  }
  return materialize_current_row(table_arg, record, error_buffer, error_buffer_size);
}

int ha_exasol_gw_cursor::close(char *error_buffer, unsigned long error_buffer_size)
{
  try
  {
    if (cursor_id != 0)
    {
      connection->close_cursor(cursor_id);
      cursor_id= 0;
    }
    if (cursor_registered)
    {
      cursor_registered= false;
      session->read_cursor_closed();
    }
    return 0;
  }
  catch (...)
  {
    cursor_id= 0;
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "closing EXASOL cursor");
  }
}

int ha_exasol_gw_pushdown_handler_base::init_scan_(THD *thd_arg,
                                                      TABLE *table_arg,
                                                      const char *query_text,
                                                      bool)
{
  try
  {
    if (!query_generation_error.empty())
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR,
               query_generation_error.c_str());
      return HA_ERR_INTERNAL_ERROR;
    }

    const int validation_rc= validate_exasol_gw_table_metadata(query_table);
    if (validation_rc != 0)
      return validation_rc;

    cursor= new ha_exasol_gw_cursor(thd_arg);
    if (!cursor)
      return HA_ERR_OUT_OF_MEM;
    char error_buffer[512]= {0};
    const int rc= cursor->open_pushed_query(table_arg, query_text, error_buffer, sizeof(error_buffer));
    if (rc != 0)
    {
      delete cursor;
      cursor= nullptr;
      my_error(ER_GET_ERRNO, MYF(0), rc,
               error_buffer[0] ? error_buffer : "failed to open EXASOL SessionGW pushed query");
    }
    return rc;
  }
  catch (...)
  {
    delete cursor;
    cursor= nullptr;
    return report_pushdown_exception("starting EXASOL pushed-query scan");
  }
}

int ha_exasol_gw_pushdown_handler_base::next_row_(TABLE *table_arg)
{
  try
  {
    if (!cursor)
      return HA_ERR_END_OF_FILE;

    char error_buffer[512]= {0};
    const int rc= cursor->fetch_row(table_arg, table_arg->record[0], error_buffer, sizeof(error_buffer));
    if (rc != 0 && rc != HA_ERR_END_OF_FILE)
    {
      my_error(ER_GET_ERRNO, MYF(0), rc,
               error_buffer[0] ? error_buffer : "failed to fetch EXASOL SessionGW row");
    }
    return rc;
  }
  catch (...)
  {
    return report_pushdown_exception("fetching EXASOL pushed-query row");
  }
}

int ha_exasol_gw_pushdown_handler_base::end_scan_()
{
  try
  {
    if (!cursor)
      return 0;
    char error_buffer[512]= {0};
    const int rc= cursor->close(error_buffer, sizeof(error_buffer));
    delete cursor;
    cursor= nullptr;
    if (rc != 0)
    {
      my_error(ER_GET_ERRNO, MYF(0), rc,
               error_buffer[0] ? error_buffer : "failed to close EXASOL SessionGW cursor");
      return rc;
    }
    return 0;
  }
  catch (...)
  {
    delete cursor;
    cursor= nullptr;
    return report_pushdown_exception("ending EXASOL pushed-query scan");
  }
}

ha_exasol_gw_derived_handler::ha_exasol_gw_derived_handler(THD *thd_arg,
                                                                 TABLE_LIST *derived_arg,
                                                                 TABLE *tbl_arg)
  : derived_handler(thd_arg, exasol_gw_hton),
    ha_exasol_gw_pushdown_handler_base(tbl_arg),
    query(thd_arg->charset())
{
  derived= derived_arg;
  query.length(0);
  auto generated= exasol_gw::generate_exasol_sql(thd_arg, derived_arg->derived);
  set_query_from_generated_sql(&query, &query_generation_error, generated);
}

ha_exasol_gw_derived_handler::~ha_exasol_gw_derived_handler()= default;

ha_exasol_gw_select_handler::ha_exasol_gw_select_handler(
    THD *thd_arg, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd_arg, exasol_gw_hton, lex_unit),
    ha_exasol_gw_pushdown_handler_base(tbl),
    query(thd_arg->charset()),
    stage_query(thd_arg->charset()),
    staged_order_by(thd_arg->charset()),
    uses_staged_distinct_pushdown(false)
{
  query.length(0);
  stage_query.length(0);
  staged_order_by.length(0);
  auto generated= exasol_gw::generate_exasol_sql(thd_arg, lex_unit);
  set_query_from_generated_sql(&query, &query_generation_error, generated);
}

ha_exasol_gw_select_handler::ha_exasol_gw_select_handler(
    THD *thd_arg, SELECT_LEX *sel_lex, SELECT_LEX_UNIT *lex_unit, TABLE *tbl)
  : select_handler(thd_arg, exasol_gw_hton, sel_lex, lex_unit),
    ha_exasol_gw_pushdown_handler_base(tbl),
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
    auto generated= exasol_gw::generate_exasol_sql(thd_arg, sel_lex->master_unit());
    set_query_from_generated_sql(&stage_query, &query_generation_error, generated);

    auto order_by= exasol_gw::generate_exasol_order_sql(thd_arg, sel_lex->order_list.first);
    set_query_from_generated_sql(&staged_order_by, &query_generation_error, order_by);
    return;
  }

  auto generated= get_pushdown_type() == select_pushdown_type::SINGLE_SELECT ?
      exasol_gw::generate_exasol_sql(thd_arg, sel_lex->master_unit()) :
      exasol_gw::generate_exasol_sql(thd_arg, sel_lex);
  set_query_from_generated_sql(&query, &query_generation_error, generated);
}

ha_exasol_gw_select_handler::~ha_exasol_gw_select_handler()
{
  (void) end_scan_();
}

int ha_exasol_gw_select_handler::init_scan()
{
  try
  {
    if (uses_staged_distinct_pushdown)
    {
      query.length(0);
      query.append(STRING_WITH_LEN("SELECT DISTINCT * FROM ("));
      query.append(stage_query.ptr(), stage_query.length());
      query.append(STRING_WITH_LEN(") SGW_STAGE"));
      if (staged_order_by.length() > 0)
      {
        query.append(STRING_WITH_LEN(" ORDER BY "));
        query.append(staged_order_by.ptr(), staged_order_by.length());
      }
    }

    return init_scan_(thd, table, query.ptr(), false);
  }
  catch (...)
  {
    return report_pushdown_exception("initializing EXASOL SELECT pushdown");
  }
}

int ha_exasol_gw_select_handler::next_row()
{
  return next_row_(table);
}

int ha_exasol_gw_select_handler::end_scan()
{
  return end_scan_();
}
