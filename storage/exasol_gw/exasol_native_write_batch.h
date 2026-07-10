#ifndef EXASOL_NATIVE_WRITE_BATCH_INCLUDED
#define EXASOL_NATIVE_WRITE_BATCH_INCLUDED

#include <cstddef>
#include <cstdint>
#include <vector>

namespace exasol_gw
{

inline constexpr std::uint32_t native_write_batch_magic= 0x53475742U;
inline constexpr std::uint32_t native_write_batch_version= 1U;
inline constexpr std::size_t native_write_batch_alignment= alignof(std::max_align_t);
inline constexpr std::uint8_t native_write_not_null= 0x00U;
inline constexpr std::uint8_t native_write_null= 0xffU;

void append_native_u32(std::vector<std::uint8_t> &output, std::uint32_t value);
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
  bool started_= false;
};

} // namespace exasol_gw

#endif
