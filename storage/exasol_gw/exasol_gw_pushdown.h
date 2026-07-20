#ifndef HA_EXASOL_GW_PUSHDOWN_INCLUDED
#define HA_EXASOL_GW_PUSHDOWN_INCLUDED

#include "derived_handler.h"
#include "sql_string.h"
#include "select_handler.h"

#include "exasol_arrow_ipc.h"
#include "exasol_gw_session.h"

#include <memory>
#include <string>
#include <vector>

class ha_exasol_gw;

int validate_exasol_gw_table_metadata(TABLE *table);

class ha_exasol_gw_cursor
{
public:
  explicit ha_exasol_gw_cursor(THD *thd_arg);
  ~ha_exasol_gw_cursor();

  int open_pushed_query(TABLE *table_arg, const char *query_text, char *error_buffer,
                        unsigned long error_buffer_size);
  int open_table_scan(TABLE *table_arg, char *error_buffer, unsigned long error_buffer_size,
                      bool include_row_handles= false);
  int fetch_row(TABLE *table_arg, unsigned char *record, char *error_buffer,
                unsigned long error_buffer_size);
  int fetch_positioned_row(TABLE *table_arg,
                           const exasol_gw::SessionGwRowHandle &row_handle,
                           unsigned char *record,
                           char *error_buffer,
                           unsigned long error_buffer_size);
  exasol_gw::SessionGwRowHandle last_row_handle() const { return last_row_handle_; }
  int close(char *error_buffer, unsigned long error_buffer_size);

private:
  int fetch_next_batch(char *error_buffer, unsigned long error_buffer_size);
  int materialize_current_row(TABLE *table_arg, unsigned char *record, char *error_buffer,
                              unsigned long error_buffer_size);
  int materialize_row(TABLE *table_arg,
                      const exasol_gw::SessionGwNativeFetchResult &batch,
                      const std::vector<exasol_gw::SessionGwRowHandle> &row_handles,
                      std::size_t row,
                      unsigned char *record,
                      char *error_buffer,
                      unsigned long error_buffer_size);
  void initialize_column_kinds(TABLE *table_arg);
  std::vector<std::string> initialize_table_scan_columns(TABLE *table_arg);
  void select_fetch_rows(TABLE *table_arg, bool include_row_handles);

  exasol_gw::SessionGwThdContext *session;
  exasol_gw::SessionGwConnection *connection;
  exasol_gw::SessionGwOptions options;
  std::uint64_t cursor_id;
  bool cursor_registered;
  exasol_gw::SessionGwNativeFetchResult current_batch;
  std::size_t current_row;
  bool end_of_cursor;
  std::uint32_t fetch_rows;
  std::vector<exasol_gw::ArrowColumnKind> column_kinds;
  std::vector<std::size_t> selected_field_indices;
  std::vector<exasol_gw::SessionGwRowHandle> current_row_handles;
  exasol_gw::SessionGwRowHandle last_row_handle_;
};

class ha_exasol_gw_pushdown_handler_base
{
protected:
  explicit ha_exasol_gw_pushdown_handler_base(TABLE *tbl_arg)
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
  ha_exasol_gw_cursor *cursor;
  std::string query_generation_error;
};

class ha_exasol_gw_derived_handler: public derived_handler,
                                       public ha_exasol_gw_pushdown_handler_base
{
public:
  ha_exasol_gw_derived_handler(THD *thd_arg, TABLE_LIST *derived_arg, TABLE *tbl_arg);
  ~ha_exasol_gw_derived_handler() override;

  int init_scan() override;
  int next_row() override { return next_row_(table); }
  int end_scan() override { return end_scan_(); }

private:
  StringBuffer<512> query;
};

class ha_exasol_gw_select_handler: public select_handler,
                                      public ha_exasol_gw_pushdown_handler_base
{
public:
  ha_exasol_gw_select_handler(THD *thd_arg, SELECT_LEX_UNIT *sel_unit, TABLE *tbl);
  ha_exasol_gw_select_handler(THD *thd_arg, SELECT_LEX *sel_lex,
                                 SELECT_LEX_UNIT *sel_unit, TABLE *tbl);
  ~ha_exasol_gw_select_handler() override;

  int init_scan() override;
  int next_row() override;
  int end_scan() override;

private:
  StringBuffer<512> query;
  StringBuffer<512> stage_query;
  StringBuffer<256> staged_order_by;
  bool uses_staged_distinct_pushdown;
};

#endif
