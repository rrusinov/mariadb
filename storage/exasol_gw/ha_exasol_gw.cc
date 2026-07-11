#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql/plugin.h>
#include <thr_lock.h>

#include "handler.h"
#include "table.h"
#include "sql_alter.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_select.h"

#include "exasol_gw_pushdown.h"
#include "exasol_native_write_batch.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <new>
#include <string>
#include <thread>
#include <utility>
#include <vector>

static select_handler *create_exasol_gw_select_handler(THD *thd,
                                                          SELECT_LEX *sel_lex,
                                                          SELECT_LEX_UNIT *lex_unit);
static select_handler *create_exasol_gw_unit_handler(THD *thd,
                                                        SELECT_LEX_UNIT *lex_unit);
static derived_handler *create_exasol_gw_derived_handler(THD *thd,
                                                            TABLE_LIST *derived);

handlerton *exasol_gw_hton= nullptr;

namespace
{
struct InsertContext;
struct UpdateContext;
struct DeleteContext;
exasol_gw::SessionGwRowHandle row_handle_from_ref(const uchar *ref);
std::string table_schema_name(TABLE *table);
std::string quote_exasol_identifier(const std::string &identifier);
bool build_create_table_sql(TABLE *form, HA_CREATE_INFO *create_info,
                            std::string *sql, std::string *error);
std::string drop_table_sql_from_path(const char *from);

int report_handler_exception(const char *operation) noexcept
{
  try
  {
    throw;
  }
  catch (const std::bad_alloc &)
  {
    char message[256];
    std::snprintf(message, sizeof(message), "%s: out of memory", operation);
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_OUT_OF_MEM, message);
    return HA_ERR_OUT_OF_MEM;
  }
  catch (const std::exception &ex)
  {
    char message[512];
    std::snprintf(message, sizeof(message), "%s: %.400s", operation, ex.what());
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, message);
    return HA_ERR_INTERNAL_ERROR;
  }
  catch (...)
  {
    char message[256];
    std::snprintf(message, sizeof(message), "%s: unknown C++ exception", operation);
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, message);
    return HA_ERR_INTERNAL_ERROR;
  }
}
}

class Exasol_gw_share: public Handler_share
{
public:
  mysql_mutex_t mutex;
  THR_LOCK lock;

  Exasol_gw_share()
  {
    mysql_mutex_init(0, &mutex, MY_MUTEX_INIT_FAST);
    thr_lock_init(&lock);
  }

  ~Exasol_gw_share() override
  {
    thr_lock_delete(&lock);
    mysql_mutex_destroy(&mutex);
  }
};

class ha_exasol_gw: public handler
{
public:
  ha_exasol_gw(handlerton *hton, TABLE_SHARE *table_arg)
    : handler(hton, table_arg), share(nullptr), cursor(nullptr),
      insert_context(nullptr), update_context(nullptr), delete_context(nullptr),
      have_positioned_row_handle(false)
  {
    ref_length= sizeof(std::uint64_t);
  }

  ~ha_exasol_gw() override;

  const char *index_type(uint) override { return "NONE"; }

  ulonglong table_flags() const override
  {
    return HA_BINLOG_STMT_CAPABLE | HA_REC_NOT_IN_SEQ | HA_NULL_IN_KEY | HA_NO_TRANSACTIONS;
  }

  ulong index_flags(uint, uint, bool) const override { return 0; }
  uint max_supported_record_length() const override { return HA_MAX_REC_LENGTH; }
  uint max_supported_keys() const override { return 0; }
  uint max_supported_key_parts() const override { return 0; }
  uint max_supported_key_length() const override { return 0; }

  IO_AND_CPU_COST scan_time() override
  {
    IO_AND_CPU_COST cost;
    cost.io= static_cast<double>(stats.records + stats.deleted) * DISK_READ_COST;
    cost.cpu= 0;
    return cost;
  }

  IO_AND_CPU_COST rnd_pos_time(ha_rows rows) override
  {
    IO_AND_CPU_COST cost;
    cost.io= 0;
    cost.cpu= static_cast<double>(rows) * DISK_READ_COST;
    return cost;
  }

  int open(const char *, int, uint) override
  {
    try
    {
      share= get_share();
      if (!share)
        return HA_ERR_OUT_OF_MEM;
      thr_lock_data_init(&share->lock, &lock, nullptr);
      return 0;
    }
    catch (...)
    {
      return report_handler_exception("opening EXASOL handler");
    }
  }

  int close(void) override
  {
    try
    {
      return rnd_end();
    }
    catch (...)
    {
      return report_handler_exception("closing EXASOL handler");
    }
  }

  int create(const char *, TABLE *form, HA_CREATE_INFO *create_info) override
  {
    try
    {
      const int sql_command= thd_sql_command(ha_thd());
      if (sql_command == SQLCOM_ALTER_TABLE || sql_command == SQLCOM_TRUNCATE)
      {
        my_error(ER_GET_ERRNO, MYF(0), HA_ERR_UNSUPPORTED,
                 "ALTER and TRUNCATE are not supported for EXASOL tables");
        return HA_ERR_UNSUPPORTED;
      }

      std::string table_sql;
      std::string error;
      if (!build_create_table_sql(form, create_info, &table_sql, &error))
      {
        my_error(ER_GET_ERRNO, MYF(0), HA_ERR_UNSUPPORTED, error.c_str());
        return HA_ERR_UNSUPPORTED;
      }

      exasol_gw::SessionGwOptions options= exasol_gw::options_from_environment();
      exasol_gw::execute_sql(options,
                             "CREATE SCHEMA IF NOT EXISTS " +
                                 quote_exasol_identifier(table_schema_name(form)));
      exasol_gw::execute_sql(options, table_sql);
      return 0;
    }
    catch (...)
    {
      return report_handler_exception("creating EXASOL table");
    }
  }

  int delete_table(const char *from) override
  {
    try
    {
      exasol_gw::execute_sql(exasol_gw::options_from_environment(), drop_table_sql_from_path(from));
      return 0;
    }
    catch (...)
    {
      return report_handler_exception("dropping EXASOL table");
    }
  }

