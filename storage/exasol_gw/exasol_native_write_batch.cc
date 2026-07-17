#include "exasol_native_write_batch.h"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace exasol_gw
{
namespace
{

void write_native_u64(std::uint8_t *output, std::uint64_t value)
{
  for (std::size_t index= 0; index < sizeof(value); ++index)
  {
    const unsigned shift= static_cast<unsigned>(56U - index * 8U);
    output[index]= static_cast<std::uint8_t>((value >> shift) & 0xffU);
  }
}

} // namespace

void append_native_u32(std::vector<std::uint8_t> &output, std::uint32_t value)
{
  output.push_back(static_cast<std::uint8_t>((value >> 24U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 16U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>((value >> 8U) & 0xffU));
  output.push_back(static_cast<std::uint8_t>(value & 0xffU));
}

void append_native_u64(std::vector<std::uint8_t> &output, std::uint64_t value)
{
  const std::size_t offset= output.size();
  if (offset > std::numeric_limits<std::size_t>::max() - sizeof(value))
    throw std::runtime_error("native write size word overflows");
  output.resize(offset + sizeof(value));
  write_native_u64(output.data() + offset, value);
}

void append_native_bytes(std::vector<std::uint8_t> &output, const void *data, std::size_t size)
{
  const auto *bytes= static_cast<const std::uint8_t *>(data);
  output.insert(output.end(), bytes, bytes + size);
}

void align_native_batch(std::vector<std::uint8_t> &output)
{
  if (output.size() > std::numeric_limits<std::size_t>::max() -
                          (native_write_batch_alignment - 1U))
    throw std::runtime_error("native write batch alignment overflows");
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
  row_count_= row_count;
  column_count_= column_count;
  appended_columns_= 0;
  append_native_u32(output_, native_write_batch_magic);
  append_native_u32(output_, native_write_batch_version);
  append_native_u32(output_, row_count);
  append_native_u32(output_, column_count);
  append_native_u32(output_, static_cast<std::uint32_t>(native_write_batch_alignment));
  append_native_u32(output_, native_write_batch_size_width);
  append_native_u32(output_, native_write_batch_scalar_endianness);
  append_native_u32(output_, native_write_batch_scalar_abi);
  started_= true;
}

void NativeWriteBatchBuilder::append_null_vector(const std::vector<std::uint8_t> &null_vector)
{
  if (!started_)
    throw std::runtime_error("native write batch builder is not open");
  if (appended_columns_ >= column_count_ || null_vector.size() != row_count_)
    throw std::runtime_error("invalid native write null-vector size or column count");
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
  ++appended_columns_;
}

void NativeWriteBatchBuilder::append_variable_column(const std::vector<std::uint8_t> &null_vector,
                                                     const std::vector<std::size_t> &sizes,
                                                     const std::vector<std::uint8_t> &variable_data)
{
  if (sizes.size() != row_count_)
    throw std::runtime_error("native write variable-size count does not match row count");
  if (variable_data.size() > std::numeric_limits<std::uint32_t>::max())
    throw std::runtime_error("native write variable data is too large");
  std::size_t described_data_bytes= 0;
  for (const std::size_t size: sizes)
  {
    if (size > variable_data.size() - described_data_bytes)
      throw std::runtime_error("native write variable sizes exceed the data extent");
    described_data_bytes+= size;
  }
  if (described_data_bytes != variable_data.size())
    throw std::runtime_error("native write variable sizes do not cover the data extent");
  append_null_vector(null_vector);
  align_native_batch(output_);
  if (sizes.size() > (std::numeric_limits<std::size_t>::max() - output_.size()) /
                         sizeof(std::uint64_t))
    throw std::runtime_error("native write size vector overflows");
  const std::size_t sizes_offset= output_.size();
  output_.resize(sizes_offset + sizes.size() * sizeof(std::uint64_t));
  for (std::size_t row= 0; row < sizes.size(); ++row)
    write_native_u64(output_.data() + sizes_offset + row * sizeof(std::uint64_t),
                     static_cast<std::uint64_t>(sizes[row]));
  append_native_u32(output_, static_cast<std::uint32_t>(variable_data.size()));
  align_native_batch(output_);
  output_.insert(output_.end(), variable_data.begin(), variable_data.end());
  ++appended_columns_;
}

void NativeWriteBatchBuilder::finish()
{
  if (!started_)
    throw std::runtime_error("native write batch builder is not open");
  if (appended_columns_ != column_count_)
    throw std::runtime_error("native write column count does not match batch header");
  align_native_batch(output_);
  started_= false;
}

} // namespace exasol_gw
