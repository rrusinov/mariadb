#include "exasol_native_write_batch.h"

#include <algorithm>
#include <stdexcept>

namespace exasol_gw
{

void append_native_u32(std::vector<std::uint8_t> &output, std::uint32_t value)
{
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_native_bytes(std::vector<std::uint8_t> &output, const void *data, std::size_t size)
{
  const auto *bytes= static_cast<const std::uint8_t *>(data);
  output.insert(output.end(), bytes, bytes + size);
}

void align_native_batch(std::vector<std::uint8_t> &output)
{
  const std::size_t aligned_size=
      (output.size() + native_write_batch_alignment - 1U) & ~(native_write_batch_alignment - 1U);
  output.resize(aligned_size, 0U);
}

void NativeWriteBatchBuilder::begin(std::uint32_t row_count,
                                    std::uint32_t column_count,
                                    bool clear_output)
{
  if (clear_output)
    output_.clear();
  append_native_u32(output_, native_write_batch_magic);
  append_native_u32(output_, native_write_batch_version);
  append_native_u32(output_, row_count);
  append_native_u32(output_, column_count);
  started_= true;
}

void NativeWriteBatchBuilder::append_null_vector(const std::vector<std::uint8_t> &null_vector)
{
  if (!started_)
    throw std::runtime_error("native write batch builder is not open");
  if (!std::all_of(null_vector.begin(), null_vector.end(), [](std::uint8_t value) {
        return value == native_write_not_null || value == native_write_null;
      }))
    throw std::runtime_error("invalid native write null vector");
  align_native_batch(output_);
  output_.insert(output_.end(), null_vector.begin(), null_vector.end());
}

void NativeWriteBatchBuilder::append_fixed_column(const std::vector<std::uint8_t> &null_vector,
                                                  const std::vector<std::uint8_t> &fixed_data)
{
  append_null_vector(null_vector);
  align_native_batch(output_);
  output_.insert(output_.end(), fixed_data.begin(), fixed_data.end());
}

void NativeWriteBatchBuilder::append_variable_column(const std::vector<std::uint8_t> &null_vector,
                                                     const std::vector<std::size_t> &sizes,
                                                     const std::vector<std::uint8_t> &variable_data)
{
  append_null_vector(null_vector);
  align_native_batch(output_);
  append_native_bytes(output_, sizes.data(), sizes.size() * sizeof(std::size_t));
  append_native_u32(output_, static_cast<std::uint32_t>(variable_data.size()));
  align_native_batch(output_);
  output_.insert(output_.end(), variable_data.begin(), variable_data.end());
}

void NativeWriteBatchBuilder::finish()
{
  if (!started_)
    throw std::runtime_error("native write batch builder is not open");
  align_native_batch(output_);
  started_= false;
}

} // namespace exasol_gw