  int truncate() override
  {
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_UNSUPPORTED,
             "TRUNCATE TABLE is not supported for EXASOL tables");
    return HA_ERR_UNSUPPORTED;
  }

  int rename_table(const char *, const char *) override
  {
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_UNSUPPORTED,
             "RENAME TABLE is not supported for EXASOL tables");
    return HA_ERR_UNSUPPORTED;
  }

  void start_bulk_insert(ha_rows rows, uint flags) override;
  int end_bulk_insert() override;
  int write_row(const uchar *) override;
  int update_row(const uchar *, const uchar *) override;
  int delete_row(const uchar *) override;

  int rnd_init(bool) override
  {
    try
    {
      (void) rnd_end();
      have_positioned_row_handle= false;
      cursor= new ha_exasol_gw_cursor(table->in_use);
      if (!cursor)
        return HA_ERR_OUT_OF_MEM;
      char error_buffer[512]= {0};
      const int rc= cursor->open_table_scan(table, error_buffer, sizeof(error_buffer), true);
      if (rc != 0)
      {
        delete cursor;
        cursor= nullptr;
        my_error(ER_GET_ERRNO, MYF(0), rc,
                 error_buffer[0] ? error_buffer : "failed to open EXASOL SessionGW table scan");
      }
      return rc;
    }
    catch (...)
    {
      delete cursor;
      cursor= nullptr;
      return report_handler_exception("starting EXASOL table scan");
    }
  }

  int rnd_end() override
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
                 error_buffer[0] ? error_buffer : "failed to close EXASOL SessionGW table scan");
      }
      return rc;
    }
    catch (...)
    {
      delete cursor;
      cursor= nullptr;
      return report_handler_exception("ending EXASOL table scan");
    }
  }

  int rnd_next(uchar *buf) override
  {
    try
    {
      if (!cursor)
        return HA_ERR_END_OF_FILE;
      char error_buffer[512]= {0};
      const int rc= cursor->fetch_row(table, buf, error_buffer, sizeof(error_buffer));
      if (rc != 0 && rc != HA_ERR_END_OF_FILE)
      {
        my_error(ER_GET_ERRNO, MYF(0), rc,
                 error_buffer[0] ? error_buffer : "failed to fetch EXASOL SessionGW table row");
      }
      return rc;
    }
    catch (...)
    {
      return report_handler_exception("fetching EXASOL table row");
    }
  }

  int rnd_pos(uchar *buf, uchar *pos) override
  {
    try
    {
      const exasol_gw::SessionGwRowHandle row_handle= row_handle_from_ref(pos);
      ha_exasol_gw_cursor positioned_cursor(table->in_use);
      char error_buffer[512]= {0};
      const int open_rc= positioned_cursor.open_table_scan_by_row_handle(table,
                                                                         row_handle,
                                                                         error_buffer,
                                                                         sizeof(error_buffer));
      if (open_rc != 0)
      {
        my_error(ER_GET_ERRNO, MYF(0), open_rc,
                 error_buffer[0] ? error_buffer : "failed to open EXASOL SessionGW positioned scan");
        return open_rc;
      }
      const int fetch_rc= positioned_cursor.fetch_row(table, buf, error_buffer, sizeof(error_buffer));
      if (fetch_rc != 0)
      {
        if (fetch_rc == HA_ERR_END_OF_FILE)
          return HA_ERR_RECORD_DELETED;
        my_error(ER_GET_ERRNO, MYF(0), fetch_rc,
                 error_buffer[0] ? error_buffer : "failed to fetch EXASOL SessionGW positioned row");
        return fetch_rc;
      }
      last_positioned_row_handle= row_handle;
      have_positioned_row_handle= true;
      return 0;
    }
    catch (...)
    {
      return report_handler_exception("reading positioned EXASOL row");
    }
  }
  void position(const uchar *) override
  {
    if (!cursor || !ref)
      return;
    const exasol_gw::SessionGwRowHandle row_handle= cursor->last_row_handle();
    std::memcpy(ref, &row_handle.row_number, sizeof(row_handle.row_number));
  }

  int info(uint) override
  {
    stats.records= 1000;
    return 0;
  }

  int external_lock(THD *, int lock_type) override;

  enum_alter_inplace_result check_if_supported_inplace_alter(TABLE *, Alter_inplace_info *) override
  {
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_UNSUPPORTED,
             "ALTER TABLE is not supported for EXASOL SessionGW tables");
    return HA_ALTER_ERROR;
  }

  THR_LOCK_DATA **store_lock(THD *, THR_LOCK_DATA **to,
                             enum thr_lock_type lock_type) override
  {
    if (lock_type != TL_IGNORE && lock.type == TL_UNLOCK)
      lock.type= lock_type;
    *to++= &lock;
    return to;
  }

private:
  Exasol_gw_share *get_share()
  {
    lock_shared_ha_data();
    auto *result= static_cast<Exasol_gw_share *>(get_ha_share_ptr());
    if (!result)
    {
      result= new Exasol_gw_share();
      if (result)
        set_ha_share_ptr(static_cast<Handler_share *>(result));
    }
    unlock_shared_ha_data();
    return result;
  }

  int close_insert_context();
  int close_update_context();
  int close_delete_context();
  int close_dml_contexts();

  THR_LOCK_DATA lock;
  Exasol_gw_share *share;
  ha_exasol_gw_cursor *cursor;
  InsertContext *insert_context;
  UpdateContext *update_context;
  DeleteContext *delete_context;
  exasol_gw::SessionGwRowHandle last_positioned_row_handle;
  bool have_positioned_row_handle;
};

