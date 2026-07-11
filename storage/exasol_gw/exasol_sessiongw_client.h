#ifndef EXASOL_SESSIONGW_CLIENT_INCLUDED
#define EXASOL_SESSIONGW_CLIENT_INCLUDED

#include <cstddef>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace exasol_gw
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
  describe_table= 8,
  describe_table_result= 9,
  get_table_version= 10,
  get_table_version_result= 11,
  open_pushed_query= 12,
  open_cursor_result= 13,
  fetch= 14,
  fetch_result= 15,
  close_cursor= 16,
  open_table_scan= 17,
  open_table_insert= 18,
  open_table_operation_result= 19,
  insert_rows= 20,
  affected_rows_result= 21,
  close_operation= 22,
  set_autocommit= 23,
  commit= 24,
  rollback= 25,
  open_table_update= 26,
  update_rows= 27,
  open_table_delete= 28,
  delete_rows= 29
};

struct SessionGwFrame
{
  SessionGwMessageType type= SessionGwMessageType::hello;
  std::uint64_t request_id= 0;
  std::vector<std::uint8_t> payload;
};

enum class SessionGwErrorCategory: std::uint16_t
{
  protocol_error= 1,
  authentication_failed= 2,
  not_authorized= 3,
  object_not_found= 4,
  unsupported_type= 5,
  unsupported_operation= 6,
  transaction_conflict= 7,
  constraint_violation= 8,
  resource_limit= 9,
  cursor_not_found= 10,
  internal_error= 11,
  transport_error= 12
};

class SessionGwError: public std::runtime_error
{
public:
  explicit SessionGwError(const std::string &message)
    : std::runtime_error(message), category_(SessionGwErrorCategory::transport_error)
  {}
  SessionGwError(SessionGwErrorCategory category, const std::string &message)
    : std::runtime_error(message), category_(category)
  {}

  SessionGwErrorCategory category() const noexcept { return category_; }

private:
  SessionGwErrorCategory category_;
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

struct SessionGwRowHandle
{
  std::uint64_t row_number= 0;
};

struct SessionGwFetchResult
{
  std::uint64_t cursor_id= 0;
  bool end_of_cursor= false;
  std::vector<std::uint8_t> arrow_batch;
  std::vector<SessionGwRowHandle> row_handles;
};

struct SessionGwDescribeTableResult
{
  std::string schema_name;
  std::string table_name;
  std::string table_version;
  std::vector<std::uint8_t> arrow_schema;
};

struct SessionGwOpenOperationResult
{
  std::uint64_t operation_id= 0;
  std::vector<std::uint8_t> accepted_schema;
};

void append_u8(std::vector<std::uint8_t> &out, std::uint8_t value);
void append_u16(std::vector<std::uint8_t> &out, std::uint16_t value);
void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value);
void append_u64(std::vector<std::uint8_t> &out, std::uint64_t value);
void append_string16(std::vector<std::uint8_t> &out, const std::string &value);
void append_string32(std::vector<std::uint8_t> &out, const std::string &value);
void append_bytes32(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &bytes);
std::uint8_t read_u8(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::uint16_t read_u16(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::uint32_t read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::uint64_t read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::string read_string16(const std::vector<std::uint8_t> &bytes, std::size_t &offset);
std::vector<std::uint8_t> read_bytes32(const std::vector<std::uint8_t> &bytes, std::size_t &offset);

SessionGwOptions options_from_environment();
void execute_sql(const SessionGwOptions &options, const std::string &sql);

class SessionGwConnection
{
public:
  SessionGwConnection();
  ~SessionGwConnection();

  SessionGwConnection(const SessionGwConnection &)= delete;
  SessionGwConnection &operator=(const SessionGwConnection &)= delete;

  void connect_and_enter(const SessionGwOptions &options);
  void execute_sql_command(const SessionGwOptions &options, const std::string &sql);
  void close();

  SessionGwDescribeTableResult describe_table(const std::string &schema,
                                              const std::string &table);
  std::string get_table_version(const std::string &schema, const std::string &table);
  SessionGwOpenCursorResult open_pushed_query(const std::string &sql);
  SessionGwOpenCursorResult open_table_scan(const std::string &schema,
                                            const std::string &table,
                                            const std::vector<std::string> &columns,
                                            bool include_row_handles= false,
                                            const std::vector<SessionGwRowHandle> &row_handles= {});
  SessionGwOpenOperationResult open_table_insert(const std::string &schema,
                                                 const std::string &table,
                                                 const std::vector<std::string> &columns,
                                                 std::uint32_t max_rows_per_batch,
                                                 const std::vector<std::uint8_t> &arrow_schema);
  std::uint64_t insert_rows(std::uint64_t operation_id,
                            std::uint32_t row_count,
                            const std::vector<std::uint8_t> &native_batch);
  SessionGwOpenOperationResult open_table_update(const std::string &schema,
                                                 const std::string &table,
                                                 const std::vector<std::string> &columns,
                                                 std::uint32_t max_rows_per_batch,
                                                 const std::vector<std::uint8_t> &arrow_schema);
  std::uint64_t update_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles,
                            const std::vector<std::uint8_t> &native_batch);
  SessionGwOpenOperationResult open_table_delete(const std::string &schema,
                                                 const std::string &table,
                                                 std::uint32_t max_rows_per_batch);
  std::uint64_t delete_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles);
  void close_operation(std::uint64_t operation_id);
  void set_autocommit(bool enabled);
  void commit();
  void rollback();
  SessionGwFetchResult fetch(std::uint64_t cursor_id,
                             std::uint32_t max_rows,
                             std::uint32_t max_bytes= 0);
  void close_cursor(std::uint64_t cursor_id);

private:
  class Impl;
  std::unique_ptr<Impl> impl_;
};

} // namespace exasol_gw

#endif
