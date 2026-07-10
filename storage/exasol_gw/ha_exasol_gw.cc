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

#include <chrono>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

static select_handler *create_exasol_gw_select_handler(THD *thd,
                                                          SELECT_LEX *sel_lex,
                                                          SELECT_LEX_UNIT *lex_unit);
static select_handler *create_exasol_gw_unit_handler(THD *thd,
                                                        SELECT_LEX_UNIT *lex_unit);
static derived_handler *create_exasol_gw_derived_handler(THD *thd,
                                                            TABLE_LIST *derived);

handlerton *exasol_gw_hton= nullptr;

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
    : handler(hton, table_arg), share(nullptr), cursor(nullptr)
  {
  }

  ~ha_exasol_gw() override
  {
    delete cursor;
  }

  const char *index_type(uint) override { return "NONE"; }

  ulonglong table_flags() const override
  {
    return HA_BINLOG_STMT_CAPABLE | HA_REC_NOT_IN_SEQ | HA_NULL_IN_KEY;
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

  int create(const char *, TABLE *, HA_CREATE_INFO *) override
  {
    return 0;
  }

  int delete_table(const char *) override
  {
    return 0;
  }

  int write_row(const uchar *) override;
  int update_row(const uchar *, const uchar *) override { return HA_ERR_WRONG_COMMAND; }
  int delete_row(const uchar *) override { return HA_ERR_WRONG_COMMAND; }

  int rnd_init(bool) override
  {
    (void) rnd_end();
    cursor= new ha_exasol_gw_cursor();
    char error_buffer[512]= {0};
    const int rc= cursor->open_table_scan(table, error_buffer, sizeof(error_buffer));
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
  void position(const uchar *) override {}

  int info(uint) override
  {
    stats.records= 1000;
    return 0;
  }

  int external_lock(THD *, int) override { return 0; }

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

  THR_LOCK_DATA lock;
  Exasol_gw_share *share;
  ha_exasol_gw_cursor *cursor;
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

void append_fixed_value(std::vector<std::uint8_t> &out, const void *value, std::size_t size)
{
  const auto *bytes= static_cast<const std::uint8_t *>(value);
  out.insert(out.end(), bytes, bytes + size);
}

bool append_field_to_native_batch(exasol_gw::NativeWriteBatchBuilder &builder, Field *field)
{
  std::vector<std::uint8_t> nulls(1, field->is_null() ? exasol_gw::native_write_null : exasol_gw::native_write_not_null);
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
    std::vector<std::uint8_t> fixed;
    append_fixed_value(fixed, &value, sizeof(value));
    builder.append_fixed_column(nulls, fixed);
    return true;
  }
  case MYSQL_TYPE_FLOAT:
  case MYSQL_TYPE_DOUBLE:
  {
    const double value= field->is_null() ? 0.0 : field->val_real();
    std::vector<std::uint8_t> fixed;
    append_fixed_value(fixed, &value, sizeof(value));
    builder.append_fixed_column(nulls, fixed);
    return true;
  }
  case MYSQL_TYPE_BIT:
  {
    const std::uint8_t value= field->is_null() ? 0 : static_cast<std::uint8_t>(field->val_int() ? 0xffU : 0x00U);
    std::vector<std::uint8_t> fixed;
    append_fixed_value(fixed, &value, sizeof(value));
    builder.append_fixed_column(nulls, fixed);
    return true;
  }
  default:
  {
    StringBuffer<512> value_buffer;
    std::vector<std::size_t> sizes(1, 0);
    std::vector<std::uint8_t> variable;
    if (!field->is_null())
    {
      String *value= field->val_str(&value_buffer);
      if (!value)
        return false;
      sizes[0]= value->length();
      variable.insert(variable.end(),
                      reinterpret_cast<const std::uint8_t *>(value->ptr()),
                      reinterpret_cast<const std::uint8_t *>(value->ptr()) + value->length());
    }
    builder.append_variable_column(nulls, sizes, variable);
    return true;
  }
  }
}

} // namespace

int ha_exasol_gw::write_row(const uchar *)
{
  constexpr int max_attempts= 8;
  std::string last_error;
  for (int attempt= 0; attempt < max_attempts; ++attempt)
  {
    try
    {
      exasol_gw::SessionGwOptions options= exasol_gw::options_from_environment();
      exasol_gw::SessionGwConnection connection;
      connection.connect_and_enter(options);

      const std::string schema= table_schema_name(table);
      const std::string object= table_object_name(table);
      exasol_gw::SessionGwDescribeTableResult described= connection.describe_table(schema, object);

      std::vector<std::string> columns;
      for (Field **field= table->field; *field; ++field)
        columns.push_back(field_name(*field));

      exasol_gw::SessionGwOpenOperationResult opened=
          connection.open_table_insert(schema, object, columns, 1, described.arrow_schema);

      std::vector<std::uint8_t> batch;
      exasol_gw::NativeWriteBatchBuilder builder(batch);
      builder.begin(1, static_cast<std::uint32_t>(columns.size()));
      for (Field **field= table->field; *field; ++field)
      {
        if (!append_field_to_native_batch(builder, *field))
          return HA_ERR_UNSUPPORTED;
      }
      builder.finish();

      const std::uint64_t affected= connection.insert_rows(opened.operation_id, 1, batch);
      connection.close_operation(opened.operation_id);
      connection.close();
      return affected == 1 ? 0 : HA_ERR_INTERNAL_ERROR;
    }
    catch (const std::exception &ex)
    {
      last_error= ex.what();
      if (last_error.find("Transaction collision") == std::string::npos &&
          last_error.find("GlobalTransactionRollback") == std::string::npos)
      {
        my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR, last_error.c_str());
        return HA_ERR_INTERNAL_ERROR;
      }
      std::this_thread::sleep_for(std::chrono::milliseconds(25 * (attempt + 1)));
    }
  }
  my_error(ER_GET_ERRNO, MYF(0), HA_ERR_INTERNAL_ERROR,
           last_error.empty() ? "EXASOL SessionGW insert failed" : last_error.c_str());
  return HA_ERR_INTERNAL_ERROR;
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
