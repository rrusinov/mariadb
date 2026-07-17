#ifndef EXASOL_NATIVE_WRITE_BATCH_INCLUDED
#define EXASOL_NATIVE_WRITE_BATCH_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

namespace exasol_gw
{

inline constexpr std::uint32_t native_write_batch_magic= 0x53475742U;
inline constexpr std::uint32_t native_write_batch_version= 2U;
inline constexpr std::size_t native_write_batch_alignment= 16U;
inline constexpr std::uint32_t native_write_batch_size_width= 8U;
inline constexpr std::uint32_t native_write_batch_scalar_abi= 1U;
inline constexpr std::uint32_t native_write_batch_little_endian= 1U;
inline constexpr std::uint32_t native_write_batch_big_endian= 2U;
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
inline constexpr std::uint32_t native_write_batch_scalar_endianness= native_write_batch_little_endian;
#elif __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
inline constexpr std::uint32_t native_write_batch_scalar_endianness= native_write_batch_big_endian;
#else
#error "EXASOL native write batch requires a known byte order"
#endif
inline constexpr std::uint8_t native_write_not_null= 0x00U;
inline constexpr std::uint8_t native_write_null= 0xffU;

void append_native_u32(std::vector<std::uint8_t> &output, std::uint32_t value);
void append_native_u64(std::vector<std::uint8_t> &output, std::uint64_t value);
void append_native_bytes(std::vector<std::uint8_t> &output, const void *data, std::size_t size);
void align_native_batch(std::vector<std::uint8_t> &output);

class NativeWriteBatchBuilder
{
public:
  explicit NativeWriteBatchBuilder(std::vector<std::uint8_t> &output): output_(output) {}

  void begin(std::uint32_t row_count, std::uint32_t column_count, bool clear_output= true);
  void append_fixed_column(const std::vector<std::uint8_t> &null_vector,
                           const std::vector<std::uint8_t> &fixed_data);
  void append_variable_column(const std::vector<std::uint8_t> &null_vector,
                              const std::vector<std::size_t> &sizes,
                              const std::vector<std::uint8_t> &variable_data);
  void finish();

private:
  void append_null_vector(const std::vector<std::uint8_t> &null_vector);

  std::vector<std::uint8_t> &output_;
  std::uint32_t row_count_= 0;
  std::uint32_t column_count_= 0;
  std::uint32_t appended_columns_= 0;
  bool started_= false;
};

} // namespace exasol_gw

#endif
