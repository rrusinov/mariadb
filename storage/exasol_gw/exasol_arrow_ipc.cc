#include "exasol_arrow_ipc.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <stdexcept>
#include <utility>

namespace exasol_gw
{
namespace
{

void require(bool condition, const char *message)
{
  if (!condition)
    throw std::runtime_error(message);
}

std::uint16_t le16(const std::uint8_t *p)
{
  return static_cast<std::uint16_t>(p[0] | (static_cast<std::uint16_t>(p[1]) << 8U));
}

std::uint32_t le32(const std::uint8_t *p)
{
  return static_cast<std::uint32_t>(p[0]) |
         (static_cast<std::uint32_t>(p[1]) << 8U) |
         (static_cast<std::uint32_t>(p[2]) << 16U) |
         (static_cast<std::uint32_t>(p[3]) << 24U);
}

std::uint64_t le64u(const std::uint8_t *p)
{
  std::uint64_t result= 0;
  for (int i= 7; i >= 0; --i)
    result= (result << 8U) | p[i];
  return result;
}

std::int32_t le32s(const std::uint8_t *p)
{
  std::uint32_t value= le32(p);
  std::int32_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

std::int64_t le64s(const std::uint8_t *p)
{
  std::uint64_t value= le64u(p);
  std::int64_t result;
  std::memcpy(&result, &value, sizeof(result));
  return result;
}

std::size_t align8(std::size_t value)
{
  return (value + 7U) & ~std::size_t(7U);
}

struct FlatTable
{
  const std::uint8_t *base= nullptr;
  std::size_t size= 0;
  std::size_t table= 0;

  std::uint16_t field_offset(std::uint16_t field) const
  {
    require(table + 4 <= size, "truncated flatbuffer table");
    const std::int32_t vtable_distance= le32s(base + table);
    const std::int64_t vtable_location= static_cast<std::int64_t>(table) - vtable_distance;
    require(vtable_location >= 0 && static_cast<std::uint64_t>(vtable_location) < size,
            "invalid flatbuffer vtable distance");
    const std::size_t vtable= static_cast<std::size_t>(vtable_location);
    require(vtable + 4 <= size, "truncated flatbuffer vtable");
    const std::uint16_t vtable_size= le16(base + vtable);
    const std::size_t entry= vtable + 4U + static_cast<std::size_t>(field) * 2U;
    if (entry + 2U > vtable + vtable_size)
      return 0;
    return le16(base + entry);
  }

  bool has(std::uint16_t field) const { return field_offset(field) != 0; }

  std::size_t field_location(std::uint16_t field) const
  {
    const std::uint16_t off= field_offset(field);
    require(off != 0, "missing flatbuffer field");
    require(table + off <= size, "flatbuffer field outside buffer");
    return table + off;
  }

  std::int64_t int64_field(std::uint16_t field, std::int64_t fallback= 0) const
  {
    if (!has(field))
      return fallback;
    const std::size_t loc= field_location(field);
    require(loc + 8 <= size, "truncated flatbuffer int64 field");
    return le64s(base + loc);
  }

  std::uint8_t u8_field(std::uint16_t field, std::uint8_t fallback= 0) const
  {
    if (!has(field))
      return fallback;
    const std::size_t loc= field_location(field);
    require(loc + 1 <= size, "truncated flatbuffer u8 field");
    return base[loc];
  }

  FlatTable table_field(std::uint16_t field) const
  {
    const std::size_t loc= field_location(field);
    require(loc + 4 <= size, "truncated flatbuffer table uoffset");
    const std::size_t target= loc + le32(base + loc);
    require(target < size, "flatbuffer nested table outside buffer");
    return FlatTable{base, size, target};
  }

  std::string string_field(std::uint16_t field) const
  {
    const std::size_t loc= field_location(field);
    require(loc + 4 <= size, "truncated flatbuffer string uoffset");
    const std::size_t target= loc + le32(base + loc);
    require(target + 4 <= size, "flatbuffer string outside buffer");
    const std::uint32_t length= le32(base + target);
    require(length <= size - target - 4U, "truncated flatbuffer string");
    return std::string(reinterpret_cast<const char *>(base + target + 4U), length);
  }

  FlatTable vector_table(std::size_t vector, std::uint32_t index) const
  {
    const std::size_t loc= vector + static_cast<std::size_t>(index) * 4U;
    require(loc + 4 <= size, "truncated flatbuffer table vector");
    const std::size_t target= loc + le32(base + loc);
    require(target < size, "flatbuffer vector table outside buffer");
    return FlatTable{base, size, target};
  }

  std::size_t vector_start(std::uint16_t field, std::uint32_t *length) const
  {
    const std::size_t loc= field_location(field);
    require(loc + 4 <= size, "truncated flatbuffer vector uoffset");
    const std::size_t vec= loc + le32(base + loc);
    require(vec + 4 <= size, "flatbuffer vector outside buffer");
    *length= le32(base + vec);
    require(vec + 4U + *length <= size || *length > 0, "flatbuffer vector header invalid");
    return vec + 4U;
  }
};

struct BufferRef
{
  std::int64_t offset= 0;
  std::int64_t length= 0;
};

struct RecordBatchMeta
{
  std::int64_t rows= 0;
  std::vector<BufferRef> buffers;
};

FlatTable root_table(const std::uint8_t *metadata, std::size_t metadata_size)
{
  require(metadata_size >= 4, "truncated flatbuffer root");
  const std::size_t root= le32(metadata);
  require(root < metadata_size, "flatbuffer root outside metadata");
  return FlatTable{metadata, metadata_size, root};
}

std::string metadata_value(const FlatTable &field, const std::string &key)
{
  if (!field.has(6))
    return std::string();
  std::uint32_t count= 0;
  const std::size_t metadata= field.vector_start(6, &count);
  require(metadata + static_cast<std::size_t>(count) * 4U <= field.size,
          "truncated Arrow field metadata vector");
  for (std::uint32_t index= 0; index < count; ++index)
  {
    const FlatTable entry= field.vector_table(metadata, index);
    if (entry.has(0) && entry.string_field(0) == key)
      return entry.has(1) ? entry.string_field(1) : std::string();
  }
  return std::string();
}

std::int64_t metadata_integer(const FlatTable &field, const std::string &key)
{
  const std::string value= metadata_value(field, key);
  if (value.empty())
    return 0;
  char *end= nullptr;
  const long long parsed= std::strtoll(value.c_str(), &end, 10);
  require(end && *end == '\0', "invalid integer in Arrow field metadata");
  return static_cast<std::int64_t>(parsed);
}

std::vector<ArrowFieldDescription> parse_schema_metadata(
    const std::vector<std::uint8_t> &ipc)
{
  require(ipc.size() >= 4, "truncated Arrow IPC schema message");
  std::size_t offset= 0;
  std::uint32_t metadata_size= le32(ipc.data());
  if (metadata_size == 0xffffffffU)
  {
    require(ipc.size() >= 8, "truncated Arrow IPC schema continuation message");
    metadata_size= le32(ipc.data() + 4);
    offset= 8;
  }
  else
    offset= 4;
  require(metadata_size > 0 && metadata_size <= ipc.size() - offset,
          "invalid Arrow IPC schema metadata size");

  const FlatTable message= root_table(ipc.data() + offset, metadata_size);
  require(message.u8_field(1, 0) == 1, "Arrow IPC message is not a Schema");
  const FlatTable schema= message.table_field(2);
  std::uint32_t field_count= 0;
  const std::size_t fields= schema.vector_start(1, &field_count);
  require(fields + static_cast<std::size_t>(field_count) * 4U <= metadata_size,
          "truncated Arrow schema field vector");

  std::vector<ArrowFieldDescription> result;
  result.reserve(field_count);
  for (std::uint32_t index= 0; index < field_count; ++index)
  {
    const FlatTable field= schema.vector_table(fields, index);
    require(field.has(0), "Arrow schema field has no name");
    ArrowFieldDescription description;
    description.name= field.string_field(0);
    description.nullable= field.u8_field(1, 0) != 0;
    description.exasol_type_id= metadata_value(field, "exasol.type_id");
    require(!description.exasol_type_id.empty(),
            "Arrow schema field has no Exasol type identifier metadata");
    description.precision= static_cast<std::int32_t>(
        metadata_integer(field, "exasol.precision"));
    description.scale= static_cast<std::int32_t>(metadata_integer(field, "exasol.scale"));
    description.char_length= metadata_integer(field, "exasol.char_length");
    result.push_back(std::move(description));
  }
  return result;
}

RecordBatchMeta parse_record_batch_metadata(const std::vector<std::uint8_t> &ipc,
                                            std::size_t *body_offset)
{
  require(ipc.size() >= 4, "truncated Arrow IPC message");
  std::size_t offset= 0;
  std::uint32_t metadata_size= le32(ipc.data());
  if (metadata_size == 0xffffffffU)
  {
    require(ipc.size() >= 8, "truncated Arrow IPC continuation message");
    metadata_size= le32(ipc.data() + 4);
    offset= 8;
  }
  else
    offset= 4;
  require(metadata_size > 0, "empty Arrow IPC metadata");
  require(offset + metadata_size <= ipc.size(), "truncated Arrow IPC metadata");

  const std::uint8_t *metadata= ipc.data() + offset;
  const FlatTable message= root_table(metadata, metadata_size);
  const std::uint8_t header_type= message.u8_field(1, 0);
  require(header_type == 3, "Arrow IPC message is not a RecordBatch");
  const FlatTable record_batch= message.table_field(2);

  RecordBatchMeta meta;
  meta.rows= record_batch.int64_field(0, 0);
  require(meta.rows >= 0, "negative Arrow row count");

  std::uint32_t buffers_count= 0;
  const std::size_t buffers= record_batch.vector_start(2, &buffers_count);
  require(buffers + static_cast<std::size_t>(buffers_count) * 16U <= metadata_size,
          "truncated Arrow buffer vector");
  meta.buffers.reserve(buffers_count);
  for (std::uint32_t i= 0; i < buffers_count; ++i)
  {
    const std::uint8_t *buffer= metadata + buffers + static_cast<std::size_t>(i) * 16U;
    meta.buffers.push_back(BufferRef{le64s(buffer), le64s(buffer + 8)});
  }

  *body_offset= offset + align8(metadata_size);
  require(*body_offset <= ipc.size(), "Arrow IPC body offset outside message");
  return meta;
}

const std::uint8_t *body_ptr(const std::vector<std::uint8_t> &ipc,
                             std::size_t body_offset,
                             const BufferRef &buffer)
{
  require(buffer.offset >= 0 && buffer.length >= 0, "negative Arrow buffer range");
  const std::size_t start= body_offset + static_cast<std::size_t>(buffer.offset);
  const std::size_t length= static_cast<std::size_t>(buffer.length);
  require(start <= ipc.size() && length <= ipc.size() - start, "Arrow buffer outside message body");
  return ipc.data() + start;
}

bool is_null(const std::vector<std::uint8_t> &ipc,
             std::size_t body_offset,
             const BufferRef &validity,
             std::size_t row)
{
  if (validity.length == 0)
    return false;
  const std::uint8_t *bits= body_ptr(ipc, body_offset, validity);
  const std::size_t byte_index= row / 8U;
  if (byte_index >= static_cast<std::size_t>(validity.length))
    return true;
  return (bits[byte_index] & (1U << (row % 8U))) == 0;
}

std::string int64_to_string(std::int64_t value)
{
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%lld", static_cast<long long>(value));
  return buffer;
}

std::string decimal128_to_string(const std::uint8_t *bytes)
{
  unsigned __int128 encoded= 0;
  for (std::size_t index= 0; index < 16U; ++index)
    encoded |= static_cast<unsigned __int128>(bytes[index]) << (index * 8U);
  const bool negative= (bytes[15] & 0x80U) != 0;
  unsigned __int128 magnitude= negative ? (~encoded + 1U) : encoded;

  std::string digits;
  do
  {
    digits.push_back(static_cast<char>('0' + magnitude % 10U));
    magnitude /= 10U;
  }
  while (magnitude != 0);
  std::reverse(digits.begin(), digits.end());

  if (negative)
    digits.insert(0, 1, '-');
  return digits;
}

std::string date32_to_string(std::int32_t days)
{
  // Civil-from-days conversion, Howard Hinnant algorithm.
  long z= static_cast<long>(days) + 719468L;
  const long era= (z >= 0 ? z : z - 146096L) / 146097L;
  const unsigned doe= static_cast<unsigned>(z - era * 146097L);
  const unsigned yoe= (doe - doe / 1460U + doe / 36524U - doe / 146096U) / 365U;
  long y= static_cast<long>(yoe) + era * 400L;
  const unsigned doy= doe - (365U * yoe + yoe / 4U - yoe / 100U);
  const unsigned mp= (5U * doy + 2U) / 153U;
  const unsigned d= doy - (153U * mp + 2U) / 5U + 1U;
  const unsigned m= mp + (mp < 10U ? 3U : static_cast<unsigned>(-9));
  y += (m <= 2U);
  char buffer[32];
  std::snprintf(buffer, sizeof(buffer), "%04ld-%02u-%02u", y, m, d);
  return buffer;
}

std::string timestamp_ns_to_string(std::int64_t ns)
{
  std::int64_t seconds= ns / 1000000000LL;
  std::int64_t nanos= ns % 1000000000LL;
  if (nanos < 0)
  {
    --seconds;
    nanos += 1000000000LL;
  }
  std::time_t t= static_cast<std::time_t>(seconds);
  std::tm tm{};
  gmtime_r(&t, &tm);
  char buffer[64];
  std::snprintf(buffer, sizeof(buffer), "%04d-%02d-%02d %02d:%02d:%02d.%06lld",
                tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                tm.tm_hour, tm.tm_min, tm.tm_sec,
                static_cast<long long>(nanos / 1000LL));
  return buffer;
}

std::size_t buffer_count_for(ArrowColumnKind kind)
{
  return kind == ArrowColumnKind::utf8 ? 3U : 2U;
}

} // namespace

std::vector<ArrowFieldDescription> decode_arrow_schema(
    const std::vector<std::uint8_t> &ipc_message)
{
  return parse_schema_metadata(ipc_message);
}

ArrowRowBatch decode_arrow_record_batch(const std::vector<std::uint8_t> &ipc_message,
                                        const std::vector<ArrowColumnKind> &columns)
{
  ArrowRowBatch batch;
  if (ipc_message.empty())
    return batch;

  std::size_t body_offset= 0;
  const RecordBatchMeta meta= parse_record_batch_metadata(ipc_message, &body_offset);
  batch.rows= static_cast<std::size_t>(meta.rows);
  batch.columns.resize(columns.size());

  std::size_t buffer_index= 0;
  for (std::size_t column= 0; column < columns.size(); ++column)
  {
    const ArrowColumnKind kind= columns[column];
    const std::size_t needed= buffer_count_for(kind);
    require(buffer_index + needed <= meta.buffers.size(), "Arrow RecordBatch has too few buffers");
    const BufferRef validity= meta.buffers[buffer_index++];
    batch.columns[column].resize(batch.rows);

    if (kind == ArrowColumnKind::utf8)
    {
      const BufferRef offsets_ref= meta.buffers[buffer_index++];
      const BufferRef data_ref= meta.buffers[buffer_index++];
      const std::uint8_t *offsets= body_ptr(ipc_message, body_offset, offsets_ref);
      const std::uint8_t *data= body_ptr(ipc_message, body_offset, data_ref);
      require(offsets_ref.length >= static_cast<std::int64_t>((batch.rows + 1U) * 4U),
              "Arrow UTF8 offsets buffer too small");
      for (std::size_t row= 0; row < batch.rows; ++row)
      {
        ArrowCell &cell= batch.columns[column][row];
        cell.is_null= is_null(ipc_message, body_offset, validity, row);
        if (!cell.is_null)
        {
          const std::int32_t begin= le32s(offsets + row * 4U);
          const std::int32_t end= le32s(offsets + (row + 1U) * 4U);
          require(begin >= 0 && end >= begin && end <= data_ref.length, "invalid Arrow UTF8 range");
          cell.value.assign(reinterpret_cast<const char *>(data + begin), static_cast<std::size_t>(end - begin));
        }
      }
      continue;
    }

    const BufferRef values_ref= meta.buffers[buffer_index++];
    const std::uint8_t *values= body_ptr(ipc_message, body_offset, values_ref);
    for (std::size_t row= 0; row < batch.rows; ++row)
    {
      ArrowCell &cell= batch.columns[column][row];
      cell.is_null= is_null(ipc_message, body_offset, validity, row);
      if (cell.is_null)
        continue;
      switch (kind)
      {
      case ArrowColumnKind::signed_int64:
        if (values_ref.length >= static_cast<std::int64_t>(batch.rows * 16U) &&
            values_ref.length % 16 == 0)
        {
          // Exasol SQL numeric/integer expressions commonly arrive as Arrow Decimal128.
          cell.value= decimal128_to_string(values + row * 16U);
        }
        else
        {
          require(values_ref.length >= static_cast<std::int64_t>((row + 1U) * 8U), "Arrow int64 buffer too small");
          cell.value= int64_to_string(le64s(values + row * 8U));
        }
        break;
      case ArrowColumnKind::float64:
      {
        require(values_ref.length >= static_cast<std::int64_t>((row + 1U) * 8U), "Arrow double buffer too small");
        double value;
        const std::uint64_t bits= le64u(values + row * 8U);
        std::memcpy(&value, &bits, sizeof(value));
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "%.17g", value);
        cell.value= buffer;
        break;
      }
      case ArrowColumnKind::boolean:
      {
        const std::size_t byte_index= row / 8U;
        require(byte_index < static_cast<std::size_t>(values_ref.length), "Arrow bool buffer too small");
        cell.value= (values[byte_index] & (1U << (row % 8U))) ? "1" : "0";
        break;
      }
      case ArrowColumnKind::date32:
        require(values_ref.length >= static_cast<std::int64_t>((row + 1U) * 4U), "Arrow date32 buffer too small");
        cell.value= date32_to_string(le32s(values + row * 4U));
        break;
      case ArrowColumnKind::timestamp_ns:
        require(values_ref.length >= static_cast<std::int64_t>((row + 1U) * 8U), "Arrow timestamp buffer too small");
        cell.value= timestamp_ns_to_string(le64s(values + row * 8U));
        break;
      case ArrowColumnKind::decimal128_as_integer_string:
        require(values_ref.length >= static_cast<std::int64_t>((row + 1U) * 16U), "Arrow decimal128 buffer too small");
        cell.value= decimal128_to_string(values + row * 16U);
        break;
      case ArrowColumnKind::utf8:
        break;
      }
    }
  }
  return batch;
}

} // namespace exasol_gw
