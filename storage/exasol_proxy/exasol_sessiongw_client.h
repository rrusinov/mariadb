#ifndef EXASOL_SESSIONGW_CLIENT_INCLUDED
#define EXASOL_SESSIONGW_CLIENT_INCLUDED

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace exasol_proxy
{

enum class SessionGwMessageType: std::uint16_t
{
  hello= 1,
  hello_ok= 2,
  ping= 3,
  pong= 4,
  close= 5,
  ok= 6,
  error= 7,
  open_pushed_query= 12,
  open_cursor_result= 13,
  fetch= 14,
  fetch_result= 15,
  close_cursor= 16,
  open_table_scan= 17
};

struct SessionGwFrame
{
  SessionGwMessageType type= SessionGwMessageType::hello;
  std::uint64_t request_id= 0;
  std::vector<std::uint8_t> payload;
};

class SessionGwError: public std::runtime_error
{
public:
  explicit SessionGwError(const std::string &message): std::runtime_error(message) {}
};

struct SessionGwOptions
{
  std::string host= "127.0.0.1";
  std::uint16_t port= 8563;
  std::string user= "sys";
  std::string password= "exasol";
  std::string tls_mode= "skip_verify"; // verify, skip_verify, plain
  std::string ca_file;
  std::uint32_t fetch_rows= 1024;
};

struct SessionGwOpenCursorResult
{
  std::uint64_t cursor_id= 0;
  std::vector<std::uint8_t> arrow_schema;
};

struct SessionGwFetchResult
{
  std::uint64_t cursor_id= 0;
  bool end_of_cursor= false;
  std::vector<std::uint8_t> arrow_batch;
};

void append_u8(std::vector<std::uint8_t> &out, std::uint8_t value);
void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value);
void append_u64(std::vector<std::uint8_t> &out, std::uint64_t value);
void append_string32(std::vector<std::uint8_t> &out, const std::string &value);
std::uint8_t read_u8(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::uint16_t read_u16(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::uint32_t read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::uint64_t read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::string read_string16(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::vector<std::uint8_t> read_bytes32(const std::vector<std::uint8_t> &bytes, std::size_t &offset);

SessionGwOptions options_from_environment();

class SessionGwConnection
{
public:
  SessionGwConnection();
  ~SessionGwConnection();

  SessionGwConnection(const SessionGwConnection &)= delete;
  SessionGwConnection &operator=(const SessionGwConnection &)= delete;

  void connect_and_enter(const SessionGwOptions &options);
  void close();

  SessionGwOpenCursorResult open_pushed_query(const std::string &sql);
  SessionGwOpenCursorResult open_table_scan(const std::string &schema,
                                            const std::string &table,
                                            const std::vector<std::string> &columns);
  SessionGwFetchResult fetch(std::uint64_t cursor_id,
                             std::uint32_t max_rows,
                             std::uint32_t max_bytes= 0);
  void close_cursor(std::uint64_t cursor_id);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace exasol_proxy

#endif
