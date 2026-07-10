#include "exasol_sessiongw_client.h"

#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/sha.h>
#include <openssl/ssl.h>
#include <openssl/err.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <netdb.h>
#include <random>
#include <sstream>
#include <sys/socket.h>
#include <unistd.h>

namespace exasol_gw
{
namespace
{

constexpr std::uint32_t frame_magic= 0x53475731U;
constexpr std::uint16_t protocol_version_v1= 1;
constexpr std::size_t frame_header_size= 24;
constexpr const char *websocket_guid= "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

void require(bool condition, const std::string &message)
{
  if (!condition)
    throw SessionGwError(message);
}

const std::uint8_t *vec_data(const std::vector<std::uint8_t> &value)
{
  return value.empty() ? nullptr : value.data();
}

std::string env_or_default(const char *name, const char *fallback)
{
  const char *value= std::getenv(name);
  return value && *value ? std::string(value) : std::string(fallback);
}

std::uint16_t env_port_or_default(const char *name, std::uint16_t fallback)
{
  const char *value= std::getenv(name);
  if (!value || !*value)
    return fallback;
  const long parsed= std::strtol(value, nullptr, 10);
  if (parsed <= 0 || parsed > 65535)
    return fallback;
  return static_cast<std::uint16_t>(parsed);
}

std::uint32_t env_u32_or_default(const char *name, std::uint32_t fallback)
{
  const char *value= std::getenv(name);
  if (!value || !*value)
    return fallback;
  const long parsed= std::strtol(value, nullptr, 10);
  if (parsed <= 0 || parsed > static_cast<long>(std::numeric_limits<std::uint32_t>::max()))
    return fallback;
  return static_cast<std::uint32_t>(parsed);
}

void check_openssl(int rc, const char *message)
{
  if (rc <= 0)
    throw SessionGwError(message);
}

std::string openssl_error(const char *context)
{
  const unsigned long error= ERR_get_error();
  if (error == 0)
    return context;
  std::array<char, 256> buffer{};
  ERR_error_string_n(error, buffer.data(), buffer.size());
  return std::string(context) + ": " + buffer.data();
}

std::string base64_encode(const std::uint8_t *data, std::size_t size)
{
  BIO *bio= BIO_new(BIO_s_mem());
  BIO *b64= BIO_new(BIO_f_base64());
  if (!bio || !b64)
  {
    BIO_free_all(b64);
    BIO_free_all(bio);
    throw SessionGwError("OpenSSL BIO allocation failed");
  }
  BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
  BIO_push(b64, bio);
  check_openssl(BIO_write(b64, data, static_cast<int>(size)), "OpenSSL base64 write failed");
  check_openssl(BIO_flush(b64), "OpenSSL base64 flush failed");
  BUF_MEM *mem= nullptr;
  BIO_get_mem_ptr(b64, &mem);
  std::string result(mem->data, mem->length);
  BIO_free_all(b64);
  return result;
}

std::string base64_encode(const std::vector<std::uint8_t> &bytes)
{
  return base64_encode(vec_data(bytes), bytes.size());
}

std::string websocket_accept_for_key(const std::string &key)
{
  const std::string input= key + websocket_guid;
  std::array<std::uint8_t, SHA_DIGEST_LENGTH> digest{};
  SHA1(reinterpret_cast<const unsigned char *>(input.data()), input.size(), digest.data());
  return base64_encode(digest.data(), digest.size());
}

std::string random_websocket_key()
{
  std::array<std::uint8_t, 16> bytes{};
  std::random_device rd;
  for (std::uint8_t &byte: bytes)
    byte= static_cast<std::uint8_t>(rd());
  return base64_encode(bytes.data(), bytes.size());
}

std::string json_escape(const std::string &value)
{
  std::string out;
  out.reserve(value.size() + 8);
  for (char ch: value)
  {
    switch (ch)
    {
    case '\\': out += "\\\\"; break;
    case '"': out += "\\\""; break;
    case '\n': out += "\\n"; break;
    case '\r': out += "\\r"; break;
    case '\t': out += "\\t"; break;
    default: out += ch; break;
    }
  }
  return out;
}

std::string json_string_value(const std::string &json, const std::string &key)
{
  const std::string quoted_key= "\"" + key + "\"";
  std::size_t pos= json.find(quoted_key);
  require(pos != std::string::npos, "WebSocket JSON response misses key: " + key);
  pos= json.find(':', pos + quoted_key.size());
  require(pos != std::string::npos, "Malformed WebSocket JSON key: " + key);
  pos= json.find('"', pos + 1);
  require(pos != std::string::npos, "WebSocket JSON value is not a string: " + key);
  ++pos;
  std::string value;
  bool escaped= false;
  for (; pos < json.size(); ++pos)
  {
    const char ch= json[pos];
    if (escaped)
    {
      switch (ch)
      {
      case 'n': value += '\n'; break;
      case 'r': value += '\r'; break;
      case 't': value += '\t'; break;
      default: value += ch; break;
      }
      escaped= false;
      continue;
    }
    if (ch == '\\')
    {
      escaped= true;
      continue;
    }
    if (ch == '"')
      return value;
    value += ch;
  }
  throw SessionGwError("Unterminated WebSocket JSON string: " + key);
}

void require_status_ok(const std::string &json)
{
  const std::string status= json_string_value(json, "status");
  if (status != "ok")
    throw SessionGwError("WebSocket command returned non-ok status: " + json);
}

std::string encrypt_password(const std::string &public_key_pem, const std::string &password)
{
  BIO *bio= BIO_new_mem_buf(public_key_pem.data(), static_cast<int>(public_key_pem.size()));
  if (!bio)
    throw SessionGwError("OpenSSL BIO allocation failed");
  EVP_PKEY *key= PEM_read_bio_PUBKEY(bio, nullptr, nullptr, nullptr);
  BIO_free(bio);
  if (!key)
    throw SessionGwError("Could not parse WebSocket public key PEM");

  EVP_PKEY_CTX *ctx= EVP_PKEY_CTX_new(key, nullptr);
  if (!ctx)
  {
    EVP_PKEY_free(key);
    throw SessionGwError("OpenSSL EVP_PKEY_CTX allocation failed");
  }
  check_openssl(EVP_PKEY_encrypt_init(ctx), "OpenSSL RSA encrypt init failed");
  check_openssl(EVP_PKEY_CTX_set_rsa_padding(ctx, RSA_PKCS1_PADDING), "OpenSSL RSA padding setup failed");

  std::size_t encrypted_size= 0;
  check_openssl(EVP_PKEY_encrypt(ctx, nullptr, &encrypted_size,
                                 reinterpret_cast<const unsigned char *>(password.data()), password.size()),
                "OpenSSL RSA encrypted-size calculation failed");
  std::vector<std::uint8_t> encrypted(encrypted_size);
  check_openssl(EVP_PKEY_encrypt(ctx, encrypted.data(), &encrypted_size,
                                 reinterpret_cast<const unsigned char *>(password.data()), password.size()),
                "OpenSSL RSA encrypt failed");
  encrypted.resize(encrypted_size);
  EVP_PKEY_CTX_free(ctx);
  EVP_PKEY_free(key);
  return base64_encode(encrypted);
}

int connect_tcp(const std::string &host, std::uint16_t port)
{
  addrinfo hints{};
  hints.ai_family= AF_UNSPEC;
  hints.ai_socktype= SOCK_STREAM;
  addrinfo *addresses= nullptr;
  const std::string port_string= std::to_string(port);
  const int gai= getaddrinfo(host.c_str(), port_string.c_str(), &hints, &addresses);
  if (gai != 0)
    throw SessionGwError(gai_strerror(gai));

  int fd= -1;
  for (addrinfo *address= addresses; address; address= address->ai_next)
  {
    fd= socket(address->ai_family, address->ai_socktype, address->ai_protocol);
    if (fd < 0)
      continue;
    if (::connect(fd, address->ai_addr, address->ai_addrlen) == 0)
      break;
    ::close(fd);
    fd= -1;
  }
  freeaddrinfo(addresses);
  if (fd < 0)
    throw SessionGwError("Could not connect TCP socket");
  return fd;
}

std::uint64_t read_big_endian(const std::uint8_t *bytes, std::size_t size)
{
  std::uint64_t result= 0;
  for (std::size_t i= 0; i < size; ++i)
    result= (result << 8U) | bytes[i];
  return result;
}

void append_big_endian(std::vector<std::uint8_t> &out, std::uint64_t value, std::size_t size)
{
  for (std::size_t i= 0; i < size; ++i)
  {
    const unsigned shift= static_cast<unsigned>((size - i - 1U) * 8U);
    out.push_back(static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_mask(std::vector<std::uint8_t> &out, std::array<std::uint8_t, 4> &mask)
{
  std::random_device rd;
  for (std::uint8_t &byte: mask)
  {
    byte= static_cast<std::uint8_t>(rd());
    out.push_back(byte);
  }
}

void append_masked_byte(std::vector<std::uint8_t> &out,
                        const std::array<std::uint8_t, 4> &mask,
                        std::size_t &index,
                        std::uint8_t value)
{
  out.push_back(value ^ mask[index % mask.size()]);
  ++index;
}

void append_masked_big_endian(std::vector<std::uint8_t> &out,
                              const std::array<std::uint8_t, 4> &mask,
                              std::size_t &index,
                              std::uint64_t value,
                              std::size_t size)
{
  for (std::size_t i= 0; i < size; ++i)
  {
    const unsigned shift= static_cast<unsigned>((size - i - 1U) * 8U);
    append_masked_byte(out, mask, index, static_cast<std::uint8_t>((value >> shift) & 0xffU));
  }
}

void append_masked_payload(std::vector<std::uint8_t> &out,
                           const std::array<std::uint8_t, 4> &mask,
                           std::size_t &index,
                           const std::uint8_t *payload,
                           std::size_t size)
{
  for (std::size_t i= 0; i < size; ++i)
    append_masked_byte(out, mask, index, payload[i]);
}

std::vector<std::uint8_t> encode_sessiongw_frame(SessionGwMessageType type,
                                                 std::uint64_t request_id,
                                                 const std::vector<std::uint8_t> &payload)
{
  if (payload.size() > std::numeric_limits<std::uint32_t>::max())
    throw SessionGwError("SessionGW frame payload is too large");
  std::vector<std::uint8_t> out;
  out.reserve(frame_header_size + payload.size());
  append_big_endian(out, frame_magic, 4);
  append_big_endian(out, protocol_version_v1, 2);
  append_big_endian(out, static_cast<std::uint16_t>(type), 2);
  append_big_endian(out, 0, 2);
  append_big_endian(out, 0, 2);
  append_big_endian(out, request_id, 8);
  append_big_endian(out, payload.size(), 4);
  out.insert(out.end(), payload.begin(), payload.end());
  return out;
}

SessionGwFrame decode_sessiongw_frame(const std::vector<std::uint8_t> &bytes)
{
  require(bytes.size() >= frame_header_size, "SessionGW frame shorter than header");
  require(read_big_endian(bytes.data(), 4) == frame_magic, "SessionGW frame has invalid magic");
  require(read_big_endian(bytes.data() + 4, 2) == protocol_version_v1, "SessionGW frame has unsupported version");
  const std::uint32_t payload_length= static_cast<std::uint32_t>(read_big_endian(bytes.data() + 20, 4));
  require(bytes.size() == frame_header_size + payload_length, "SessionGW frame payload length mismatch");
  SessionGwFrame frame;
  frame.type= static_cast<SessionGwMessageType>(read_big_endian(bytes.data() + 6, 2));
  frame.request_id= read_big_endian(bytes.data() + 12, 8);
  frame.payload.assign(bytes.begin() + static_cast<std::ptrdiff_t>(frame_header_size), bytes.end());
  return frame;
}

void throw_if_error_frame(const SessionGwFrame &frame)
{
  if (frame.type != SessionGwMessageType::error)
    return;
  std::size_t offset= 0;
  const std::uint16_t category= read_u16(frame.payload, offset);
  const std::string message= read_string16(frame.payload, offset);
  std::ostringstream out;
  out << "SessionGW error " << category << ": " << message;
  throw SessionGwError(out.str());
}

} // namespace

void append_u8(std::vector<std::uint8_t> &out, std::uint8_t value)
{
  out.push_back(value);
}

void append_u16(std::vector<std::uint8_t> &out, std::uint16_t value)
{
  append_big_endian(out, value, 2);
}

void append_u32(std::vector<std::uint8_t> &out, std::uint32_t value)
{
  append_big_endian(out, value, 4);
}

void append_u64(std::vector<std::uint8_t> &out, std::uint64_t value)
{
  append_big_endian(out, value, 8);
}

void append_string16(std::vector<std::uint8_t> &out, const std::string &value)
{
  if (value.size() > std::numeric_limits<std::uint16_t>::max())
    throw SessionGwError("SessionGW string16 value too large");
  append_u16(out, static_cast<std::uint16_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void append_string32(std::vector<std::uint8_t> &out, const std::string &value)
{
  append_u32(out, static_cast<std::uint32_t>(value.size()));
  out.insert(out.end(), value.begin(), value.end());
}

void append_bytes32(std::vector<std::uint8_t> &out, const std::vector<std::uint8_t> &bytes)
{
  append_u32(out, static_cast<std::uint32_t>(bytes.size()));
  out.insert(out.end(), bytes.begin(), bytes.end());
}

std::uint8_t read_u8(const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
  require(offset < bytes.size(), "truncated u8");
  return bytes[offset++];
}

std::uint16_t read_u16(const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
  require(offset + 2 <= bytes.size(), "truncated u16");
  const std::uint16_t value= static_cast<std::uint16_t>(read_big_endian(bytes.data() + offset, 2));
  offset += 2;
  return value;
}

std::uint32_t read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
  require(offset + 4 <= bytes.size(), "truncated u32");
  const std::uint32_t value= static_cast<std::uint32_t>(read_big_endian(bytes.data() + offset, 4));
  offset += 4;
  return value;
}

std::uint64_t read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
  require(offset + 8 <= bytes.size(), "truncated u64");
  const std::uint64_t value= read_big_endian(bytes.data() + offset, 8);
  offset += 8;
  return value;
}

std::string read_string16(const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
  const std::uint16_t size= read_u16(bytes, offset);
  require(offset + size <= bytes.size(), "truncated string16");
  std::string value(reinterpret_cast<const char *>(bytes.data() + offset), size);
  offset += size;
  return value;
}

std::vector<std::uint8_t> read_bytes32(const std::vector<std::uint8_t> &bytes, std::size_t &offset)
{
  const std::uint32_t size= read_u32(bytes, offset);
  require(offset + size <= bytes.size(), "truncated bytes32");
  std::vector<std::uint8_t> value(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                  bytes.begin() + static_cast<std::ptrdiff_t>(offset + size));
  offset += size;
  return value;
}

SessionGwOptions options_from_environment()
{
  SessionGwOptions options;
  options.host= env_or_default("EXASOL_SESSIONGW_HOST", options.host.c_str());
  options.port= env_port_or_default("EXASOL_SESSIONGW_PORT", options.port);
  options.user= env_or_default("EXASOL_SESSIONGW_USER", options.user.c_str());
  options.password= env_or_default("EXASOL_SESSIONGW_PASSWORD", options.password.c_str());
  options.tls_mode= env_or_default("EXASOL_SESSIONGW_TLS", options.tls_mode.c_str());
  options.ca_file= env_or_default("EXASOL_SESSIONGW_CA_FILE", "");
  options.fetch_rows= env_u32_or_default("EXASOL_SESSIONGW_FETCH_ROWS", options.fetch_rows);
  return options;
}

class SessionGwConnection::Impl
{
public:
  ~Impl() { close(); }

  void connect_and_login(const SessionGwOptions &options)
  {
    close();
    options_= options;
    fd_= connect_tcp(options.host, options.port);
    if (options.tls_mode != "plain")
      enable_tls();
    websocket_upgrade();
    login();
  }

  void connect_and_enter(const SessionGwOptions &options)
  {
    connect_and_login(options);
    send_text("{\"command\":\"enterSessionGateway\",\"protocolVersion\":1}");
    require_status_ok(receive_text());
    send_frame(SessionGwMessageType::hello, {});
    SessionGwFrame hello= receive_frame();
    throw_if_error_frame(hello);
    require(hello.type == SessionGwMessageType::hello_ok, "SessionGW hello failed");
  }

  void execute_sql(const SessionGwOptions &options, const std::string &sql)
  {
    connect_and_login(options);
    send_text("{\"command\":\"execute\",\"sqlText\":\"" + json_escape(sql) + "\"}");
    require_status_ok(receive_text());
    close();
  }

  void close()
  {
    if (fd_ >= 0)
    {
      try
      {
        send_frame(SessionGwMessageType::close, {});
      }
      catch (...)
      {
      }
    }
    if (ssl_)
    {
      SSL_shutdown(ssl_);
      SSL_free(ssl_);
      ssl_= nullptr;
    }
    if (ssl_ctx_)
    {
      SSL_CTX_free(ssl_ctx_);
      ssl_ctx_= nullptr;
    }
    if (fd_ >= 0)
    {
      ::close(fd_);
      fd_= -1;
    }
  }

  SessionGwDescribeTableResult describe_table(const std::string &schema, const std::string &table)
  {
    std::vector<std::uint8_t> payload;
    append_string16(payload, schema);
    append_string16(payload, table);
    SessionGwFrame frame= request(SessionGwMessageType::describe_table,
                                  payload,
                                  SessionGwMessageType::describe_table_result);
    std::size_t offset= 0;
    SessionGwDescribeTableResult result;
    result.schema_name= read_string16(frame.payload, offset);
    result.table_name= read_string16(frame.payload, offset);
    result.table_version= read_string16(frame.payload, offset);
    result.arrow_schema= read_bytes32(frame.payload, offset);
    return result;
  }

  std::string get_table_version(const std::string &schema, const std::string &table)
  {
    std::vector<std::uint8_t> payload;
    append_string16(payload, schema);
    append_string16(payload, table);
    SessionGwFrame frame= request(SessionGwMessageType::get_table_version,
                                  payload,
                                  SessionGwMessageType::get_table_version_result);
    std::size_t offset= 0;
    (void) read_string16(frame.payload, offset);
    (void) read_string16(frame.payload, offset);
    return read_string16(frame.payload, offset);
  }

  SessionGwOpenCursorResult open_pushed_query(const std::string &sql)
  {
    std::vector<std::uint8_t> payload;
    append_string32(payload, sql);
    SessionGwFrame frame= request(SessionGwMessageType::open_pushed_query,
                                  payload,
                                  SessionGwMessageType::open_cursor_result);
    return parse_open_cursor(frame);
  }

  SessionGwOpenCursorResult open_table_scan(const std::string &schema,
                                            const std::string &table,
                                            const std::vector<std::string> &columns,
                                            bool include_row_handles,
                                            const std::vector<SessionGwRowHandle> &row_handles= {})
  {
    std::vector<std::uint8_t> payload;
    append_string32(payload, schema);
    append_string32(payload, table);
    append_u32(payload, static_cast<std::uint32_t>(columns.size()));
    for (const std::string &column: columns)
      append_string32(payload, column);
    append_u8(payload, include_row_handles ? 1 : 0);
    append_u32(payload, static_cast<std::uint32_t>(row_handles.size()));
    for (const SessionGwRowHandle &row_handle: row_handles)
      append_u64(payload, row_handle.row_number);
    SessionGwFrame frame= request(SessionGwMessageType::open_table_scan,
                                  payload,
                                  SessionGwMessageType::open_cursor_result);
    return parse_open_cursor(frame);
  }

  SessionGwOpenOperationResult open_table_insert(const std::string &schema,
                                                 const std::string &table,
                                                 const std::vector<std::string> &columns,
                                                 std::uint32_t max_rows_per_batch,
                                                 const std::vector<std::uint8_t> &arrow_schema)
  {
    std::vector<std::uint8_t> payload;
    append_string32(payload, schema);
    append_string32(payload, table);
    append_u32(payload, static_cast<std::uint32_t>(columns.size()));
    for (const std::string &column: columns)
      append_string32(payload, column);
    append_u32(payload, max_rows_per_batch);
    append_bytes32(payload, arrow_schema);
    SessionGwFrame frame= request(SessionGwMessageType::open_table_insert,
                                  payload,
                                  SessionGwMessageType::open_table_operation_result);
    return parse_open_operation(frame);
  }

  std::uint64_t insert_rows(std::uint64_t operation_id,
                            std::uint32_t row_count,
                            const std::vector<std::uint8_t> &native_batch)
  {
    std::vector<std::uint8_t> payload;
    (void) row_count;
    append_u64(payload, operation_id);
    append_u32(payload, static_cast<std::uint32_t>(native_batch.size()));
    const std::size_t aligned_size= (payload.size() + alignof(std::max_align_t) - 1U) & ~(alignof(std::max_align_t) - 1U);
    payload.resize(aligned_size, 0U);
    payload.insert(payload.end(), native_batch.begin(), native_batch.end());
    SessionGwFrame frame= request(SessionGwMessageType::insert_rows,
                                  payload,
                                  SessionGwMessageType::affected_rows_result);
    return parse_affected_rows(frame);
  }

  void close_operation(std::uint64_t operation_id)
  {
    std::vector<std::uint8_t> payload;
    append_u64(payload, operation_id);
    (void) request(SessionGwMessageType::close_operation, payload, SessionGwMessageType::ok);
  }

  void set_autocommit(bool enabled)
  {
    std::vector<std::uint8_t> payload;
    append_u8(payload, enabled ? 1 : 0);
    (void) request(SessionGwMessageType::set_autocommit, payload, SessionGwMessageType::ok);
  }

  void commit()
  {
    (void) request(SessionGwMessageType::commit, {}, SessionGwMessageType::ok);
  }

  void rollback()
  {
    (void) request(SessionGwMessageType::rollback, {}, SessionGwMessageType::ok);
  }

  SessionGwFetchResult fetch(std::uint64_t cursor_id, std::uint32_t max_rows, std::uint32_t max_bytes)
  {
    std::vector<std::uint8_t> payload;
    append_u64(payload, cursor_id);
    append_u32(payload, max_rows);
    append_u32(payload, max_bytes);
    SessionGwFrame frame= request(SessionGwMessageType::fetch, payload, SessionGwMessageType::fetch_result);
    std::size_t offset= 0;
    SessionGwFetchResult result;
    result.cursor_id= read_u64(frame.payload, offset);
    result.end_of_cursor= read_u8(frame.payload, offset) != 0;
    result.arrow_batch= read_bytes32(frame.payload, offset);
    if (offset < frame.payload.size())
    {
      const std::uint32_t count= read_u32(frame.payload, offset);
      result.row_handles.reserve(count);
      for (std::uint32_t i= 0; i < count; ++i)
        result.row_handles.push_back(SessionGwRowHandle{read_u64(frame.payload, offset)});
    }
    return result;
  }

  void close_cursor(std::uint64_t cursor_id)
  {
    std::vector<std::uint8_t> payload;
    append_u64(payload, cursor_id);
    (void) request(SessionGwMessageType::close_cursor, payload, SessionGwMessageType::ok);
  }

  SessionGwOpenOperationResult open_table_update(const std::string &schema,
                                                 const std::string &table,
                                                 const std::vector<std::string> &columns,
                                                 std::uint32_t max_rows_per_batch,
                                                 const std::vector<std::uint8_t> &arrow_schema)
  {
    std::vector<std::uint8_t> payload;
    append_string32(payload, schema);
    append_string32(payload, table);
    append_u32(payload, static_cast<std::uint32_t>(columns.size()));
    for (const std::string &column: columns)
      append_string32(payload, column);
    append_u32(payload, max_rows_per_batch);
    append_bytes32(payload, arrow_schema);
    SessionGwFrame frame= request(SessionGwMessageType::open_table_update,
                                  payload,
                                  SessionGwMessageType::open_table_operation_result);
    return parse_open_operation(frame);
  }

  std::uint64_t update_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles,
                            const std::vector<std::uint8_t> &native_batch)
  {
    std::vector<std::uint8_t> payload;
    append_u64(payload, operation_id);
    append_row_handles(payload, row_handles);
    append_u32(payload, static_cast<std::uint32_t>(native_batch.size()));
    const std::size_t aligned_size= (payload.size() + alignof(std::max_align_t) - 1U) & ~(alignof(std::max_align_t) - 1U);
    payload.resize(aligned_size, 0U);
    payload.insert(payload.end(), native_batch.begin(), native_batch.end());
    SessionGwFrame frame= request(SessionGwMessageType::update_rows,
                                  payload,
                                  SessionGwMessageType::affected_rows_result);
    return parse_affected_rows(frame);
  }

  SessionGwOpenOperationResult open_table_delete(const std::string &schema,
                                                 const std::string &table,
                                                 std::uint32_t max_rows_per_batch)
  {
    std::vector<std::uint8_t> payload;
    append_string32(payload, schema);
    append_string32(payload, table);
    append_u32(payload, max_rows_per_batch);
    SessionGwFrame frame= request(SessionGwMessageType::open_table_delete,
                                  payload,
                                  SessionGwMessageType::open_table_operation_result);
    return parse_open_operation(frame);
  }

  std::uint64_t delete_rows(std::uint64_t operation_id,
                            const std::vector<SessionGwRowHandle> &row_handles)
  {
    std::vector<std::uint8_t> payload;
    append_u64(payload, operation_id);
    append_row_handles(payload, row_handles);
    SessionGwFrame frame= request(SessionGwMessageType::delete_rows,
                                  payload,
                                  SessionGwMessageType::affected_rows_result);
    return parse_affected_rows(frame);
  }

private:
  void enable_tls()
  {
    SSL_CTX *ctx= SSL_CTX_new(TLS_client_method());
    if (!ctx)
      throw SessionGwError(openssl_error("OpenSSL TLS context allocation failed"));
    ssl_ctx_= ctx;
    if (options_.tls_mode == "verify")
    {
      SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, nullptr);
      if (options_.ca_file.empty())
      {
        if (SSL_CTX_set_default_verify_paths(ctx) != 1)
          throw SessionGwError(openssl_error("OpenSSL could not load default CA paths"));
      }
      else if (SSL_CTX_load_verify_locations(ctx, options_.ca_file.c_str(), nullptr) != 1)
        throw SessionGwError(openssl_error("OpenSSL could not load CA file"));
    }
    else
      SSL_CTX_set_verify(ctx, SSL_VERIFY_NONE, nullptr);

    ssl_= SSL_new(ctx);
    if (!ssl_)
      throw SessionGwError(openssl_error("OpenSSL TLS allocation failed"));
    if (SSL_set_fd(ssl_, fd_) != 1)
      throw SessionGwError(openssl_error("OpenSSL TLS fd setup failed"));
    SSL_set_tlsext_host_name(ssl_, options_.host.c_str());
    if (options_.tls_mode == "verify" && SSL_set1_host(ssl_, options_.host.c_str()) != 1)
      throw SessionGwError(openssl_error("OpenSSL TLS host verification setup failed"));
    if (SSL_connect(ssl_) != 1)
      throw SessionGwError(openssl_error("OpenSSL TLS handshake failed"));
    if (options_.tls_mode == "verify" && SSL_get_verify_result(ssl_) != X509_V_OK)
      throw SessionGwError("OpenSSL TLS certificate verification failed");
  }

  void transport_write(const std::uint8_t *payload, std::size_t size)
  {
    std::size_t written= 0;
    while (written < size)
    {
      int rc= 0;
      if (ssl_)
        rc= SSL_write(ssl_, payload + written, static_cast<int>(size - written));
      else
        rc= static_cast<int>(::send(fd_, payload + written, size - written, 0));
      if (rc <= 0)
        throw SessionGwError(ssl_ ? openssl_error("OpenSSL TLS write failed") : std::strerror(errno));
      written += static_cast<std::size_t>(rc);
    }
  }

  void transport_write(const std::string &text)
  {
    transport_write(reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  }

  void transport_write(const std::vector<std::uint8_t> &bytes)
  {
    transport_write(vec_data(bytes), bytes.size());
  }

  void transport_read(std::uint8_t *payload, std::size_t size)
  {
    std::size_t read= 0;
    while (read < size)
    {
      int rc= 0;
      if (ssl_)
        rc= SSL_read(ssl_, payload + read, static_cast<int>(size - read));
      else
        rc= static_cast<int>(::recv(fd_, payload + read, size - read, 0));
      if (rc <= 0)
        throw SessionGwError(ssl_ ? openssl_error("OpenSSL TLS read failed") : "Socket closed while reading");
      read += static_cast<std::size_t>(rc);
    }
  }

  void websocket_upgrade()
  {
    const std::string key= random_websocket_key();
    std::ostringstream request;
    request << "GET / HTTP/1.1\r\n"
            << "Host: " << options_.host << ':' << options_.port << "\r\n"
            << "Upgrade: websocket\r\n"
            << "Connection: Upgrade\r\n"
            << "Sec-WebSocket-Key: " << key << "\r\n"
            << "Sec-WebSocket-Version: 13\r\n\r\n";
    transport_write(request.str());

    std::string response;
    std::array<std::uint8_t, 1> ch{};
    while (response.find("\r\n\r\n") == std::string::npos)
    {
      transport_read(ch.data(), ch.size());
      response.push_back(static_cast<char>(ch[0]));
      if (response.size() > 16U * 1024U)
        throw SessionGwError("WebSocket HTTP upgrade response is too large");
    }
    if (response.find("101") == std::string::npos ||
        response.find(websocket_accept_for_key(key)) == std::string::npos)
      throw SessionGwError("Invalid WebSocket upgrade response: " + response);
  }

  void login()
  {
    send_text("{\"command\":\"login\",\"protocolVersion\":5}");
    const std::string key_response= receive_text();
    require_status_ok(key_response);
    const std::string public_key= json_string_value(key_response, "publicKeyPem");
    const std::string encrypted_password= encrypt_password(public_key, options_.password);
    std::ostringstream login;
    login << "{\"username\":\"" << json_escape(options_.user)
          << "\",\"password\":\"" << encrypted_password
          << "\",\"useCompression\":false,\"clientName\":\"mariadb_exasol_gw_sessiongw\"}";
    send_text(login.str());
    require_status_ok(receive_text());
  }

  void send_text(const std::string &text)
  {
    send_websocket_frame(0x81U, reinterpret_cast<const std::uint8_t *>(text.data()), text.size());
  }

  std::string receive_text()
  {
    std::vector<std::uint8_t> bytes= receive_websocket_payload();
    return std::string(reinterpret_cast<const char *>(vec_data(bytes)), bytes.size());
  }

  void send_frame(SessionGwMessageType type, const std::vector<std::uint8_t> &payload)
  {
    std::vector<std::uint8_t> frame= encode_sessiongw_frame(type, request_id_++, payload);
    send_websocket_frame(0x82U, vec_data(frame), frame.size());
  }

  SessionGwFrame receive_frame()
  {
    return decode_sessiongw_frame(receive_websocket_payload());
  }

  void append_row_handles(std::vector<std::uint8_t> &payload,
                          const std::vector<SessionGwRowHandle> &row_handles)
  {
    append_u32(payload, static_cast<std::uint32_t>(row_handles.size()));
    for (const SessionGwRowHandle &row_handle: row_handles)
    {
      append_u64(payload, row_handle.row_number);
    }
  }

  SessionGwFrame request(SessionGwMessageType type,
                         const std::vector<std::uint8_t> &payload,
                         SessionGwMessageType expected)
  {
    send_frame(type, payload);
    SessionGwFrame frame= receive_frame();
    throw_if_error_frame(frame);
    if (frame.type != expected)
    {
      std::ostringstream out;
      out << "unexpected SessionGW frame type " << static_cast<std::uint16_t>(frame.type)
          << ", expected " << static_cast<std::uint16_t>(expected);
      throw SessionGwError(out.str());
    }
    return frame;
  }

  SessionGwOpenCursorResult parse_open_cursor(const SessionGwFrame &frame)
  {
    std::size_t offset= 0;
    SessionGwOpenCursorResult result;
    result.cursor_id= read_u64(frame.payload, offset);
    result.arrow_schema= read_bytes32(frame.payload, offset);
    if (result.cursor_id == 0)
      throw SessionGwError("invalid SessionGW cursor id");
    return result;
  }

  SessionGwOpenOperationResult parse_open_operation(const SessionGwFrame &frame)
  {
    std::size_t offset= 0;
    SessionGwOpenOperationResult result;
    result.operation_id= read_u64(frame.payload, offset);
    result.accepted_schema= read_bytes32(frame.payload, offset);
    if (result.operation_id == 0)
      throw SessionGwError("invalid SessionGW operation id");
    return result;
  }

  std::uint64_t parse_affected_rows(const SessionGwFrame &frame)
  {
    std::size_t offset= 0;
    return read_u64(frame.payload, offset);
  }

  void send_websocket_frame(std::uint8_t first_byte, const std::uint8_t *payload, std::size_t size)
  {
    std::vector<std::uint8_t> out;
    out.push_back(first_byte);
    if (size <= 125U)
      out.push_back(static_cast<std::uint8_t>(0x80U | size));
    else if (size <= 0xffffU)
    {
      out.push_back(0x80U | 126U);
      append_big_endian(out, size, 2);
    }
    else
    {
      out.push_back(0x80U | 127U);
      append_big_endian(out, size, 8);
    }
    std::array<std::uint8_t, 4> mask{};
    append_mask(out, mask);
    std::size_t index= 0;
    append_masked_payload(out, mask, index, payload, size);
    transport_write(out);
  }

  std::vector<std::uint8_t> receive_websocket_payload()
  {
    std::array<std::uint8_t, 2> header{};
    transport_read(header.data(), header.size());
    const std::uint8_t opcode= header[0] & 0x0fU;
    std::uint64_t payload_length= header[1] & 0x7fU;
    if (payload_length == 126U)
    {
      std::array<std::uint8_t, 2> extended{};
      transport_read(extended.data(), extended.size());
      payload_length= read_big_endian(extended.data(), extended.size());
    }
    else if (payload_length == 127U)
    {
      std::array<std::uint8_t, 8> extended{};
      transport_read(extended.data(), extended.size());
      payload_length= read_big_endian(extended.data(), extended.size());
    }
    if (payload_length > 256U * 1024U * 1024U)
      throw SessionGwError("WebSocket payload too large");
    std::vector<std::uint8_t> payload(static_cast<std::size_t>(payload_length));
    if (!payload.empty())
      transport_read(payload.data(), payload.size());
    if (opcode == 0x8U)
      throw SessionGwError("WebSocket close received");
    if (opcode != 0x1U && opcode != 0x2U)
      return receive_websocket_payload();
    return payload;
  }

  SessionGwOptions options_;
  int fd_= -1;
  SSL_CTX *ssl_ctx_= nullptr;
  SSL *ssl_= nullptr;
  std::uint64_t request_id_= 1;
};

SessionGwConnection::SessionGwConnection(): impl_(new Impl()) {}
SessionGwConnection::~SessionGwConnection()= default;

void execute_sql(const SessionGwOptions &options, const std::string &sql)
{
  SessionGwConnection connection;
  connection.execute_sql_command(options, sql);
}

void SessionGwConnection::connect_and_enter(const SessionGwOptions &options)
{
  impl_->connect_and_enter(options);
}

void SessionGwConnection::execute_sql_command(const SessionGwOptions &options, const std::string &sql)
{
  impl_->execute_sql(options, sql);
}

void SessionGwConnection::close()
{
  impl_->close();
}

SessionGwDescribeTableResult SessionGwConnection::describe_table(const std::string &schema,
                                                                  const std::string &table)
{
  return impl_->describe_table(schema, table);
}

std::string SessionGwConnection::get_table_version(const std::string &schema,
                                                   const std::string &table)
{
  return impl_->get_table_version(schema, table);
}

SessionGwOpenCursorResult SessionGwConnection::open_pushed_query(const std::string &sql)
{
  return impl_->open_pushed_query(sql);
}

SessionGwOpenCursorResult SessionGwConnection::open_table_scan(const std::string &schema,
                                                               const std::string &table,
                                                               const std::vector<std::string> &columns,
                                                               bool include_row_handles,
                                                               const std::vector<SessionGwRowHandle> &row_handles)
{
  return impl_->open_table_scan(schema, table, columns, include_row_handles, row_handles);
}

SessionGwFetchResult SessionGwConnection::fetch(std::uint64_t cursor_id,
                                                std::uint32_t max_rows,
                                                std::uint32_t max_bytes)
{
  return impl_->fetch(cursor_id, max_rows, max_bytes);
}

void SessionGwConnection::close_cursor(std::uint64_t cursor_id)
{
  impl_->close_cursor(cursor_id);
}

SessionGwOpenOperationResult SessionGwConnection::open_table_insert(
    const std::string &schema,
    const std::string &table,
    const std::vector<std::string> &columns,
    std::uint32_t max_rows_per_batch,
    const std::vector<std::uint8_t> &arrow_schema)
{
  return impl_->open_table_insert(schema, table, columns, max_rows_per_batch, arrow_schema);
}

std::uint64_t SessionGwConnection::insert_rows(std::uint64_t operation_id,
                                               std::uint32_t row_count,
                                               const std::vector<std::uint8_t> &native_batch)
{
  return impl_->insert_rows(operation_id, row_count, native_batch);
}

SessionGwOpenOperationResult SessionGwConnection::open_table_update(
    const std::string &schema,
    const std::string &table,
    const std::vector<std::string> &columns,
    std::uint32_t max_rows_per_batch,
    const std::vector<std::uint8_t> &arrow_schema)
{
  return impl_->open_table_update(schema, table, columns, max_rows_per_batch, arrow_schema);
}

std::uint64_t SessionGwConnection::update_rows(std::uint64_t operation_id,
                                               const std::vector<SessionGwRowHandle> &row_handles,
                                               const std::vector<std::uint8_t> &native_batch)
{
  return impl_->update_rows(operation_id, row_handles, native_batch);
}

SessionGwOpenOperationResult SessionGwConnection::open_table_delete(
    const std::string &schema,
    const std::string &table,
    std::uint32_t max_rows_per_batch)
{
  return impl_->open_table_delete(schema, table, max_rows_per_batch);
}

std::uint64_t SessionGwConnection::delete_rows(std::uint64_t operation_id,
                                               const std::vector<SessionGwRowHandle> &row_handles)
{
  return impl_->delete_rows(operation_id, row_handles);
}

void SessionGwConnection::close_operation(std::uint64_t operation_id)
{
  impl_->close_operation(operation_id);
}

void SessionGwConnection::set_autocommit(bool enabled)
{
  impl_->set_autocommit(enabled);
}

void SessionGwConnection::commit()
{
  impl_->commit();
}

void SessionGwConnection::rollback()
{
  impl_->rollback();
}

} // namespace exasol_gw
