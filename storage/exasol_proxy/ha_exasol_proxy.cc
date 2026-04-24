#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql/plugin.h>
#include <mysql.h>
#include <thr_lock.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstdlib>

#include "handler.h"
#include "table.h"
#include "sql_class.h"
#include "sql_lex.h"
#include "sql_select.h"

#include "mariadbstorageenginecoreabi.h"
#include "exasol_proxy_pushdown.h"

static select_handler *create_exasol_proxy_select_handler(THD *thd,
                                                          SELECT_LEX *sel_lex,
                                                          SELECT_LEX_UNIT *lex_unit);
static select_handler *create_exasol_proxy_unit_handler(THD *thd,
                                                        SELECT_LEX_UNIT *lex_unit);

static handlerton *exasol_proxy_hton= nullptr;
static void *exasol_proxy_core_handle= nullptr;
static const ExasolMariaDBCoreAbiV1 *exasol_proxy_core_abi= nullptr;

static void exasol_proxy_log_error(const char *message)
{
  std::fprintf(stderr, "ha_exasol_proxy: %s\n", message);
}

static void exasol_proxy_log_dlerror(const char *context)
{
  const char *dlerror_message= dlerror();
  if (dlerror_message)
    std::fprintf(stderr, "ha_exasol_proxy: %s: %s\n", context, dlerror_message);
  else
    std::fprintf(stderr, "ha_exasol_proxy: %s\n", context);
}

static int exasol_proxy_load_core()
{
  if (exasol_proxy_core_abi)
    return 0;

  const char *core_soname= std::getenv("EXASOL_PROXY_CORE_SONAME");
  if (!core_soname || !*core_soname)
    core_soname= "ha_exasol.so";

  exasol_proxy_core_handle= dlopen(core_soname, RTLD_NOW | RTLD_LOCAL);
  if (!exasol_proxy_core_handle)
  {
    exasol_proxy_log_dlerror("failed to load EXASOL core module");
    return 1;
  }

  dlerror();
  typedef const ExasolMariaDBCoreAbiV1 *(*get_core_abi_v1_fn)();
  get_core_abi_v1_fn get_core_abi_v1=
    reinterpret_cast<get_core_abi_v1_fn>(dlsym(exasol_proxy_core_handle,
                                               "exasol_mariadb_get_core_abi_v1"));
  if (!get_core_abi_v1)
  {
    exasol_proxy_log_dlerror("failed to resolve EXASOL core ABI");
    dlclose(exasol_proxy_core_handle);
    exasol_proxy_core_handle= nullptr;
    return 1;
  }

  exasol_proxy_core_abi= get_core_abi_v1();
  if (!exasol_proxy_core_abi ||
      exasol_proxy_core_abi->abiVersion != EXASOL_MARIADB_CORE_ABI_VERSION_1)
  {
    exasol_proxy_log_error("unsupported EXASOL core ABI");
    exasol_proxy_core_abi= nullptr;
    dlclose(exasol_proxy_core_handle);
    exasol_proxy_core_handle= nullptr;
    return 1;
  }

  return 0;
}

static bool exasol_proxy_table_belongs_to_engine(TABLE_LIST *tbl)
{
  return tbl && tbl->table && tbl->table->file &&
         tbl->table->file->partition_ht() == exasol_proxy_hton;
}

static TABLE *get_exasol_proxy_table_for_pushdown(SELECT_LEX *sel_lex)
{
  TABLE_LIST *tbl= sel_lex->join ? sel_lex->join->tables_list : nullptr;
  TABLE *found= nullptr;
  for (; tbl; tbl= tbl->next_global)
  {
    if (!exasol_proxy_table_belongs_to_engine(tbl))
      return nullptr;
    if (!found)
      found= tbl->table;
  }
  return found;
}

static TABLE *get_exasol_proxy_table_for_unit_pushdown(SELECT_LEX_UNIT *lex_unit)
{
  TABLE *table= nullptr;
  for (SELECT_LEX *sel_lex= lex_unit->first_select(); sel_lex;
       sel_lex= sel_lex->next_select())
  {
    TABLE *next_table= get_exasol_proxy_table_for_pushdown(sel_lex);
    if (!next_table)
      return nullptr;
    if (!table)
      table= next_table;
  }
  return table;
}

static bool is_supported_exasol_proxy_pushdown(enum_sql_command sql_command)
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

static select_handler *create_exasol_proxy_select_handler(THD *thd,
                                                          SELECT_LEX *sel_lex,
                                                          SELECT_LEX_UNIT *lex_unit)
{
  if (!is_supported_exasol_proxy_pushdown(thd->lex->sql_command))
    return nullptr;

  TABLE *tbl= get_exasol_proxy_table_for_pushdown(sel_lex);
  if (!tbl)
    return nullptr;

  if (sel_lex->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  return new ha_exasol_proxy_select_handler(thd, sel_lex, lex_unit, tbl);
}

static select_handler *create_exasol_proxy_unit_handler(THD *thd,
                                                        SELECT_LEX_UNIT *lex_unit)
{
  if (!is_supported_exasol_proxy_pushdown(thd->lex->sql_command))
    return nullptr;

  TABLE *tbl= get_exasol_proxy_table_for_unit_pushdown(lex_unit);
  if (!tbl)
    return nullptr;

  if (lex_unit->uncacheable & UNCACHEABLE_SIDEEFFECT)
    return nullptr;

  return new ha_exasol_proxy_select_handler(thd, lex_unit, tbl);
}

static int exasol_proxy_init(void *p)
{
  if (exasol_proxy_load_core() != 0)
    return 1;

  exasol_proxy_hton= static_cast<handlerton *>(p);
  if (!exasol_proxy_core_abi->initStorageEngine ||
      exasol_proxy_core_abi->initStorageEngine(p) != 0)
  {
    exasol_proxy_log_error("failed to initialize EXASOL core storage engine");
    exasol_proxy_hton= nullptr;
    return 1;
  }
  exasol_proxy_hton->create_select= create_exasol_proxy_select_handler;
  exasol_proxy_hton->create_unit= create_exasol_proxy_unit_handler;
  return 0;
}

static int exasol_proxy_done(void *)
{
  if (exasol_proxy_core_abi && exasol_proxy_core_abi->deinitStorageEngine)
    exasol_proxy_core_abi->deinitStorageEngine(nullptr);
  exasol_proxy_hton= nullptr;
  exasol_proxy_core_abi= nullptr;
  if (exasol_proxy_core_handle)
  {
    dlclose(exasol_proxy_core_handle);
    exasol_proxy_core_handle= nullptr;
  }
  return 0;
}

static struct st_mysql_storage_engine exasol_proxy_storage_engine=
{ MYSQL_HANDLERTON_INTERFACE_VERSION };

maria_declare_plugin(exasol_proxy)
{
  MYSQL_STORAGE_ENGINE_PLUGIN,
  &exasol_proxy_storage_engine,
  "EXASOL",
  "Exasol",
  "In-tree EXASOL proxy storage engine",
  PLUGIN_LICENSE_GPL,
  exasol_proxy_init,
  exasol_proxy_done,
  0x0001,
  nullptr,
  nullptr,
  "0.1",
  MariaDB_PLUGIN_MATURITY_EXPERIMENTAL
}
maria_declare_plugin_end;

#include "exasol_proxy_pushdown.cc"