namespace
{

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

std::string quote_exasol_identifier(const std::string &identifier)
{
  std::string quoted= "\"";
  for (char ch: identifier)
  {
    if (ch == '"')
      quoted += "\"\"";
    else
      quoted += ch;
  }
  quoted += '"';
  return quoted;
}

enum_field_types declared_field_type(Field *field, Create_field *create_field)
{
  return create_field ? create_field->type_handler()->field_type() : field->real_type();
}

bool declared_field_is_unsigned(Field *field, Create_field *create_field)
{
  return create_field ? (create_field->flags & UNSIGNED_FLAG) != 0 : field->is_unsigned();
}

uint declared_field_char_length(Field *field, Create_field *create_field)
{
  return create_field ? create_field->char_length : field->char_length();
}

CHARSET_INFO *declared_field_charset(Field *field, Create_field *create_field)
{
  return create_field && create_field->charset ? create_field->charset : field->charset();
}

bool append_exasol_type(Field *field, Create_field *create_field,
                        std::string *sql, std::string *error)
{
  const enum_field_types type= declared_field_type(field, create_field);
  const bool is_unsigned= declared_field_is_unsigned(field, create_field);
  switch (type)
  {
  case MYSQL_TYPE_TINY:
    *sql += "DECIMAL(3,0)";
    return true;
  case MYSQL_TYPE_SHORT:
    *sql += is_unsigned ? "DECIMAL(5,0)" : "DECIMAL(9,0)";
    return true;
  case MYSQL_TYPE_INT24:
    *sql += is_unsigned ? "DECIMAL(8,0)" : "DECIMAL(9,0)";
    return true;
  case MYSQL_TYPE_LONG:
    *sql += is_unsigned ? "DECIMAL(10,0)" : "DECIMAL(18,0)";
    return true;
  case MYSQL_TYPE_LONGLONG:
    if (is_unsigned)
    {
      *error= "unsigned BIGINT is not supported for EXASOL field '" + field_name(field) + "'";
      return false;
    }
    *sql += "DECIMAL(18,0)";
    return true;
  case MYSQL_TYPE_YEAR:
    *sql += "DECIMAL(4,0)";
    return true;
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    *sql += "DOUBLE PRECISION";
    return true;
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  {
    const uint scale= field->decimals();
    const uint precision= field->real_type() == MYSQL_TYPE_NEWDECIMAL
                              ? static_cast<Field_new_decimal *>(field)->precision
                              : my_decimal_length_to_precision(field->field_length, scale,
                                                               field->is_unsigned());
    if (precision == 0 || precision > 36 || scale > precision)
    {
      *error= "unsupported DECIMAL precision for EXASOL field '" + field_name(field) + "'";
      return false;
    }
    *sql += "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
    return true;
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
    *sql += "DATE";
    return true;
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    *sql += "TIMESTAMP(" + std::to_string(field->decimals()) + ")";
    return true;
  case MYSQL_TYPE_BIT:
    if (field->field_length == 1)
    {
      *sql += "BOOLEAN";
      return true;
    }
    break;
  case MYSQL_TYPE_STRING:
  case MYSQL_TYPE_VAR_STRING:
  case MYSQL_TYPE_VARCHAR:
  {
    if (declared_field_charset(field, create_field) == &my_charset_bin)
    {
      *error= "binary strings are not supported for EXASOL field '" + field_name(field) + "'";
      return false;
    }
    const uint length= declared_field_char_length(field, create_field);
    if (length == 0 || length > 2000000U)
      break;
    *sql += type == MYSQL_TYPE_STRING ? "CHAR(" : "VARCHAR(";
    *sql += std::to_string(length) + ") UTF8";
    return true;
  }
  default:
    break;
  }
  *error= "unsupported MariaDB type for EXASOL field '" + field_name(field) + "'";
  return false;
}

bool validate_create_options(HA_CREATE_INFO *create_info, std::string *error)
{
  if (!create_info || !create_info->alter_info)
    return true;
  Alter_info *alter_info= create_info->alter_info;
  if (alter_info->key_list.elements != 0 || alter_info->check_constraint_list.elements != 0)
  {
    *error= "indexes and table constraints are not supported for EXASOL tables";
    return false;
  }
  if ((create_info->used_fields & ~(HA_CREATE_USED_ENGINE)) != 0 ||
      create_info->versioned() || create_info->tmp_table() || alter_info->num_parts != 0)
  {
    *error= "the requested MariaDB table options are not supported for EXASOL tables";
    return false;
  }

  List_iterator_fast<Create_field> fields(alter_info->create_list);
  while (Create_field *field= fields++)
  {
    if (field->default_value || field->on_update || field->vcol_info || field->check_constraint ||
        (field->flags & AUTO_INCREMENT_FLAG) != 0 || field->comment.length != 0 ||
        field->option_list || field->invisible != VISIBLE ||
        field->versioning != Column_definition::VERSIONING_NOT_SET || field->period)
    {
      *error= "unsupported clause for EXASOL field '" +
              std::string(field->field_name.str, field->field_name.length) + "'";
      return false;
    }
  }
  return true;
}

bool build_create_table_sql(TABLE *form, HA_CREATE_INFO *create_info,
                            std::string *sql, std::string *error)
{
  if (!form || !sql || !error)
    return false;
  if (!validate_create_options(create_info, error))
    return false;

  const std::string schema= table_schema_name(form);
  const std::string object= table_object_name(form);
  *sql= "CREATE TABLE " + quote_exasol_identifier(schema) + "." +
        quote_exasol_identifier(object) + " (";
  List<Create_field> empty_fields;
  List_iterator_fast<Create_field> create_fields(
      create_info && create_info->alter_info ? create_info->alter_info->create_list : empty_fields);
  const bool have_create_fields= create_info && create_info->alter_info;
  bool first= true;
  for (Field **field= form->field; *field; ++field)
  {
    Create_field *create_field= have_create_fields ? create_fields++ : nullptr;
    if (create_field && field_name(*field) !=
                            std::string(create_field->field_name.str, create_field->field_name.length))
    {
      *error= "MariaDB field metadata does not match the EXASOL table definition";
      return false;
    }
    if (!first)
      *sql += ", ";
    first= false;
    *sql += quote_exasol_identifier(field_name(*field)) + " ";
    if (!append_exasol_type(*field, create_field, sql, error))
      return false;
    *sql += ((*field)->flags & NOT_NULL_FLAG) != 0 ? " NOT NULL" : " NULL";
  }
  *sql += ")";
  return true;
}

std::string drop_table_sql_from_path(const char *from)
{
  std::string path= from ? std::string(from) : std::string();
  for (char &ch: path)
  {
    if (ch == '\\')
      ch= '/';
  }
  const std::size_t slash= path.find_last_of('/');
  const std::string object= slash == std::string::npos ? path : path.substr(slash + 1);
  std::string schema;
  if (slash != std::string::npos)
  {
    const std::size_t prev= path.find_last_of('/', slash == 0 ? 0 : slash - 1);
    schema= prev == std::string::npos ? path.substr(0, slash) : path.substr(prev + 1, slash - prev - 1);
  }
  return "DROP TABLE IF EXISTS " + quote_exasol_identifier(schema) + "." + quote_exasol_identifier(object);
}

void append_fixed_value(std::vector<std::uint8_t> &out, const void *value, std::size_t size)
{
  const auto *bytes= static_cast<const std::uint8_t *>(value);
  out.insert(out.end(), bytes, bytes + size);
}

struct ExasolNativeTimestamp
{
  std::uint32_t nanosecond;
  std::uint32_t seconds_since_midnight;
  std::uint32_t date;
  std::uint32_t padding;
};

std::uint32_t native_date_value(const std::uint32_t year,
                                const std::uint32_t month,
                                const std::uint32_t day)
{
  return (year << 16U) | (month << 8U) | day;
}

std::uint32_t parse_native_date(Field *field)
{
  const longlong value= field->val_int();
  const std::uint32_t day= static_cast<std::uint32_t>(value % 100);
  const std::uint32_t month= static_cast<std::uint32_t>((value / 100) % 100);
  const std::uint32_t year= static_cast<std::uint32_t>(value / 10000);
  return native_date_value(year, month, day);
}

ExasolNativeTimestamp parse_native_timestamp(Field *field)
{
  const longlong value= field->val_int();
  const std::uint32_t second= static_cast<std::uint32_t>(value % 100);
  const std::uint32_t minute= static_cast<std::uint32_t>((value / 100) % 100);
  const std::uint32_t hour= static_cast<std::uint32_t>((value / 10000) % 100);
  const std::uint32_t day= static_cast<std::uint32_t>((value / 1000000) % 100);
  const std::uint32_t month= static_cast<std::uint32_t>((value / 100000000) % 100);
  const std::uint32_t year= static_cast<std::uint32_t>(value / 10000000000LL);
  return {0U, hour * 3600U + minute * 60U + second, native_date_value(year, month, day), 0U};
}

__int128_t parse_scaled_decimal(Field *field)
{
  StringBuffer<128> value_buffer;
  String *value= field->val_str(&value_buffer);
  if (!value)
    return 0;
  const uint scale= field->decimals();
  __int128_t scaled= 0;
  bool negative= false;
  uint fractional_digits= 0;
  bool fractional= false;
  const char *ptr= value->ptr();
  const char *end= ptr + value->length();
  if (ptr != end && *ptr == '-')
  {
    negative= true;
    ++ptr;
  }
  for (; ptr != end; ++ptr)
  {
    if (*ptr == '.')
    {
      fractional= true;
      continue;
    }
    if (*ptr < '0' || *ptr > '9')
      continue;
    if (fractional && fractional_digits >= scale)
      continue;
    scaled= scaled * 10 + static_cast<int>(*ptr - '0');
    if (fractional)
      ++fractional_digits;
  }
  while (fractional_digits < scale)
  {
    scaled *= 10;
    ++fractional_digits;
  }
  return negative ? -scaled : scaled;
}

exasol_gw::SessionGwRowHandle row_handle_from_ref(const uchar *ref)
{
  exasol_gw::SessionGwRowHandle row_handle;
  if (ref)
  {
    std::memcpy(&row_handle.row_number, ref, sizeof(row_handle.row_number));
  }
  return row_handle;
}

bool native_field_is_variable(Field *field)
{
  switch (field->type())
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_YEAR:
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
  case MYSQL_TYPE_BIT:
    return false;
  default:
    return true;
  }
}

struct NativeColumnBuffer
{
  bool variable= false;
  std::vector<std::uint8_t> nulls;
  std::vector<std::uint8_t> fixed;
  std::vector<std::size_t> sizes;
  std::vector<std::uint8_t> variable_data;
};

bool append_field_to_column_buffer(NativeColumnBuffer &column, Field *field)
{
  column.nulls.push_back(field->is_null() ? exasol_gw::native_write_null : exasol_gw::native_write_not_null);
  switch (field->type())
  {
  case MYSQL_TYPE_TINY:
  case MYSQL_TYPE_SHORT:
  case MYSQL_TYPE_LONG:
  case MYSQL_TYPE_INT24:
  case MYSQL_TYPE_LONGLONG:
  case MYSQL_TYPE_YEAR:
  {
    const std::int64_t value= field->is_null() ? 0 : static_cast<std::int64_t>(field->val_int());
    append_fixed_value(column.fixed, &value, sizeof(value));
    return true;
  }
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  {
    const double value= field->is_null() ? 0.0 : field->val_real();
    append_fixed_value(column.fixed, &value, sizeof(value));
    return true;
  }
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  {
    const __int128_t scaled= field->is_null() ? 0 : parse_scaled_decimal(field);
    if (field->field_length <= 18)
    {
      const std::int64_t value= static_cast<std::int64_t>(scaled);
      append_fixed_value(column.fixed, &value, sizeof(value));
    }
    else
    {
      append_fixed_value(column.fixed, &scaled, sizeof(scaled));
    }
    return true;
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
  {
    const std::uint32_t value= field->is_null() ? 0U : parse_native_date(field);
    append_fixed_value(column.fixed, &value, sizeof(value));
    return true;
  }
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
  {
    const ExasolNativeTimestamp value= field->is_null() ? ExasolNativeTimestamp{0U, 0U, 0U, 0U} : parse_native_timestamp(field);
    append_fixed_value(column.fixed, &value, sizeof(value));
    return true;
  }
  case MYSQL_TYPE_BIT:
  {
    const std::uint8_t value= field->is_null() ? 0 : static_cast<std::uint8_t>(field->val_int() ? 0xffU : 0x00U);
    append_fixed_value(column.fixed, &value, sizeof(value));
    return true;
  }
  default:
  {
    StringBuffer<512> value_buffer;
    if (!field->is_null())
    {
      String *value= field->val_str(&value_buffer);
      if (!value)
        return false;
      column.sizes.push_back(value->length());
      column.variable_data.insert(column.variable_data.end(),
                                  reinterpret_cast<const std::uint8_t *>(value->ptr()),
                                  reinterpret_cast<const std::uint8_t *>(value->ptr()) + value->length());
    }
    else
    {
      column.sizes.push_back(0);
    }
    return true;
  }
  }
}

bool build_native_batch_from_columns(const std::vector<NativeColumnBuffer> &columns,
                                     std::uint32_t row_count,
                                     std::vector<std::uint8_t> *batch)
{
  batch->clear();
  exasol_gw::NativeWriteBatchBuilder builder(*batch);
  builder.begin(row_count, static_cast<std::uint32_t>(columns.size()));
  for (const NativeColumnBuffer &column: columns)
  {
    if (column.variable)
      builder.append_variable_column(column.nulls, column.sizes, column.variable_data);
    else
      builder.append_fixed_column(column.nulls, column.fixed);
  }
  builder.finish();
  return true;
}

std::vector<std::string> table_column_names(TABLE *table)
{
  std::vector<std::string> columns;
  for (Field **field= table->field; *field; ++field)
    columns.push_back(field_name(*field));
  return columns;
}

std::uint32_t batch_rows_from_environment(const char *name)
{
  const char *value= std::getenv(name);
  if (!value || !*value)
    return 10000;
  const unsigned long parsed= std::strtoul(value, nullptr, 10);
  if (parsed == 0)
    return 10000;
  return static_cast<std::uint32_t>(std::min<unsigned long>(parsed, 1000000UL));
}

std::uint32_t insert_batch_rows_from_environment()
{
  return batch_rows_from_environment("EXASOL_SESSIONGW_INSERT_BATCH_ROWS");
}

std::uint32_t update_batch_rows_from_environment()
{
  return batch_rows_from_environment("EXASOL_SESSIONGW_UPDATE_BATCH_ROWS");
}

std::uint32_t delete_batch_rows_from_environment()
{
  return batch_rows_from_environment("EXASOL_SESSIONGW_DELETE_BATCH_ROWS");
}

bool is_retryable_insert_failure(const std::string &message)
{
  return message.find("Transaction collision") != std::string::npos ||
         message.find("GlobalTransactionRollback") != std::string::npos ||
         message.find("unexpected eof") != std::string::npos ||
         message.find("socket closed") != std::string::npos ||
         message.find("WebSocket close received") != std::string::npos;
}

class DbugReadSetGuard
{
 public:
  explicit DbugReadSetGuard(TABLE *table_arg)
    : table(table_arg), saved(dbug_tmp_use_all_columns(table, &table->read_set))
  {}

  ~DbugReadSetGuard()
  {
    dbug_tmp_restore_column_map(&table->read_set, saved);
  }

 private:
  TABLE *table;
  MY_BITMAP *saved;
};

struct InsertContext
{
  InsertContext(std::uint32_t max_rows, THD *thd)
    : session(&exasol_gw::session_for_thd(thd)), connection(nullptr),
      max_rows_per_batch(max_rows)
  {
    DBUG_EXECUTE_IF("exasol_gw_insert_context_constructor_oom", throw std::bad_alloc(););
  }

  int append(TABLE *table)
  {
    try
    {
      DbugReadSetGuard read_set_guard(table);
      ensure_operation_open(table);
      if (pending_columns.empty())
      {
        for (Field **field= table->field; *field; ++field)
        {
          NativeColumnBuffer column;
          column.variable= native_field_is_variable(*field);
          pending_columns.push_back(std::move(column));
        }
      }
      std::size_t column_index= 0;
      for (Field **field= table->field; *field; ++field, ++column_index)
      {
        if (!append_field_to_column_buffer(pending_columns[column_index], *field))
        {
          abort();
          return HA_ERR_UNSUPPORTED;
        }
      }
      ++pending_rows;
      if (pending_rows >= max_rows_per_batch)
        return flush(table);
      return 0;
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int close(TABLE *table)
  {
    if (failed)
      return 0;
    try
    {
      const int rc= flush(table);
      if (rc != 0)
        return rc;
      return close_operation();
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_open(TABLE *table)
  {
    if (initialized)
      return;
    schema= table_schema_name(table);
    object= table_object_name(table);
    columns= table_column_names(table);
    described= connection->describe_table(schema, object);
    initialized= true;
  }

  int flush(TABLE *table)
  {
    if (failed || pending_rows == 0)
      return 0;
    std::vector<std::uint8_t> batch;
    if (!build_native_batch_from_columns(pending_columns, pending_rows, &batch))
    {
      abort();
      return HA_ERR_UNSUPPORTED;
    }

    try
    {
      ensure_operation_open(table);
      const std::uint64_t affected= connection->insert_rows(operation_id, pending_rows, batch);
      if (affected != pending_rows)
      {
        abort();
        return HA_ERR_INTERNAL_ERROR;
      }
      ++successful_batches;
      reset_pending();
      return 0;
    }
    catch (const std::exception &ex)
    {
      const std::string error= ex.what();
      if (successful_batches == 0 && is_retryable_insert_failure(error))
        return retry_pending_first_batch(table, batch, pending_rows, error);
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, error.c_str());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int retry_pending_first_batch(TABLE *table,
                                const std::vector<std::uint8_t> &batch,
                                std::uint32_t rows,
                                std::string last_error)
  {
    reset_operation_after_error();
    constexpr int max_attempts= 8;
    for (int attempt= 1; attempt < max_attempts; ++attempt)
    {
      std::this_thread::sleep_for(std::chrono::milliseconds(25 * attempt));
      try
      {
        ensure_operation_open(table);
        const std::uint64_t affected= connection->insert_rows(operation_id, rows, batch);
        if (affected != rows)
        {
          abort();
          return HA_ERR_INTERNAL_ERROR;
        }
        ++successful_batches;
        reset_pending();
        return 0;
      }
      catch (const std::exception &ex)
      {
        last_error= ex.what();
        reset_operation_after_error();
        if (!is_retryable_insert_failure(last_error))
          break;
      }
    }
    abort();
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, last_error.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }

  void ensure_operation_open(TABLE *table)
  {
    if (!connection)
      connection= &session->connection();
    ensure_open(table);
    if (operation_open)
      return;
    const exasol_gw::SessionGwOpenOperationResult opened=
        connection->open_table_insert(schema, object, columns, max_rows_per_batch, described.arrow_schema);
    operation_id= opened.operation_id;
    operation_open= true;
    session->operation_opened();
  }

  int close_operation()
  {
    if (!operation_open)
      return 0;
    try
    {
      connection->close_operation(operation_id);
      operation_open= false;
      operation_id= 0;
      successful_batches= 0;
      session->operation_closed();
      return 0;
    }
    catch (...)
    {
      abort();
      throw;
    }
  }

  void reset_operation_after_error()
  {
    session->reset();
    connection= nullptr;
    operation_open= false;
    operation_id= 0;
  }

  void abort()
  {
    if (failed)
      return;
    session->reset();
    connection= nullptr;
    operation_open= false;
    operation_id= 0;
    successful_batches= 0;
    reset_pending();
    failed= true;
  }

  void reset_pending()
  {
    pending_columns.clear();
    pending_rows= 0;
  }

  exasol_gw::SessionGwThdContext *session;
  exasol_gw::SessionGwConnection *connection;
  bool initialized= false;
  bool operation_open= false;
  bool failed= false;
  std::uint64_t operation_id= 0;
  std::string schema;
  std::string object;
  std::vector<std::string> columns;
  exasol_gw::SessionGwDescribeTableResult described;
  std::uint32_t max_rows_per_batch= 10000;
  std::uint32_t pending_rows= 0;
  std::uint32_t successful_batches= 0;
  std::vector<NativeColumnBuffer> pending_columns;
};

struct UpdateContext
{
  UpdateContext(std::uint32_t max_rows, THD *thd)
    : session(&exasol_gw::session_for_thd(thd)), connection(nullptr),
      max_rows_per_batch(max_rows)
  {
    DBUG_EXECUTE_IF("exasol_gw_update_context_constructor_oom", throw std::bad_alloc(););
  }

  int append(TABLE *table, const exasol_gw::SessionGwRowHandle &row_handle)
  {
    try
    {
      DbugReadSetGuard read_set_guard(table);
      ensure_operation_open(table);
      if (pending_columns.empty())
      {
        for (Field **field= table->field; *field; ++field)
        {
          NativeColumnBuffer column;
          column.variable= native_field_is_variable(*field);
          pending_columns.push_back(std::move(column));
        }
      }
      std::size_t column_index= 0;
      for (Field **field= table->field; *field; ++field, ++column_index)
      {
        if (!append_field_to_column_buffer(pending_columns[column_index], *field))
        {
          abort();
          return HA_ERR_UNSUPPORTED;
        }
      }
      pending_handles.push_back(row_handle);
      ++pending_rows;
      if (pending_rows >= max_rows_per_batch)
        return flush(table);
      return 0;
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int close(TABLE *table)
  {
    if (failed)
      return 0;
    try
    {
      const int rc= flush(table);
      if (rc != 0)
        return rc;
      return close_operation();
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_open(TABLE *table)
  {
    if (initialized)
      return;
    schema= table_schema_name(table);
    object= table_object_name(table);
    columns= table_column_names(table);
    described= connection->describe_table(schema, object);
    initialized= true;
  }

  int flush(TABLE *table)
  {
    if (failed || pending_rows == 0)
      return 0;
    std::vector<std::uint8_t> batch;
    if (!build_native_batch_from_columns(pending_columns, pending_rows, &batch))
    {
      abort();
      return HA_ERR_UNSUPPORTED;
    }

    try
    {
      const std::uint64_t affected= connection->update_rows(operation_id, pending_handles, batch);
      if (affected != pending_rows)
      {
        abort();
        return HA_ERR_KEY_NOT_FOUND;
      }
      reset_pending();
      return 0;
    }
    catch (const std::exception &ex)
    {
      abort();
      if (!current_thd->get_stmt_da()->is_set())
        my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_operation_open(TABLE *table)
  {
    if (!connection)
      connection= &session->connection();
    ensure_open(table);
    if (operation_open)
      return;
    const exasol_gw::SessionGwOpenOperationResult opened=
        connection->open_table_update(schema, object, columns, max_rows_per_batch, described.arrow_schema);
    operation_id= opened.operation_id;
    operation_open= true;
    session->operation_opened();
  }

  int close_operation()
  {
    if (!operation_open)
      return 0;
    try
    {
      connection->close_operation(operation_id);
      operation_open= false;
      operation_id= 0;
      session->operation_closed();
      return 0;
    }
    catch (...)
    {
      abort();
      throw;
    }
  }

  void abort()
  {
    if (failed)
      return;
    session->reset();
    connection= nullptr;
    operation_open= false;
    operation_id= 0;
    reset_pending();
    failed= true;
  }

  void reset_pending()
  {
    pending_columns.clear();
    pending_handles.clear();
    pending_rows= 0;
  }

  exasol_gw::SessionGwThdContext *session;
  exasol_gw::SessionGwConnection *connection;
  bool initialized= false;
  bool operation_open= false;
  bool failed= false;
  std::uint64_t operation_id= 0;
  std::string schema;
  std::string object;
  std::vector<std::string> columns;
  exasol_gw::SessionGwDescribeTableResult described;
  std::uint32_t max_rows_per_batch= 10000;
  std::uint32_t pending_rows= 0;
  std::vector<NativeColumnBuffer> pending_columns;
  std::vector<exasol_gw::SessionGwRowHandle> pending_handles;
};

struct DeleteContext
{
  DeleteContext(std::uint32_t max_rows, THD *thd)
    : session(&exasol_gw::session_for_thd(thd)), connection(nullptr),
      max_rows_per_batch(max_rows)
  {
    DBUG_EXECUTE_IF("exasol_gw_delete_context_constructor_oom", throw std::bad_alloc(););
  }

  int append(TABLE *table, const exasol_gw::SessionGwRowHandle &row_handle)
  {
    try
    {
      ensure_operation_open(table);
      pending_handles.push_back(row_handle);
      if (pending_handles.size() >= max_rows_per_batch)
        return flush(table);
      return 0;
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int close(TABLE *table)
  {
    if (failed)
      return 0;
    try
    {
      const int rc= flush(table);
      if (rc != 0)
        return rc;
      return close_operation();
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_open(TABLE *table)
  {
    if (initialized)
      return;
    schema= table_schema_name(table);
    object= table_object_name(table);
    initialized= true;
  }

  int flush(TABLE *)
  {
    if (failed || pending_handles.empty())
      return 0;
    try
    {
      const std::uint64_t affected= connection->delete_rows(operation_id, pending_handles);
      if (affected != pending_handles.size())
      {
        abort();
        return HA_ERR_KEY_NOT_FOUND;
      }
      pending_handles.clear();
      return 0;
    }
    catch (const std::exception &ex)
    {
      abort();
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_operation_open(TABLE *table)
  {
    if (!connection)
      connection= &session->connection();
    ensure_open(table);
    if (operation_open)
      return;
    const exasol_gw::SessionGwOpenOperationResult opened=
        connection->open_table_delete(schema, object, max_rows_per_batch);
    operation_id= opened.operation_id;
    operation_open= true;
    session->operation_opened();
  }

  int close_operation()
  {
    if (!operation_open)
      return 0;
    try
    {
      connection->close_operation(operation_id);
      operation_open= false;
      operation_id= 0;
      session->operation_closed();
      return 0;
    }
    catch (...)
    {
      abort();
      throw;
    }
  }

  void abort()
  {
    if (failed)
      return;
    session->reset();
    connection= nullptr;
    operation_open= false;
    operation_id= 0;
    pending_handles.clear();
    failed= true;
  }

  exasol_gw::SessionGwThdContext *session;
  exasol_gw::SessionGwConnection *connection;
  bool initialized= false;
  bool operation_open= false;
  bool failed= false;
  std::uint64_t operation_id= 0;
  std::string schema;
  std::string object;
  std::uint32_t max_rows_per_batch= 10000;
  std::vector<exasol_gw::SessionGwRowHandle> pending_handles;
};

} // namespace

ha_exasol_gw::~ha_exasol_gw()
{
  try
  {
    (void) close_dml_contexts();
    delete cursor;
  }
  catch (...)
  {
    delete insert_context;
    delete update_context;
    delete delete_context;
    delete cursor;
  }
}

void ha_exasol_gw::start_bulk_insert(ha_rows, uint)
{
  try
  {
    if (!insert_context)
      insert_context= new InsertContext(insert_batch_rows_from_environment(), table->in_use);
    if (!insert_context)
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_OUT_OF_MEM,
               "starting EXASOL bulk insert: out of memory");
  }
  catch (...)
  {
    insert_context= nullptr;
    (void) report_handler_exception("starting EXASOL bulk insert");
  }
}

int ha_exasol_gw::end_bulk_insert()
{
  try
  {
    return close_insert_context();
  }
  catch (...)
  {
    return report_handler_exception("ending EXASOL bulk insert");
  }
}

int ha_exasol_gw::external_lock(THD *thd, int lock_type)
{
  try
  {
    if (lock_type != F_UNLCK)
    {
      exasol_gw::session_for_thd(thd).statement_table_opened();
      return 0;
    }

    const int rc= close_dml_contexts();
    exasol_gw::session_for_thd(thd).statement_table_closed();
    return rc;
  }
  catch (...)
  {
    return report_handler_exception("changing EXASOL statement lock state");
  }
}

int ha_exasol_gw::close_insert_context()
{
  if (!insert_context)
    return 0;
  std::unique_ptr<InsertContext> context(insert_context);
  insert_context= nullptr;
  return context->close(table);
}

int ha_exasol_gw::close_update_context()
{
  if (!update_context)
    return 0;
  std::unique_ptr<UpdateContext> context(update_context);
  update_context= nullptr;
  return context->close(table);
}

int ha_exasol_gw::close_delete_context()
{
  if (!delete_context)
    return 0;
  std::unique_ptr<DeleteContext> context(delete_context);
  delete_context= nullptr;
  return context->close(table);
}

int ha_exasol_gw::close_dml_contexts()
{
  const int insert_rc= close_insert_context();
  const int update_rc= close_update_context();
  const int delete_rc= close_delete_context();
  if (insert_rc != 0)
    return insert_rc;
  if (update_rc != 0)
    return update_rc;
  return delete_rc;
}

int ha_exasol_gw::write_row(const uchar *)
{
  try
  {
    if (!insert_context)
      insert_context= new InsertContext(insert_batch_rows_from_environment(), table->in_use);
    if (!insert_context)
      return HA_ERR_OUT_OF_MEM;
    return insert_context->append(table);
  }
  catch (...)
  {
    return report_handler_exception("writing EXASOL row");
  }
}

int ha_exasol_gw::update_row(const uchar *, const uchar *new_data)
{
  try
  {
    if (new_data != table->record[0])
      return HA_ERR_WRONG_COMMAND;
    if (!update_context)
      update_context= new UpdateContext(update_batch_rows_from_environment(), table->in_use);
    if (!update_context)
      return HA_ERR_OUT_OF_MEM;
    const exasol_gw::SessionGwRowHandle row_handle= have_positioned_row_handle
        ? last_positioned_row_handle
        : (cursor ? cursor->last_row_handle() : row_handle_from_ref(ref));
    have_positioned_row_handle= false;
    return update_context->append(table, row_handle);
  }
  catch (...)
  {
    return report_handler_exception("updating EXASOL row");
  }
}

int ha_exasol_gw::delete_row(const uchar *)
{
  try
  {
    if (!delete_context)
      delete_context= new DeleteContext(delete_batch_rows_from_environment(), table->in_use);
    if (!delete_context)
      return HA_ERR_OUT_OF_MEM;
    const exasol_gw::SessionGwRowHandle row_handle= have_positioned_row_handle
        ? last_positioned_row_handle
        : (cursor ? cursor->last_row_handle() : row_handle_from_ref(ref));
    have_positioned_row_handle= false;
    return delete_context->append(table, row_handle);
  }
  catch (...)
  {
    return report_handler_exception("deleting EXASOL row");
  }
}

static handler *exasol_gw_create_handler(handlerton *hton,
                                            TABLE_SHARE *table,
                                            MEM_ROOT *mem_root)
{
  try
  {
    return new (mem_root) ha_exasol_gw(hton, table);
  }
  catch (...)
  {
    (void) report_handler_exception("allocating EXASOL handler");
    return nullptr;
  }
}

static bool exasol_gw_table_belongs_to_engine(TABLE_LIST *tbl)
{
  return tbl && tbl->table && tbl->table->file &&
         tbl->table->file->partition_ht() == exasol_gw_hton;
}

static TABLE *get_exasol_gw_table_for_pushdown(SELECT_LEX *sel_lex)
{
  TABLE_LIST *tbl= sel_lex->join ? sel_lex->join->tables_list : nullptr;
  TABLE *found= nullptr;
  for (; tbl; tbl= tbl->next_local)
  {
    if (tbl->derived)
      continue;
    if (!exasol_gw_table_belongs_to_engine(tbl))
      return nullptr;
    if (!found)
      found= tbl->table;
  }

  for (SELECT_LEX_UNIT *unit= sel_lex->first_inner_unit(); unit; unit= unit->next_unit())
  {
    for (SELECT_LEX *inner= unit->first_select(); inner; inner= inner->next_select())
    {
      TABLE *next_table= get_exasol_gw_table_for_pushdown(inner);
      if (!next_table)
        return nullptr;
      if (!found)
        found= next_table;
    }
  }
  return found;
}

static TABLE *get_exasol_gw_table_for_unit_pushdown(SELECT_LEX_UNIT *lex_unit)
{
  TABLE *table= nullptr;
  for (SELECT_LEX *sel_lex= lex_unit->first_select(); sel_lex;
       sel_lex= sel_lex->next_select())
  {
    TABLE *next_table= get_exasol_gw_table_for_pushdown(sel_lex);
    if (!next_table)
      return nullptr;
    if (!table)
      table= next_table;
  }
  return table;
}

static bool are_supported_exasol_gw_selects(SELECT_LEX_UNIT *lex_unit);

static bool is_supported_exasol_gw_pushdown(enum_sql_command sql_command)
{
  switch (sql_command)
  {
  case SQLCOM_SELECT:
  case SQLCOM_INSERT_SELECT:
    return true;
  default:
    return false;
  }
}

static bool is_supported_exasol_gw_select(SELECT_LEX *sel_lex)
{
  if (!sel_lex)
    return false;

  if (sel_lex->limit_params.with_ties)
    return false;

  if ((sel_lex->options & SELECT_DISTINCT) && sel_lex->order_list.elements &&
      sel_lex->limit_params.select_limit)
    return false;

  for (SELECT_LEX_UNIT *unit= sel_lex->first_inner_unit(); unit; unit= unit->next_unit())
  {
    if (!are_supported_exasol_gw_selects(unit))
      return false;
  }

  return true;
}

static bool are_supported_exasol_gw_selects(SELECT_LEX_UNIT *lex_unit)
{
  if (!lex_unit)
    return false;

  for (SELECT_LEX *sel_lex= lex_unit->first_select(); sel_lex;
       sel_lex= sel_lex->next_select())
  {
    if (!is_supported_exasol_gw_select(sel_lex))
      return false;
  }
  return true;
}

static select_handler *create_exasol_gw_select_handler(THD *thd,
                                                          SELECT_LEX *sel_lex,
                                                          SELECT_LEX_UNIT *lex_unit)
{
  try
  {
    if (!is_supported_exasol_gw_pushdown(thd->lex->sql_command))
      return nullptr;

    if (!is_supported_exasol_gw_select(sel_lex))
      return nullptr;

    TABLE *tbl= get_exasol_gw_table_for_pushdown(sel_lex);
    if (!tbl)
      return nullptr;

    if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
      return nullptr;

    return new ha_exasol_gw_select_handler(thd, sel_lex, lex_unit, tbl);
  }
  catch (...)
  {
    (void) report_handler_exception("creating EXASOL SELECT pushdown handler");
    return nullptr;
  }
}

static select_handler *create_exasol_gw_unit_handler(THD *thd,
                                                        SELECT_LEX_UNIT *lex_unit)
{
  try
  {
    if (!is_supported_exasol_gw_pushdown(thd->lex->sql_command))
      return nullptr;

    if (!are_supported_exasol_gw_selects(lex_unit))
      return nullptr;

    TABLE *tbl= get_exasol_gw_table_for_unit_pushdown(lex_unit);
    if (!tbl)
      return nullptr;

    if (lex_unit->uncacheable & UNCACHEABLE_SIDEEFFECT)
      return nullptr;

    return new ha_exasol_gw_select_handler(thd, lex_unit, tbl);
  }
  catch (...)
  {
    (void) report_handler_exception("creating EXASOL unit pushdown handler");
    return nullptr;
  }
}

static derived_handler *create_exasol_gw_derived_handler(THD *thd,
                                                            TABLE_LIST *derived)
{
  try
  {
    if (!derived || !derived->derived)
      return nullptr;

    if (!is_supported_exasol_gw_pushdown(thd->lex->sql_command))
      return nullptr;

    if (!are_supported_exasol_gw_selects(derived->derived))
      return nullptr;

    TABLE *tbl= get_exasol_gw_table_for_unit_pushdown(derived->derived);
    if (!tbl)
      return nullptr;

    if (derived->derived->uncacheable & UNCACHEABLE_SIDEEFFECT)
      return nullptr;

    return new ha_exasol_gw_derived_handler(thd, derived, tbl);
  }
  catch (...)
  {
    (void) report_handler_exception("creating EXASOL derived pushdown handler");
    return nullptr;
  }
}

static int exasol_gw_close_connection(THD *thd)
{
  try
  {
    exasol_gw::destroy_session_for_thd(thd);
    return 0;
  }
  catch (...)
  {
    return report_handler_exception("closing EXASOL THD connection");
  }
}

static int exasol_gw_init(void *p)
{
  exasol_gw_hton= static_cast<handlerton *>(p);
  exasol_gw_hton->db_type= DB_TYPE_AUTOASSIGN;
  exasol_gw_hton->create= exasol_gw_create_handler;
  exasol_gw_hton->close_connection= exasol_gw_close_connection;
  exasol_gw_hton->create_select= create_exasol_gw_select_handler;
  exasol_gw_hton->create_unit= create_exasol_gw_unit_handler;
  exasol_gw_hton->create_derived= create_exasol_gw_derived_handler;
  exasol_gw_hton->flags= HTON_NO_BINLOG_ROW_OPT;
  return 0;
}

static int exasol_gw_done(void *)
{
  exasol_gw_hton= nullptr;
  return 0;
}

static struct st_mysql_storage_engine exasol_gw_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(exasol_gw)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &exasol_gw_storage_engine,
  "EXASOL",
  "Exasol",
  "EXASOL Session Gateway proxy storage engine",
  PLUGIN_LICENSE_GPL,
  exasol_gw_init,
  exasol_gw_done,
  0x0001,
  nullptr,
  nullptr,
  "0.1-sessiongw",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;
