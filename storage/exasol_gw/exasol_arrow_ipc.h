#ifndef EXASOL_ARROW_IPC_INCLUDED
#define EXASOL_ARROW_IPC_INCLUDED

#include <cstdint>
#include <string>
#include <vector>

namespace exasol_gw
{

enum class ArrowColumnKind
{
  signed_int64,
  float64,
  utf8,
  boolean,
  date32,
  timestamp_ns,
  decimal128_as_integer_string
};

struct ArrowCell
{
  bool is_null= true;
  std::string value;
};

struct ArrowRowBatch
{
  std::vector<std::vector<ArrowCell> > columns;
  std::size_t rows= 0;
};

ArrowRowBatch decode_arrow_record_batch(const std::vector<std::uint8_t> &ipc_message,
                                        const std::vector<ArrowColumnKind> &columns);

} // namespace exasol_gw

#endif
