#ifndef EXASOL_GW_SESSION_INCLUDED
#define EXASOL_GW_SESSION_INCLUDED

#include "exasol_sessiongw_client.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class THD;

namespace exasol_gw
{

struct SessionGwAdapterStatistics
{
  std::uint64_t connection_attempts= 0;
  std::uint64_t connection_retries= 0;
  std::uint64_t metadata_cache_hits= 0;
  std::uint64_t metadata_cache_misses= 0;
  std::uint64_t metadata_refreshes= 0;
  std::uint64_t cursors_opened= 0;
  std::uint64_t cursors_closed= 0;
  std::uint64_t operations_opened= 0;
  std::uint64_t fetch_batches= 0;
  std::uint64_t fetched_rows= 0;
  std::uint64_t positioned_cache_hits= 0;
  std::uint64_t positioned_fetches= 0;
  std::uint64_t positioned_rows= 0;
  std::uint64_t arrow_bytes= 0;
  std::uint64_t native_read_bytes= 0;
  std::uint64_t projected_columns= 0;
  std::uint64_t available_columns= 0;
  std::uint64_t arrow_decode_nanoseconds= 0;
  std::uint64_t row_materialize_nanoseconds= 0;
  std::uint64_t native_buffer_nanoseconds= 0;
  std::uint64_t native_encode_nanoseconds= 0;
};

class SessionGwThdContext
{
public:
  explicit SessionGwThdContext(THD *thd);
  ~SessionGwThdContext();

  void validate_authenticated_principal(const THD *thd) const;
  SessionGwConnection &connection();
  void participate_in_statement(bool explicit_transaction);
  void commit_transaction(bool all);
  void rollback_transaction(bool all);
  SessionGwDescribeTableResult describe_table(const std::string &schema,
                                               const std::string &table);
  void read_cursor_opened();
  void read_cursor_closed();
  void operation_opened();
  void operation_closed();
  void statement_table_opened();
  void statement_table_closed();
  void record_projection(std::size_t projected_columns, std::size_t available_columns);
  void record_fetch(std::size_t rows, std::size_t native_read_bytes,
                    std::uint64_t decode_nanoseconds, bool positioned= false);
  void record_positioned_cache_hit();
  void record_row_materialize(std::uint64_t nanoseconds);
  void record_native_buffer(std::uint64_t nanoseconds);
  void record_native_encode(std::uint64_t nanoseconds);
  bool instrumentation_enabled() const noexcept;
  void reset() noexcept;

private:
  struct CachedMetadata
  {
    SessionGwDescribeTableResult value;
    std::chrono::steady_clock::time_point statistics_refreshed;
  };

  void finish_idle_read_transaction();
  void synchronize_autocommit(bool enabled);
  void finish_transaction_boundary();

  THD *thd_;
  std::string authenticated_principal_;
  SessionGwOptions options_= options_from_environment();
  SessionGwConnection connection_;
  bool connected_= false;
  std::size_t open_cursors_= 0;
  std::size_t open_operations_= 0;
  std::size_t statement_tables_= 0;
  bool read_transaction_pending_= false;
  bool remote_autocommit_known_= false;
  bool remote_autocommit_= true;
  bool transaction_active_= false;
  std::vector<CachedMetadata> metadata_cache_;
  SessionGwAdapterStatistics statistics_;
};

SessionGwThdContext &session_for_thd(THD *thd);
void destroy_session_for_thd(THD *thd);

} // namespace exasol_gw

#endif
