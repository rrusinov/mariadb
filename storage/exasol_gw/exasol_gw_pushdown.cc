#define MYSQL_SERVER 1
#include <my_global.h>
#include <mysql/plugin.h>

#include "handler.h"
#include "field.h"
#include "table.h"
#include "sql_class.h"
#include "tztime.h"
#include "sql_lex.h"
#include "my_decimal.h"

#include "exasol_gw_pushdown.h"
#include "exasol_gw_sql_generator.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <limits>
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

std::uint32_t little_u32(const std::uint8_t *bytes)
{
  std::uint32_t value= 0;
  for (unsigned index= 0; index < 4U; ++index)
    value |= static_cast<std::uint32_t>(bytes[index]) << (index * 8U);
  return value;
}

std::uint64_t little_u64(const std::uint8_t *bytes)
{
  std::uint64_t value= 0;
  for (unsigned index= 0; index < 8U; ++index)
    value |= static_cast<std::uint64_t>(bytes[index]) << (index * 8U);
  return value;
}

std::int64_t little_i64(const std::uint8_t *bytes)
{
  return static_cast<std::int64_t>(little_u64(bytes));
}

void civil_from_days(std::int64_t days, MYSQL_TIME *value)
{
  std::int64_t z= days + 719468;
  const std::int64_t era= (z >= 0 ? z : z - 146096) / 146097;
  const unsigned doe= static_cast<unsigned>(z - era * 146097);
  const unsigned yoe= (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  std::int64_t year= static_cast<std::int64_t>(yoe) + era * 400;
  const unsigned doy= doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp= (5U * doy + 2U) / 153U;
  value->day= doy - (153U * mp + 2U) / 5U + 1U;
  value->month= mp + (mp < 10U ? 3U : static_cast<unsigned>(-9));
  year += value->month <= 2U;
  if (year < 0 || year > 9999)
    throw std::runtime_error("SessionGW temporal value is outside MariaDB range");
  value->year= static_cast<unsigned>(year);
}

MYSQL_TIME native_date(std::int32_t days)
{
  MYSQL_TIME value{};
  civil_from_days(days, &value);
  value.time_type= MYSQL_TIMESTAMP_DATE;
  return value;
}

MYSQL_TIME native_timestamp(Field *field, std::int64_t nanoseconds)
{
  std::int64_t seconds= nanoseconds / 1000000000LL;
  std::int64_t nanos= nanoseconds % 1000000000LL;
  if (nanos < 0) { --seconds; nanos += 1000000000LL; }
  MYSQL_TIME value{};
  if (field->real_type() == MYSQL_TYPE_TIMESTAMP || field->real_type() == MYSQL_TYPE_TIMESTAMP2)
  {
    THD *thd= field->table ? field->table->in_use : nullptr;
    if (!thd || !thd->variables.time_zone)
      throw std::runtime_error("MariaDB session timezone is unavailable for TIMESTAMP conversion");
    if (seconds < std::numeric_limits<my_time_t>::min() ||
        seconds > std::numeric_limits<my_time_t>::max())
      throw std::runtime_error("SessionGW UTC timestamp is outside MariaDB TIMESTAMP range");
    thd->variables.time_zone->gmt_sec_to_TIME(&value, static_cast<my_time_t>(seconds));
  }
  else
  {
    std::int64_t days= seconds / 86400LL;
    std::int64_t day_seconds= seconds % 86400LL;
    if (day_seconds < 0) { --days; day_seconds += 86400LL; }
    civil_from_days(days, &value);
    value.hour= static_cast<unsigned>(day_seconds / 3600LL);
    value.minute= static_cast<unsigned>((day_seconds % 3600LL) / 60LL);
    value.second= static_cast<unsigned>(day_seconds % 60LL);
  }
  value.second_part= static_cast<unsigned long>(nanos / 1000LL);
  value.time_type= MYSQL_TIMESTAMP_DATETIME;
  return value;
}

my_decimal native_decimal(const std::uint8_t *bytes, unsigned width, unsigned scale)
{
  unsigned __int128 encoded= 0;
  for (unsigned index= 0; index < width; ++index)
    encoded |= static_cast<unsigned __int128>(bytes[index]) << (index * 8U);
  const bool negative= (bytes[width - 1U] & 0x80U) != 0U;
  if (negative && width < 16U)
    encoded |= (~static_cast<unsigned __int128>(0U)) << (width * 8U);
  unsigned __int128 magnitude= negative ? (~encoded + 1U) : encoded;
  unsigned digits= 1U;
  for (unsigned __int128 copy= magnitude; copy >= 10U; copy /= 10U) ++digits;
  const unsigned integer_digits= digits > scale ? digits - scale : 1U;
  const unsigned integer_groups= (integer_digits + 8U) / 9U;
  const unsigned fractional_groups= (scale + 8U) / 9U;
  const unsigned groups= integer_groups + fractional_groups;
  if (groups > DECIMAL_BUFF_LENGTH)
    throw std::runtime_error("SessionGW decimal exceeds MariaDB decimal buffer");

  my_decimal result;
  result.sign(negative);
  result.intg= integer_digits;
  result.frac= scale;
  std::fill(result.buf, result.buf + DECIMAL_BUFF_LENGTH, 0);
  int target= static_cast<int>(groups) - 1;
  const unsigned partial_fraction= scale % 9U;
  if (partial_fraction != 0U)
  {
    unsigned __int128 divisor= 1U;
    for (unsigned index= 0; index < partial_fraction; ++index) divisor *= 10U;
    unsigned __int128 padding= 1U;
    for (unsigned index= partial_fraction; index < 9U; ++index) padding *= 10U;
    result.buf[target--]= static_cast<decimal_digit_t>((magnitude % divisor) * padding);
    magnitude /= divisor;
  }
  while (target >= 0)
  {
    result.buf[target--]= static_cast<decimal_digit_t>(magnitude % 1000000000U);
    magnitude /= 1000000000U;
  }
  if (magnitude != 0U)
    throw std::runtime_error("SessionGW decimal magnitude exceeds MariaDB precision");
  return result;
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
    end_of_cursor(false),
    fetch_rows(1024)
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
  session->record_projection(selected_field_indices.size(), field_index);
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
  session->record_projection(selected_field_indices.size(), field_index);
  return columns;
}

void ha_exasol_gw_cursor::select_fetch_rows(TABLE *table_arg, bool include_row_handles)
{
  if (options.fetch_rows != 0U)
  {
    fetch_rows= options.fetch_rows;
    return;
  }
  // Narrow row-location scans stay at the proven 1K lifecycle batch. Wide
  // scans target about 4 MiB from projected MariaDB width, bounded to avoid
  // request fan-out and oversized frames.
  std::uint64_t projected_width= 0;
  for (const std::size_t index : selected_field_indices)
  {
    const std::uint64_t width= table_arg->field[index]->pack_length();
    projected_width += std::max<std::uint64_t>(1U, std::min<std::uint64_t>(width, 256U));
  }
  projected_width= std::max<std::uint64_t>(projected_width, 1U);
  if (include_row_handles && projected_width < 128U)
  {
    fetch_rows= 1024U;
    return;
  }
  const std::uint64_t target_rows= (4U * 1024U * 1024U) / projected_width;
  fetch_rows= static_cast<std::uint32_t>(
      std::max<std::uint64_t>(1024U, std::min<std::uint64_t>(10000U, target_rows)));
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
    select_fetch_rows(table_arg, false);
    exasol_gw::SessionGwOpenCursorResult opened=
        connection->open_pushed_query(query_text ? std::string(query_text) : std::string());
    cursor_id= opened.cursor_id;
    session->read_cursor_opened();
    cursor_registered= true;
    current_batch= exasol_gw::SessionGwNativeFetchResult();
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
    select_fetch_rows(table_arg, include_row_handles);
    exasol_gw::SessionGwOpenCursorResult opened=
        connection->open_table_scan(table_schema_name(table_arg), table_object_name(table_arg), columns, include_row_handles);
    cursor_id= opened.cursor_id;
    session->read_cursor_opened();
    cursor_registered= true;
    current_batch= exasol_gw::SessionGwNativeFetchResult();
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

int ha_exasol_gw_cursor::fetch_next_batch(char *error_buffer, unsigned long error_buffer_size)
{
  try
  {
    while (!end_of_cursor)
    {
      current_batch= connection->fetch_native(cursor_id, fetch_rows, 0);
      end_of_cursor= current_batch.end_of_cursor;
      std::uint64_t native_bytes= 0;
      for (const auto &column : current_batch.columns)
        native_bytes += column.nulls_size + column.fixed_data_size +
                        column.sizes_size + column.variable_data_size;
      session->record_fetch(current_batch.row_count, native_bytes, 0U);
      current_row_handles= current_batch.row_handles;
      current_row= 0;
      if (current_batch.row_count > 0)
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

int ha_exasol_gw_cursor::materialize_row(
    TABLE *table_arg,
    const exasol_gw::SessionGwNativeFetchResult &batch,
    const std::vector<exasol_gw::SessionGwRowHandle> &row_handles,
    std::size_t row,
    unsigned char *,
    char *error_buffer,
    unsigned long error_buffer_size)
{
  if (!table_arg || row >= batch.row_count)
    return HA_ERR_END_OF_FILE;
  try
  {
    const auto materialize_started= options.instrumentation_enabled
        ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
    DbugWriteSetGuard write_set_guard(table_arg);
    if (batch.columns.size() != selected_field_indices.size())
      throw std::runtime_error("SessionGW native column count does not match projected MariaDB fields");
    for (std::size_t column= 0; column < selected_field_indices.size(); ++column)
    {
      Field *field= table_arg->field[selected_field_indices[column]];
      const exasol_gw::SessionGwNativeColumn &values= batch.columns[column];
      if (values.nulls == nullptr || row >= values.nulls_size)
        throw std::runtime_error("SessionGW native NULL vector is truncated");
      if (values.nulls[row] == 0xffU)
      {
        field->set_null();
        continue;
      }
      if (values.nulls[row] != 0x00U)
        throw std::runtime_error("SessionGW native NULL vector contains an invalid value");
      field->set_notnull();
      switch (values.kind)
      {
      case exasol_gw::SessionGwNativeKind::boolean:
        field->store(static_cast<longlong>(values.fixed_data[row]), true);
        break;
      case exasol_gw::SessionGwNativeKind::int64:
        field->store(static_cast<longlong>(little_i64(values.fixed_data + row * 8U)),
                     (field->flags & UNSIGNED_FLAG) != 0U);
        break;
      case exasol_gw::SessionGwNativeKind::double64:
      {
        const std::uint64_t bits= little_u64(values.fixed_data + row * 8U);
        double value= 0;
        std::memcpy(&value, &bits, sizeof(value));
        field->store(value);
        break;
      }
      case exasol_gw::SessionGwNativeKind::decimal32:
      case exasol_gw::SessionGwNativeKind::decimal64:
      case exasol_gw::SessionGwNativeKind::decimal128:
      {
        const unsigned width= values.kind == exasol_gw::SessionGwNativeKind::decimal32 ? 4U :
            (values.kind == exasol_gw::SessionGwNativeKind::decimal64 ? 8U : 16U);
        my_decimal value= native_decimal(values.fixed_data + row * width, width,
                                         static_cast<unsigned>(values.scale));
        field->store_decimal(&value);
        break;
      }
      case exasol_gw::SessionGwNativeKind::date32:
      {
        MYSQL_TIME value= native_date(static_cast<std::int32_t>(
            little_u32(values.fixed_data + row * 4U)));
        field->store_time_dec(&value, field->decimals());
        break;
      }
      case exasol_gw::SessionGwNativeKind::timestamp_ns:
      {
        MYSQL_TIME value= native_timestamp(field, little_i64(values.fixed_data + row * 8U));
        field->store_time_dec(&value, field->decimals());
        break;
      }
      case exasol_gw::SessionGwNativeKind::utf8:
      {
        if (values.variable_offsets.size() != batch.row_count + 1U)
          throw std::runtime_error("SessionGW native string offsets are invalid");
        const std::uint64_t begin= values.variable_offsets[row];
        const std::uint64_t end= values.variable_offsets[row + 1U];
        if (begin > end || end > values.variable_data_size)
          throw std::runtime_error("SessionGW native string extent is invalid");
        const char *data= begin == end ? "" :
            reinterpret_cast<const char *>(values.variable_data + begin);
        field->store(data, static_cast<std::size_t>(end - begin), &my_charset_bin);
        break;
      }
      default:
        throw std::runtime_error("SessionGW native column kind is unsupported");
      }
    }
    if (row < row_handles.size())
      last_row_handle_= row_handles[row];
    if (options.instrumentation_enabled)
      session->record_row_materialize(static_cast<std::uint64_t>(
          std::chrono::duration_cast<std::chrono::nanoseconds>(
              std::chrono::steady_clock::now() - materialize_started).count()));
    return 0;
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "materializing EXASOL cursor row");
  }
}

int ha_exasol_gw_cursor::materialize_current_row(TABLE *table_arg,
                                                    unsigned char *record,
                                                    char *error_buffer,
                                                    unsigned long error_buffer_size)
{
  const int rc= materialize_row(table_arg, current_batch, current_row_handles, current_row,
                                record, error_buffer, error_buffer_size);
  if (rc == 0)
    ++current_row;
  return rc;
}

int ha_exasol_gw_cursor::fetch_row(TABLE *table_arg,
                                      unsigned char *record,
                                      char *error_buffer,
                                      unsigned long error_buffer_size)
{
  if (current_row >= current_batch.row_count)
  {
    const int rc= fetch_next_batch(error_buffer, error_buffer_size);
    if (rc != 0)
      return rc;
  }
  return materialize_current_row(table_arg, record, error_buffer, error_buffer_size);
}

int ha_exasol_gw_cursor::fetch_positioned_row(
    TABLE *table_arg,
    const exasol_gw::SessionGwRowHandle &row_handle,
    unsigned char *record,
    char *error_buffer,
    unsigned long error_buffer_size)
{
  try
  {
    for (std::size_t row= 0; row < current_row_handles.size(); ++row)
    {
      if (current_row_handles[row].row_number == row_handle.row_number)
      {
        session->record_positioned_cache_hit();
        return materialize_row(table_arg, current_batch, current_row_handles, row,
                               record, error_buffer, error_buffer_size);
      }
    }

    const std::vector<exasol_gw::SessionGwRowHandle> row_handles{row_handle};
    // A miss is a dedicated vector positioned-read message, never a cursor lifecycle.
    const exasol_gw::SessionGwNativeFetchResult fetched=
        connection->fetch_positioned_rows_native(cursor_id, row_handles);
    std::uint64_t native_bytes= 0;
    for (const auto &column : fetched.columns)
      native_bytes += column.nulls_size + column.fixed_data_size +
                      column.sizes_size + column.variable_data_size;
    session->record_fetch(fetched.row_count, native_bytes, 0U, true);
    if (fetched.row_count == 0)
      return HA_ERR_END_OF_FILE;
    if (fetched.row_count != 1)
      throw std::runtime_error("SessionGW positioned fetch returned an unexpected row count");
    return materialize_row(table_arg, fetched, row_handles, 0, record, error_buffer, error_buffer_size);
  }
  catch (...)
  {
    return copy_current_exception(error_buffer, error_buffer_size,
                                  "fetching EXASOL positioned row");
  }
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

ha_exasol_gw_derived_handler::~ha_exasol_gw_derived_handler()
{
  // MariaDB may destroy a derived handler immediately after init_scan()
  // fails, without calling end_scan(). Keep cursor ownership symmetric with
  // the select handler so every early-delete path closes remote accounting.
  (void) end_scan_();
}

int ha_exasol_gw_derived_handler::init_scan()
{
  try
  {
    const int rc= init_scan_(thd, table, query.ptr(), false);
    if (rc != 0)
      return rc;
    DBUG_EXECUTE_IF("exasol_gw_derived_after_cursor_open_oom", throw std::bad_alloc(););
    return 0;
  }
  catch (...)
  {
    return report_pushdown_exception("initializing EXASOL derived pushdown");
  }
}

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
