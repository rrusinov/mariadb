#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql/plugin.h>
#include <thr_lock.h>

#include "handler.h"
#include "table.h"
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
std::string create_table_sql(TABLE *form);
std::string drop_table_sql_from_path(const char *from);
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
      insert_context(nullptr), update_context(nullptr), delete_context(nullptr)
  {
    ref_length= sizeof(std::uint32_t) + sizeof(std::uint64_t);
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
    share= get_share();
    if (!share)
      return HA_ERR_OUT_OF_MEM;
    thr_lock_data_init(&share->lock, &lock, nullptr);
    return 0;
  }

  int close(void) override
  {
    return rnd_end();
  }

  int create(const char *, TABLE *form, HA_CREATE_INFO *) override
  {
    try
    {
      exasol_gw::SessionGwOptions options= exasol_gw::options_from_environment();
      const std::string ddl= create_table_sql(form);
      const std::size_t separator= ddl.find(';');
      if (separator != std::string::npos)
      {
        exasol_gw::execute_sql(options, ddl.substr(0, separator));
        exasol_gw::execute_sql(options, ddl.substr(separator + 1));
      }
      else
      {
        exasol_gw::execute_sql(options, ddl);
      }
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int delete_table(const char *from) override
  {
    try
    {
      exasol_gw::execute_sql(exasol_gw::options_from_environment(), drop_table_sql_from_path(from));
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void start_bulk_insert(ha_rows rows, uint flags) override;
  int end_bulk_insert() override;
  int write_row(const uchar *) override;
  int update_row(const uchar *, const uchar *) override;
  int delete_row(const uchar *) override;

  int rnd_init(bool) override
  {
    (void) rnd_end();
    cursor= new ha_exasol_gw_cursor();
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

  int rnd_end() override
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

  int rnd_next(uchar *buf) override
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

  int rnd_pos(uchar *, uchar *) override { return HA_ERR_WRONG_COMMAND; }
  void position(const uchar *) override
  {
    if (!cursor || !ref)
      return;
    const exasol_gw::SessionGwRowHandle row_handle= cursor->last_row_handle();
    std::memcpy(ref, &row_handle.node_id, sizeof(row_handle.node_id));
    std::memcpy(ref + sizeof(row_handle.node_id),
                &row_handle.local_row_number,
                sizeof(row_handle.local_row_number));
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
    if (!share)
    {
      lock_shared_ha_data();
      if (!share)
      {
        share= new Exasol_gw_share();
        if (!share)
        {
          unlock_shared_ha_data();
          return nullptr;
        }
        set_ha_share_ref(reinterpret_cast<Handler_share **>(&share));
      }
      unlock_shared_ha_data();
    }
    return share;
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

std::string exasol_type_for_field(Field *field)
{
  switch (field->type())
  {
  case MYSQL_TYPE_TINY:
    return (field->flags & UNSIGNED_FLAG) ? "DECIMAL(3,0)" : "DECIMAL(3,0)";
  case MYSQL_TYPE_SHORT:
    return (field->flags & UNSIGNED_FLAG) ? "DECIMAL(5,0)" : "DECIMAL(9,0)";
  case MYSQL_TYPE_INT24:
    return (field->flags & UNSIGNED_FLAG) ? "DECIMAL(8,0)" : "DECIMAL(9,0)";
  case MYSQL_TYPE_LONG:
    return (field->flags & UNSIGNED_FLAG) ? "DECIMAL(10,0)" : "DECIMAL(18,0)";
  case MYSQL_TYPE_LONGLONG:
    return (field->flags & UNSIGNED_FLAG) ? "DECIMAL(18,0)" : "DECIMAL(18,0)";
  case MYSQL_TYPE_YEAR:
    return "DECIMAL(4,0)";
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
    return "DOUBLE PRECISION";
  case MYSQL_TYPE_DECIMAL:
  case MYSQL_TYPE_NEWDECIMAL:
  {
    const uint scale= field->decimals();
    const uint precision= std::max<uint>(scale + 1U, std::min<uint>(36U, field->field_length));
    return "DECIMAL(" + std::to_string(precision) + "," + std::to_string(scale) + ")";
  }
  case MYSQL_TYPE_DATE:
  case MYSQL_TYPE_NEWDATE:
    return "DATE";
  case MYSQL_TYPE_DATETIME:
  case MYSQL_TYPE_DATETIME2:
  case MYSQL_TYPE_TIMESTAMP:
  case MYSQL_TYPE_TIMESTAMP2:
    return "TIMESTAMP(6)";
  case MYSQL_TYPE_BIT:
    return "BOOLEAN";
  default:
  {
    const uint length= std::max<uint>(1, field->char_length());
    if (field->type() == MYSQL_TYPE_STRING)
      return "CHAR(" + std::to_string(length) + ") UTF8";
    return "VARCHAR(" + std::to_string(length) + ") UTF8";
  }
  }
}

std::string create_table_sql(TABLE *form)
{
  const std::string schema= table_schema_name(form);
  const std::string object= table_object_name(form);
  std::string sql= "CREATE SCHEMA IF NOT EXISTS " + quote_exasol_identifier(schema) + "; CREATE OR REPLACE TABLE " +
                   quote_exasol_identifier(schema) + "." + quote_exasol_identifier(object) + " (";
  bool first= true;
  for (Field **field= form->field; *field; ++field)
  {
    if (!first)
      sql += ", ";
    first= false;
    sql += quote_exasol_identifier(field_name(*field));
    sql += " ";
    sql += exasol_type_for_field(*field);
  }
  sql += ")";
  return sql;
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
    std::memcpy(&row_handle.node_id, ref, sizeof(row_handle.node_id));
    std::memcpy(&row_handle.local_row_number,
                ref + sizeof(row_handle.node_id),
                sizeof(row_handle.local_row_number));
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

bool is_transaction_collision(const std::string &message)
{
  return message.find("Transaction collision") != std::string::npos ||
         message.find("GlobalTransactionRollback") != std::string::npos;
}

struct InsertContext
{
  explicit InsertContext(std::uint32_t max_rows): max_rows_per_batch(max_rows) {}

  int append(TABLE *table)
  {
    try
    {
      ensure_open(table);
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
          return HA_ERR_UNSUPPORTED;
      }
      ++pending_rows;
      if (pending_rows >= max_rows_per_batch)
        return flush(table);
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int close(TABLE *table)
  {
    try
    {
      int rc= flush(table);
      connection.close();
      connected= false;
      return rc;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_open(TABLE *table)
  {
    if (connected)
      return;
    options= exasol_gw::options_from_environment();
    connection.connect_and_enter(options);
    connected= true;
    schema= table_schema_name(table);
    object= table_object_name(table);
    columns= table_column_names(table);
    described= connection.describe_table(schema, object);
  }

  int flush(TABLE *table)
  {
    if (pending_rows == 0)
      return 0;
    std::vector<std::uint8_t> batch;
    if (!build_native_batch_from_columns(pending_columns, pending_rows, &batch))
      return HA_ERR_UNSUPPORTED;

    constexpr int max_attempts= 8;
    std::string last_error;
    for (int attempt= 0; attempt < max_attempts; ++attempt)
    {
      try
      {
        ensure_open(table);
        const exasol_gw::SessionGwOpenOperationResult opened=
            connection.open_table_insert(schema, object, columns, max_rows_per_batch, described.arrow_schema);
        const std::uint64_t affected= connection.insert_rows(opened.operation_id, pending_rows, batch);
        connection.close_operation(opened.operation_id);
        if (affected != pending_rows)
          return HA_ERR_INTERNAL_ERROR;
        reset_pending();
        return 0;
      }
      catch (const std::exception &ex)
      {
        last_error= ex.what();
        connection.close();
        connected= false;
        if (!is_transaction_collision(last_error))
        {
          my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, last_error.c_str());
          return HA_ERR_INTERNAL_ERROR;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
      }
    }
    my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR,
             last_error.empty() ? "EXASOL SessionGW batched insert failed" : last_error.c_str());
    return HA_ERR_INTERNAL_ERROR;
  }

  void reset_pending()
  {
    pending_columns.clear();
    pending_rows= 0;
  }

  exasol_gw::SessionGwOptions options;
  exasol_gw::SessionGwConnection connection;
  bool connected= false;
  std::string schema;
  std::string object;
  std::vector<std::string> columns;
  exasol_gw::SessionGwDescribeTableResult described;
  std::uint32_t max_rows_per_batch= 10000;
  std::uint32_t pending_rows= 0;
  std::vector<NativeColumnBuffer> pending_columns;
};

struct UpdateContext
{
  explicit UpdateContext(std::uint32_t max_rows): max_rows_per_batch(max_rows) {}

  int append(TABLE *table, const exasol_gw::SessionGwRowHandle &row_handle)
  {
    try
    {
      ensure_open(table);
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
          return HA_ERR_UNSUPPORTED;
      }
      pending_handles.push_back(row_handle);
      ++pending_rows;
      if (pending_rows >= max_rows_per_batch)
        return flush(table);
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int close(TABLE *table)
  {
    try
    {
      int rc= flush(table);
      connection.close();
      connected= false;
      return rc;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_open(TABLE *table)
  {
    if (connected)
      return;
    options= exasol_gw::options_from_environment();
    connection.connect_and_enter(options);
    connected= true;
    schema= table_schema_name(table);
    object= table_object_name(table);
    columns= table_column_names(table);
    described= connection.describe_table(schema, object);
  }

  int flush(TABLE *table)
  {
    if (pending_rows == 0)
      return 0;
    std::vector<std::uint8_t> batch;
    if (!build_native_batch_from_columns(pending_columns, pending_rows, &batch))
      return HA_ERR_UNSUPPORTED;

    try
    {
      ensure_open(table);
      const exasol_gw::SessionGwOpenOperationResult opened=
          connection.open_table_update(schema, object, columns, max_rows_per_batch, described.arrow_schema);
      const std::uint64_t affected= connection.update_rows(opened.operation_id, pending_handles, batch);
      connection.close_operation(opened.operation_id);
      if (affected != pending_rows)
        return HA_ERR_KEY_NOT_FOUND;
      reset_pending();
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void reset_pending()
  {
    pending_columns.clear();
    pending_handles.clear();
    pending_rows= 0;
  }

  exasol_gw::SessionGwOptions options;
  exasol_gw::SessionGwConnection connection;
  bool connected= false;
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
  explicit DeleteContext(std::uint32_t max_rows): max_rows_per_batch(max_rows) {}

  int append(TABLE *table, const exasol_gw::SessionGwRowHandle &row_handle)
  {
    try
    {
      ensure_open(table);
      pending_handles.push_back(row_handle);
      if (pending_handles.size() >= max_rows_per_batch)
        return flush(table);
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  int close(TABLE *table)
  {
    try
    {
      int rc= flush(table);
      connection.close();
      connected= false;
      return rc;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  void ensure_open(TABLE *table)
  {
    if (connected)
      return;
    options= exasol_gw::options_from_environment();
    connection.connect_and_enter(options);
    connected= true;
    schema= table_schema_name(table);
    object= table_object_name(table);
  }

  int flush(TABLE *table)
  {
    if (pending_handles.empty())
      return 0;
    try
    {
      ensure_open(table);
      const exasol_gw::SessionGwOpenOperationResult opened=
          connection.open_table_delete(schema, object, max_rows_per_batch);
      const std::uint64_t affected= connection.delete_rows(opened.operation_id, pending_handles);
      connection.close_operation(opened.operation_id);
      if (affected != pending_handles.size())
        return HA_ERR_KEY_NOT_FOUND;
      pending_handles.clear();
      return 0;
    }
    catch (const std::exception &ex)
    {
      my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, ex.what());
      return HA_ERR_INTERNAL_ERROR;
    }
  }

  exasol_gw::SessionGwOptions options;
  exasol_gw::SessionGwConnection connection;
  bool connected= false;
  std::string schema;
  std::string object;
  std::uint32_t max_rows_per_batch= 10000;
  std::vector<exasol_gw::SessionGwRowHandle> pending_handles;
};

} // namespace

ha_exasol_gw::~ha_exasol_gw()
{
  (void) close_dml_contexts();
  delete cursor;
}

void ha_exasol_gw::start_bulk_insert(ha_rows, uint)
{
  if (!insert_context)
    insert_context= new InsertContext(insert_batch_rows_from_environment());
}

int ha_exasol_gw::end_bulk_insert()
{
  return close_insert_context();
}

int ha_exasol_gw::external_lock(THD *, int lock_type)
{
  if (lock_type == F_UNLCK)
    return close_dml_contexts();
  return 0;
}

int ha_exasol_gw::close_insert_context()
{
  if (!insert_context)
    return 0;
  InsertContext *context= insert_context;
  insert_context= nullptr;
  const int rc= context->close(table);
  delete context;
  return rc;
}

int ha_exasol_gw::close_update_context()
{
  if (!update_context)
    return 0;
  UpdateContext *context= update_context;
  update_context= nullptr;
  const int rc= context->close(table);
  delete context;
  return rc;
}

int ha_exasol_gw::close_delete_context()
{
  if (!delete_context)
    return 0;
  DeleteContext *context= delete_context;
  delete_context= nullptr;
  const int rc= context->close(table);
  delete context;
  return rc;
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
  if (!insert_context)
    insert_context= new InsertContext(insert_batch_rows_from_environment());
  if (!insert_context)
    return HA_ERR_OUT_OF_MEM;
  return insert_context->append(table);
}

int ha_exasol_gw::update_row(const uchar *, const uchar *new_data)
{
  if (new_data != table->record[0])
    return HA_ERR_WRONG_COMMAND;
  if (!update_context)
    update_context= new UpdateContext(update_batch_rows_from_environment());
  if (!update_context)
    return HA_ERR_OUT_OF_MEM;
  const exasol_gw::SessionGwRowHandle row_handle= cursor ? cursor->last_row_handle() : row_handle_from_ref(ref);
  return update_context->append(table, row_handle);
}

int ha_exasol_gw::delete_row(const uchar *)
{
  if (!delete_context)
    delete_context= new DeleteContext(delete_batch_rows_from_environment());
  if (!delete_context)
    return HA_ERR_OUT_OF_MEM;
  const exasol_gw::SessionGwRowHandle row_handle= cursor ? cursor->last_row_handle() : row_handle_from_ref(ref);
  return delete_context->append(table, row_handle);
}

static handler *exasol_gw_create_handler(handlerton *hton,
                                            TABLE_SHARE *table,
                                            MEM_ROOT *mem_root)
{
  return new (mem_root) ha_exasol_gw(hton, table);
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

static select_handler *create_exasol_gw_unit_handler(THD *thd,
                                                        SELECT_LEX_UNIT *lex_unit)
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

static derived_handler *create_exasol_gw_derived_handler(THD *thd,
                                                            TABLE_LIST *derived)
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

static int exasol_gw_init(void *p)
{
  exasol_gw_hton= static_cast<handlerton *>(p);
  exasol_gw_hton->db_type= DB_TYPE_AUTOASSIGN;
  exasol_gw_hton->create= exasol_gw_create_handler;
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
