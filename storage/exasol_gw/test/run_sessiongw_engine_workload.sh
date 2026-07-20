#!/usr/bin/env bash
set -euo pipefail

# Live SessionGW-backed MariaDB ENGINE=EXASOL workload.
# This intentionally mirrors the old rr.examariadb prototype's core coverage:
# plugin load, DDL lifecycle, scan/pushdown, insert/update/delete, type conversion,
# LOAD DATA, prepared statements, multi-session stress, a configurable performance
# baseline, and direct Exasol final-state verification.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MARIADB_SRC=${MARIADB_SRC:-$(cd "$SCRIPT_DIR/../../.." && pwd -P)}
MARIADB_BUILD=${MARIADB_BUILD:-$MARIADB_SRC/build-sessiongw}
DB_EXANANO=${DB_EXANANO:-$(cd "$MARIADB_SRC/../db.exanano" && pwd -P)}
NANO_RUN=${NANO_RUN:-$DB_EXANANO/.build/exasol-nano-db-2026.2.0-nano.3-x86_64.run}
BASE_DIR=${BASE_DIR:-${TMPDIR:-/tmp}/exasol-gw-mariadb-workload.$$}
EXASOL_PORT=${EXASOL_PORT:-8571}
NANO_DBRAM=${EXASOL_NANO_DBRAM:-2048}
SCHEMA=${SCHEMA:-SGW_MDB_COV}
READ_CLIENTS=${READ_CLIENTS:-16}
INSERT_CLIENTS=${INSERT_CLIENTS:-20}
INSERT_ROUNDS=${INSERT_ROUNDS:-1}
INSERT_ROWS_PER_CLIENT=${INSERT_ROWS_PER_CLIENT:-1}
PERF_ROWS=${PERF_ROWS:-100000}
PERF_INSERT_BATCH_ROWS=${PERF_INSERT_BATCH_ROWS:-10000}
PERF_UPDATE_ROWS=${PERF_UPDATE_ROWS:-$((PERF_ROWS / 10))}
PERF_DELETE_ROWS=${PERF_DELETE_ROWS:-$((PERF_ROWS / 20))}
FAULT_INJECTION_ONLY=${FAULT_INJECTION_ONLY:-0}
FAULT_TIMEOUT_SECONDS=${FAULT_TIMEOUT_SECONDS:-20}
POSITIONED_DML_TEST_ONLY=${POSITIONED_DML_TEST_ONLY:-0}
POSITIONED_DML_BATCH_ROWS=${POSITIONED_DML_BATCH_ROWS:-3}
# Zero lets the engine select a projection-aware read-only batch size while
# retaining 1K rows for row-location scans. A positive value is an override.
SESSIONGW_FETCH_ROWS=${EXASOL_SESSIONGW_FETCH_ROWS:-0}
SESSIONGATEWAY_SDK_RUNTIME_PATH=${SESSIONGATEWAY_SDK_RUNTIME_PATH:-}
if (( PERF_UPDATE_ROWS == 0 )); then PERF_UPDATE_ROWS=1; fi
if (( PERF_DELETE_ROWS == 0 )); then PERF_DELETE_ROWS=1; fi
if [[ "$POSITIONED_DML_TEST_ONLY" == "1" && ! "$POSITIONED_DML_BATCH_ROWS" =~ ^[3-9][0-9]*$ ]]; then
    echo "POSITIONED_DML_BATCH_ROWS must be at least 3" >&2
    exit 2
fi

INSERT_BATCH_ROWS=${EXASOL_SESSIONGW_INSERT_BATCH_ROWS:-10000}
UPDATE_BATCH_ROWS=${EXASOL_SESSIONGW_UPDATE_BATCH_ROWS:-10000}
DELETE_BATCH_ROWS=${EXASOL_SESSIONGW_DELETE_BATCH_ROWS:-10000}
if [[ "$POSITIONED_DML_TEST_ONLY" == "1" ]]; then
    # This focused mode makes two-row DML stay below a known threshold without
    # distorting the workload's performance baseline. The SQL filesort probe
    # exercises vector positioned-fetch misses only: MariaDB closes its sequential
    # phase before beginning rowid lookup, so no current Arrow batch remains.
    UPDATE_BATCH_ROWS=$POSITIONED_DML_BATCH_ROWS
    DELETE_BATCH_ROWS=$POSITIONED_DML_BATCH_ROWS
    SESSIONGW_FETCH_ROWS=${EXASOL_SESSIONGW_FETCH_ROWS:-1}
fi

require_file() {
    if [[ ! -e "$1" ]]; then
        echo "Missing $1" >&2
        exit 2
    fi
}

require_file "$MARIADB_BUILD/sql/mariadbd"
require_file "$MARIADB_BUILD/client/mariadb"
require_file "$MARIADB_BUILD/scripts/mariadb-install-db"
require_file "$MARIADB_BUILD/storage/exasol_gw/ha_exasol_gw.so"
require_file "$NANO_RUN"

NANO_BASE=$BASE_DIR/nano
NANO_APP=$NANO_BASE/app
NANO_DB=$NANO_BASE/db
MDB=$BASE_DIR/mariadb
SOCKET=$MDB/mariadb.sock
PIDFILE=$MDB/mariadb.pid
REPORT=$BASE_DIR/report.txt
mkdir -p "$NANO_APP" "$NANO_DB" "$MDB"
: >"$REPORT"

cleanup() {
    if [[ -f "$PIDFILE" ]]; then
        kill "$(cat "$PIDFILE")" >/dev/null 2>&1 || true
    fi
    if [[ -f "$NANO_BASE/pid" ]]; then
        nano_pid=$(cat "$NANO_BASE/pid")
        # AppRun starts several descendants. It has its own process group so
        # cleanup cannot kill the invoking shell by matching BASE_DIR text.
        kill -- "-$nano_pid" >/dev/null 2>&1 || kill "$nano_pid" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

log() { printf '%s\n' "$*" | tee -a "$REPORT"; }

sql_exasol() {
    (cd "$DB_EXANANO" && c4 sqlclient --usetls --skiptlsverify --user sys --password exasol \
        --connection 127.0.0.1:$EXASOL_PORT --query "$1")
}

mysql() {
    "$MARIADB_BUILD/client/mariadb" --no-defaults --local-infile=1 --socket="$SOCKET" "$@"
}

mysql_scalar() {
    mysql --batch --raw --skip-column-names -e "$1" | tail -n 1
}

expect_scalar() {
    local label=$1
    local sql=$2
    local expected=$3
    local actual
    actual=$(mysql_scalar "$sql")
    if [[ "$actual" != "$expected" ]]; then
        echo "Unexpected $label: expected '$expected' got '$actual'" >&2
        exit 1
    fi
    log "PASS $label = $actual"
}

expect_failure() {
    local label=$1
    local sql=$2
    if mysql -e "$sql" >"$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out" 2>&1; then
        echo "Expected failure for $label but command succeeded" >&2
        exit 1
    fi
    log "PASS expected failure: $label"
}

expect_failure_contains() {
    local label=$1
    local sql=$2
    local expected=$3
    local output="$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out"
    if mysql -e "$sql" >"$output" 2>&1; then
        echo "Expected failure for $label but command succeeded" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$output"; then
        echo "Failure for $label did not contain '$expected'" >&2
        cat "$output" >&2
        exit 1
    fi
    log "PASS contained exception: $label"
}

mysql_timed() {
    timeout --foreground --signal=KILL "${FAULT_TIMEOUT_SECONDS}s" \
        "$MARIADB_BUILD/client/mariadb" --no-defaults --local-infile=1 --socket="$SOCKET" "$@"
}

expect_timed_failure_contains() {
    local label=$1
    local sql=$2
    local expected=$3
    local output="$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out"
    local rc=0
    mysql_timed -e "$sql" >"$output" 2>&1 || rc=$?
    if (( rc == 0 )); then
        echo "Expected failure for $label but command succeeded" >&2
        exit 1
    fi
    if (( rc == 124 || rc == 137 )); then
        echo "Timed out after ${FAULT_TIMEOUT_SECONDS}s while testing $label" >&2
        cat "$output" >&2
        exit 1
    fi
    if ! grep -Fq "$expected" "$output"; then
        echo "Failure for $label did not contain '$expected'" >&2
        cat "$output" >&2
        exit 1
    fi
    log "PASS bounded contained exception: $label"
}

expect_timed_scalar() {
    local label=$1
    local sql=$2
    local expected=$3
    local actual
    actual=$(mysql_timed --batch --raw --skip-column-names -e "$sql" | tail -n 1)
    if [[ "$actual" != "$expected" ]]; then
        echo "Unexpected $label: expected '$expected' got '$actual'" >&2
        exit 1
    fi
    log "PASS bounded reuse: $label = $actual"
}

expect_one_balanced_cursor_metric() {
    local label=$1
    local before=$2
    local after metrics
    after=$(grep -c 'SessionGW performance:' "$MDB/mariadb.err" || true)
    if (( after != before + 1 )); then
        echo "Expected one SessionGW metric record for $label, got $((after - before))" >&2
        exit 1
    fi
    metrics=$(grep 'SessionGW performance:' "$MDB/mariadb.err" | tail -n 1)
    python3 - "$label" "$metrics" <<'PY'
import re
import sys
label, line = sys.argv[1:]
values = {name: int(value) for name, value in re.findall(r"([a-z_]+)=([0-9]+)", line)}
for name in ("cursors_opened", "cursors_closed"):
    if name not in values:
        raise SystemExit(f"{label}: missing {name} in SessionGW performance record")
if values["cursors_opened"] != 1 or values["cursors_closed"] != 1:
    raise SystemExit(f"{label}: expected one balanced cursor lifecycle, got {values}")
print(f"{label}: cursors_opened=1 cursors_closed=1")
PY
    log "PASS early-destruction cursor cleanup $label: $metrics"
}

expect_handler_unsupported() {
    expect_failure_contains "$1" "$2" 'Got error 138 "Unsupported extension used for table"'
}

expect_exasol_failure() {
    local label=$1
    local sql=$2
    if sql_exasol "$sql" >"$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out" 2>&1; then
        echo "Expected Exasol failure for $label but command succeeded" >&2
        exit 1
    fi
    log "PASS expected Exasol failure: $label"
}

run_positioned_probe() {
    local label=$1
    local sql=$2
    local expected_positioned_fetches=$3
    local sort_data_limit=16
    local before after metrics plan effective_sort_data_limit
    effective_sort_data_limit=$(mysql_scalar "SET SESSION max_length_for_sort_data=$sort_data_limit; SELECT @@SESSION.max_length_for_sort_data")
    if [[ "$effective_sort_data_limit" != "$sort_data_limit" ]]; then
        echo "Expected max_length_for_sort_data=$sort_data_limit for $label, got $effective_sort_data_limit" >&2
        exit 1
    fi
    plan=$(mysql --batch --raw -e "SET SESSION max_length_for_sort_data=$sort_data_limit; EXPLAIN $sql")
    if ! grep -Fq 'Using filesort' <<<"$plan"; then
        echo "Expected filesort plan for $label" >&2
        printf '%s\n' "$plan" >&2
        exit 1
    fi
    log "PASS positioned probe prerequisite $label: max_length_for_sort_data=$effective_sort_data_limit"
    log "PASS positioned probe plan $label: $(tr '\n' '|' <<<"$plan")"
    before=$(grep -c 'SessionGW performance:' "$MDB/mariadb.err" || true)
    mysql -e "SET SESSION max_length_for_sort_data=$sort_data_limit; $sql" >"$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out"
    after=$(grep -c 'SessionGW performance:' "$MDB/mariadb.err" || true)
    if (( after != before + 1 )); then
        echo "Expected one SessionGW metric record for $label, got $((after - before))" >&2
        exit 1
    fi
    metrics=$(grep 'SessionGW performance:' "$MDB/mariadb.err" | tail -n 1)
    python3 - "$label" "$metrics" "$expected_positioned_fetches" <<'PY'
import re
import sys
label, line, expected_fetches = sys.argv[1:]
values = {name: int(value) for name, value in re.findall(r"([a-z_]+)=([0-9]+)", line)}
for name in ("cursors_opened", "cursors_closed", "positioned_cache_hits", "positioned_fetches", "positioned_rows"):
    if name not in values:
        raise SystemExit(f"{label}: missing {name} in SessionGW performance record")
# MariaDB filesort has two handler phases: the sequential sort-key scan and
# the subsequent rowid lookup. The invariant is one cursor per phase, rather
# than a cursor lifecycle for every rnd_pos call.
if values["cursors_opened"] != 2 or values["cursors_closed"] != 2:
    raise SystemExit(f"{label}: expected two filesort phase cursor lifecycles, got {values}")
if values["positioned_fetches"] != int(expected_fetches):
    raise SystemExit(f"{label}: expected {expected_fetches} positioned fetches, got {values}")
if values["positioned_rows"] != int(expected_fetches):
    raise SystemExit(f"{label}: expected {expected_fetches} positioned rows, got {values}")
print(f"{label}: " + " ".join(f"{name}={values[name]}" for name in
      ("cursors_opened", "cursors_closed", "positioned_cache_hits", "positioned_fetches", "positioned_rows")))
PY
    log "PASS positioned probe $label metrics: $metrics"
}

run_positioned_cache_hit_probe() {
    local label=$1
    local sql=$2
    local expected_cache_hits=$3
    local before after metrics
    before=$(grep -c 'SessionGW performance:' "$MDB/mariadb.err" || true)
    mysql -e "$sql" >"$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out"
    after=$(grep -c 'SessionGW performance:' "$MDB/mariadb.err" || true)
    if (( after != before + 1 )); then
        echo "Expected one SessionGW metric record for $label, got $((after - before))" >&2
        exit 1
    fi
    metrics=$(grep 'SessionGW performance:' "$MDB/mariadb.err" | tail -n 1)
    python3 - "$label" "$metrics" "$expected_cache_hits" <<'PY'
import re
import sys
label, line, expected_hits = sys.argv[1:]
values = {name: int(value) for name, value in re.findall(r"([a-z_]+)=([0-9]+)", line)}
for name in ("cursors_opened", "cursors_closed", "positioned_cache_hits", "positioned_fetches", "positioned_rows"):
    if name not in values:
        raise SystemExit(f"{label}: missing {name} in SessionGW performance record")
if values["cursors_opened"] != 1 or values["cursors_closed"] != 1:
    raise SystemExit(f"{label}: expected one live-scan cursor lifecycle, got {values}")
if values["positioned_cache_hits"] != int(expected_hits):
    raise SystemExit(f"{label}: expected {expected_hits} positioned cache hits, got {values}")
if values["positioned_fetches"] != 0 or values["positioned_rows"] != 0:
    raise SystemExit(f"{label}: cache hits unexpectedly issued remote positioned fetches: {values}")
print(f"{label}: " + " ".join(f"{name}={values[name]}" for name in
      ("cursors_opened", "cursors_closed", "positioned_cache_hits", "positioned_fetches", "positioned_rows")))
PY
    log "PASS positioned cache-hit probe $label metrics: $metrics"
}

log "SessionGW MariaDB engine workload"
log "base=$BASE_DIR"
log "mariadb_build=$MARIADB_BUILD"
log "nano_port=$EXASOL_PORT"
log "nano_dbram=$NANO_DBRAM"
log "perf_rows=$PERF_ROWS"
log "perf_insert_batch_rows=$PERF_INSERT_BATCH_ROWS"
log "perf_update_rows=$PERF_UPDATE_ROWS"
log "perf_delete_rows=$PERF_DELETE_ROWS"
log "insert_rounds=$INSERT_ROUNDS"
log "insert_rows_per_client=$INSERT_ROWS_PER_CLIENT"
log "sessiongw_insert_batch_rows=$INSERT_BATCH_ROWS"
log "sessiongw_update_batch_rows=$UPDATE_BATCH_ROWS"
log "sessiongw_delete_batch_rows=$DELETE_BATCH_ROWS"
log "sessiongw_fetch_rows=$SESSIONGW_FETCH_ROWS"
if [[ "$POSITIONED_DML_TEST_ONLY" == "1" ]]; then
    log "positioned_dml_test_only=1 positioned_dml_batch_rows=$POSITIONED_DML_BATCH_ROWS"
fi

"$NANO_RUN" --target "$NANO_APP" --noexec >/dev/null
APPDIR=$(find "$NANO_APP" -maxdepth 1 -type d -name '*.AppDir' | head -n 1)
(cd "$NANO_BASE" && setsid "$APPDIR/AppRun" --db-files-dir "$NANO_DB" --port "$EXASOL_PORT" \
    -dbram="$NANO_DBRAM" >"$NANO_BASE/nano.log" 2>&1 & echo $! > "$NANO_BASE/pid")
for _ in $(seq 1 90); do
    if sql_exasol 'select 1' >/dev/null 2>&1; then break; fi
    sleep 2
done
sql_exasol 'select 1' >/dev/null
log "PASS nano ready"

"$MARIADB_BUILD/scripts/mariadb-install-db" --force --no-defaults \
    --srcdir="$MARIADB_SRC" --builddir="$MARIADB_BUILD" --datadir="$MDB/data" \
    --auth-root-authentication-method=normal >"$MDB/install.log" 2>&1

EXASOL_SESSIONGW_HOST=localhost \
EXASOL_SESSIONGW_PORT=$EXASOL_PORT \
EXASOL_SESSIONGW_USER=sys \
EXASOL_SESSIONGW_PASSWORD=exasol \
EXASOL_SESSIONGW_TLS=skip_verify \
EXASOL_SESSIONGW_INSTRUMENTATION=${EXASOL_SESSIONGW_INSTRUMENTATION:-1} \
EXASOL_SESSIONGW_FETCH_ROWS=$SESSIONGW_FETCH_ROWS \
EXASOL_SESSIONGW_INSERT_BATCH_ROWS=$INSERT_BATCH_ROWS \
EXASOL_SESSIONGW_UPDATE_BATCH_ROWS=$UPDATE_BATCH_ROWS \
EXASOL_SESSIONGW_DELETE_BATCH_ROWS=$DELETE_BATCH_ROWS \
LD_LIBRARY_PATH="$SESSIONGATEWAY_SDK_RUNTIME_PATH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}" \
"$MARIADB_BUILD/sql/mariadbd" --no-defaults \
    --datadir="$MDB/data" --socket="$SOCKET" --pid-file="$PIDFILE" \
    --port=0 --skip-networking \
    --plugin-dir="$MARIADB_BUILD/storage/exasol_gw" \
    --plugin-load-add=ha_exasol_gw.so \
    --log-error="$MDB/mariadb.err" --skip-grant-tables --local-infile=1 --user="$(id -un)" \
    >"$MDB/stdout.log" 2>&1 &

for _ in $(seq 1 60); do
    if mysql -e 'select 1' >/dev/null 2>&1; then break; fi
    sleep 1
done
mysql -e 'select 1' >/dev/null
log "PASS mariadb ready"

ENGINES=$(mysql --batch --raw -e 'SHOW ENGINES')
echo "$ENGINES" | grep -Eq '^EXASOL[[:space:]]+YES'
EXASOL_TRANSACTIONS=$(mysql --batch --raw --skip-column-names -e \
    "SELECT TRANSACTIONS FROM INFORMATION_SCHEMA.ENGINES WHERE ENGINE='EXASOL'")
if [[ "$EXASOL_TRANSACTIONS" != "YES" ]]; then
    echo "EXASOL must advertise full commit/rollback support, got: $EXASOL_TRANSACTIONS" >&2
    exit 1
fi
log "PASS show engines and transaction capability = $EXASOL_TRANSACTIONS"

mysql --table <<SQL | tee -a "$REPORT"
DROP DATABASE IF EXISTS $SCHEMA;
CREATE DATABASE $SCHEMA;
USE $SCHEMA;
CREATE TABLE T(ID BIGINT, NAME VARCHAR(40)) ENGINE=EXASOL;
INSERT INTO T VALUES (1, 'Alice'), (2, 'Bob'), (3, 'Carol'), (4, 'Dave');
SELECT * FROM T ORDER BY ID;
UPDATE T SET NAME='Bobby' WHERE ID=2;
DELETE FROM T WHERE ID=3;
SELECT * FROM T ORDER BY ID;
SELECT COUNT(*) AS C, SUM(ID) AS S, MAX(NAME) AS M FROM T;
SQL
PLANNER_ROWS=$(mysql --batch --raw --skip-column-names -e "
  SELECT TABLE_ROWS FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='$SCHEMA' AND TABLE_NAME='T';
  USE $SCHEMA; INSERT INTO T VALUES (5, 'Stats');
  SELECT TABLE_ROWS FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='$SCHEMA' AND TABLE_NAME='T';
  DELETE FROM T WHERE ID=5;
  SELECT TABLE_ROWS FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='$SCHEMA' AND TABLE_NAME='T';" | paste -sd'|' -)
if [[ "$PLANNER_ROWS" != "3|4|3" ]]; then
    echo "Unexpected cached planner row statistics: $PLANNER_ROWS" >&2
    exit 1
fi
log "PASS remote planner row statistics and local DML invalidation = $PLANNER_ROWS"

mysql -e "USE $SCHEMA; CREATE TABLE STATS_REFRESH_T(ID INT) ENGINE=EXASOL; INSERT INTO STATS_REFRESH_T VALUES (1)"
STATS_REFRESH_ROWS=$(mysql --batch --raw --skip-column-names -e "
  SELECT TABLE_ROWS FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='$SCHEMA' AND TABLE_NAME='STATS_REFRESH_T';
  DO SLEEP(6);
  SELECT TABLE_ROWS FROM INFORMATION_SCHEMA.TABLES WHERE TABLE_SCHEMA='$SCHEMA' AND TABLE_NAME='STATS_REFRESH_T';" | paste -sd'|' -)
if [[ "$STATS_REFRESH_ROWS" != "1|1" ]]; then
    echo "Unexpected refreshed planner statistics: $STATS_REFRESH_ROWS" >&2
    exit 1
fi
STATS_REFRESH_METRICS=$(grep 'SessionGW performance:' "$MDB/mariadb.err" | tail -1)
if [[ ! "$STATS_REFRESH_METRICS" =~ metadata_hits=[1-9][0-9]*\ metadata_misses=1\ metadata_refreshes=1 ]]; then
    echo "Planner metadata freshness was not instrumented as one cache refresh: $STATS_REFRESH_METRICS" >&2
    exit 1
fi
log "PASS planner statistics five-second freshness and cache instrumentation = $STATS_REFRESH_ROWS"
log "PASS ddl insert update delete scan"

mysql -e "USE $SCHEMA; CREATE TABLE META_GUARD(ID INT, NAME VARCHAR(20)) ENGINE=EXASOL; INSERT INTO META_GUARD VALUES (1, 'bound'); SELECT * FROM META_GUARD" >/dev/null
sql_exasol "DROP TABLE $SCHEMA.META_GUARD" >/dev/null
sql_exasol "CREATE TABLE $SCHEMA.META_GUARD(ID DECIMAL(18,0), NAME VARCHAR(21) UTF8)" >/dev/null
expect_failure_contains "incompatible remote metadata" \
    "USE $SCHEMA; SELECT * FROM META_GUARD" \
    "MariaDB column metadata does not match remote EXASOL column 'NAME'"
sql_exasol "DROP TABLE $SCHEMA.META_GUARD" >/dev/null
sql_exasol "CREATE TABLE $SCHEMA.META_GUARD(ID DECIMAL(18,0), NAME VARCHAR(20) UTF8)" >/dev/null
expect_failure_contains "replaced remote table generation" \
    "USE $SCHEMA; SELECT * FROM META_GUARD" \
    "Remote EXASOL table was changed or replaced"
mysql -e "USE $SCHEMA; DROP TABLE META_GUARD" >/dev/null
log "PASS remote metadata compatibility and generation invalidation"

if mysql -e "SET SESSION debug_dbug=''" >/dev/null 2>&1; then
    expect_failure_contains "pushdown cursor constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_cursor_constructor_oom'; USE $SCHEMA; SELECT * FROM T" \
        "out of memory"
    expect_timed_failure_contains "table scan cursor constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_table_scan_cursor_constructor_oom'; USE $SCHEMA; UPDATE T SET NAME=NAME WHERE ID=1" \
        "out of memory"
    expect_timed_scalar "table scan cursor failure preserves reuse" \
        "USE $SCHEMA; SELECT COUNT(*) FROM T" "3"
    expect_failure_contains "insert context constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_insert_context_constructor_oom'; USE $SCHEMA; INSERT INTO T VALUES (10, 'fault')" \
        "out of memory"
    expect_failure_contains "update context constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_update_context_constructor_oom'; USE $SCHEMA; UPDATE T SET NAME='fault' WHERE ID=1" \
        "out of memory"
    expect_failure_contains "delete context constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_delete_context_constructor_oom'; USE $SCHEMA; DELETE FROM T WHERE ID=1" \
        "out of memory"

    mysql -e "USE $SCHEMA; CREATE TABLE DML_ABORT_GUARD(ID INT, NAME VARCHAR(20)) ENGINE=EXASOL; INSERT INTO DML_ABORT_GUARD VALUES (1, 'one'), (2, 'two')"
    expect_failure_contains "insert failure after completed batch preserves original error" \
        "SET SESSION debug_dbug='+d,exasol_gw_dml_batch_one,exasol_gw_insert_after_batch_error'; USE $SCHEMA; INSERT INTO DML_ABORT_GUARD VALUES (3, 'three'), (4, 'four')" \
        "injected insert failure after a completed SessionGW batch"
    expect_scalar "failed insert commits no completed-batch prefix" \
        "USE $SCHEMA; SELECT COUNT(*) FROM DML_ABORT_GUARD" "2"
    expect_failure_contains "update failure after completed batch preserves original error" \
        "SET SESSION debug_dbug='+d,exasol_gw_dml_batch_one,exasol_gw_update_after_batch_error'; USE $SCHEMA; UPDATE DML_ABORT_GUARD SET NAME='changed' ORDER BY ID" \
        "injected update failure after a completed SessionGW batch"
    expect_scalar "failed update commits no completed-batch prefix" \
        "USE $SCHEMA; SELECT COUNT(*) FROM DML_ABORT_GUARD WHERE NAME='changed'" "0"
    expect_failure_contains "delete failure after completed batch preserves original error" \
        "SET SESSION debug_dbug='+d,exasol_gw_dml_batch_one,exasol_gw_delete_after_batch_error'; USE $SCHEMA; DELETE FROM DML_ABORT_GUARD ORDER BY ID" \
        "injected delete failure after a completed SessionGW batch"
    expect_scalar "failed delete commits no completed-batch prefix" \
        "USE $SCHEMA; SELECT COUNT(*) FROM DML_ABORT_GUARD" "2"
    log "PASS failed insert/update/delete CLEAN-abort completed prefixes and preserve original errors"

    mysql -e "USE $SCHEMA; CREATE TABLE OUTCOME_UNKNOWN_GUARD(ID INT, NAME VARCHAR(20)) ENGINE=EXASOL; INSERT INTO OUTCOME_UNKNOWN_GUARD VALUES (1, 'one'), (2, 'two')"
    expect_failure_contains "lost insert completion acknowledgement reports unknown outcome" \
        "SET SESSION debug_dbug='+d,exasol_gw_completion_ack_lost'; USE $SCHEMA; INSERT INTO OUTCOME_UNKNOWN_GUARD VALUES (3, 'three')" \
        "outcome unknown"
    expect_scalar "unknown insert outcome was applied exactly once without replay" \
        "USE $SCHEMA; SELECT COUNT(*) FROM OUTCOME_UNKNOWN_GUARD WHERE ID=3 AND NAME='three'" "1"
    expect_failure_contains "lost update completion acknowledgement reports unknown outcome" \
        "SET SESSION debug_dbug='+d,exasol_gw_completion_ack_lost'; USE $SCHEMA; UPDATE OUTCOME_UNKNOWN_GUARD SET NAME='updated' WHERE ID=1" \
        "outcome unknown"
    expect_scalar "unknown update outcome was applied exactly once without replay" \
        "USE $SCHEMA; SELECT COUNT(*) FROM OUTCOME_UNKNOWN_GUARD WHERE ID=1 AND NAME='updated'" "1"
    expect_failure_contains "lost delete completion acknowledgement reports unknown outcome" \
        "SET SESSION debug_dbug='+d,exasol_gw_completion_ack_lost'; USE $SCHEMA; DELETE FROM OUTCOME_UNKNOWN_GUARD WHERE ID=2" \
        "outcome unknown"
    expect_scalar "unknown delete outcome was applied exactly once without replay" \
        "USE $SCHEMA; SELECT COUNT(*) FROM OUTCOME_UNKNOWN_GUARD WHERE ID=2" "0"
    log "PASS lost completion acknowledgements report UNKNOWN without replaying durable DML"

    mysql -e "USE $SCHEMA; CREATE TABLE SHARE_LOCK_GUARD(ID INT) ENGINE=EXASOL; FLUSH TABLE SHARE_LOCK_GUARD"
    expect_timed_failure_contains "table share construction allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_share_constructor_oom'; USE $SCHEMA; SELECT COUNT(*) FROM SHARE_LOCK_GUARD" \
        "out of memory"
    expect_timed_scalar "table share lock released after allocation fault" \
        "USE $SCHEMA; SELECT COUNT(*) FROM SHARE_LOCK_GUARD" "0"

    expect_timed_failure_contains "metadata version critical-section allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_metadata_version_assignment_oom'; USE $SCHEMA; SELECT COUNT(*) FROM T" \
        "out of memory"
    expect_timed_scalar "metadata mutex released after allocation fault" \
        "USE $SCHEMA; SELECT COUNT(*) FROM T" "3"

    mysql -e "USE $SCHEMA; CREATE TABLE DERIVED_DEST(ID BIGINT) ENGINE=InnoDB; INSERT INTO DERIVED_DEST VALUES (1)"
    DERIVED_METRICS_BEFORE=$(grep -c 'SessionGW performance:' "$MDB/mariadb.err" || true)
    expect_timed_failure_contains "derived handler early destruction allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_derived_after_cursor_open_oom'; USE $SCHEMA; SELECT * FROM DERIVED_DEST AS L, (SELECT ID FROM T LIMIT 2) AS D" \
        "out of memory"
    expect_one_balanced_cursor_metric "derived handler early destruction" "$DERIVED_METRICS_BEFORE"
    expect_timed_scalar "derived handler failure preserves reuse" \
        "USE $SCHEMA; SELECT COUNT(*) FROM T" "3"

    expect_scalar "constructor faults preserve server and rows" \
        "USE $SCHEMA; SELECT CONCAT(COUNT(*), '|', MIN(NAME)) FROM T" "3|Alice"
else
    log "SKIP constructor fault injection (MariaDB build has DBUG disabled)"
fi

if [[ "$FAULT_INJECTION_ONLY" == "1" ]]; then
    log "SessionGW MariaDB constructor fault-injection workload passed"
    exit 0
fi

# Multi-table DML makes MariaDB re-read EXASOL rows by position.  In focused
# mode the two-row cases are deliberately below the three-row operation limit.
mysql --table <<SQL | tee -a "$REPORT"
USE $SCHEMA;
CREATE TABLE POS_T(ID INT, NAME VARCHAR(40)) ENGINE=EXASOL;
INSERT INTO POS_T VALUES
  (1, 'One'), (2, 'Two'), (3, 'Three'), (4, 'Four'), (5, 'Five'),
  (6, 'Six'), (7, 'Seven'), (8, 'Eight'), (9, 'Nine'), (10, 'Ten');
CREATE TABLE POS_KEYS(ID INT) ENGINE=InnoDB;
INSERT INTO POS_KEYS VALUES (2), (3), (4), (5), (6), (7);
UPDATE POS_T AS p JOIN POS_KEYS AS k ON p.ID=k.ID SET p.NAME='one-update' WHERE k.ID=2;
UPDATE POS_T AS p JOIN POS_KEYS AS k ON p.ID=k.ID SET p.NAME='multi-update' WHERE k.ID IN (4, 5);
DELETE p FROM POS_T AS p JOIN POS_KEYS AS k ON p.ID=k.ID WHERE k.ID=3;
DELETE p FROM POS_T AS p JOIN POS_KEYS AS k ON p.ID=k.ID WHERE k.ID IN (6, 7);
SQL
POS_FINAL=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT COUNT(*) FROM POS_T; SELECT SUM(ID) FROM POS_T; SELECT COUNT(*) FROM POS_T WHERE NAME='one-update'; SELECT COUNT(*) FROM POS_T WHERE NAME='multi-update'; SELECT COUNT(*) FROM POS_T WHERE ID IN (3, 6, 7);" | paste -sd'|' -)
if [[ "$POS_FINAL" != "7|39|1|2|0" ]]; then
    echo "Unexpected positioned DML final state: $POS_FINAL" >&2
    exit 1
fi
log "PASS positioned one-row and below-threshold multi-row DML final = $POS_FINAL"

if [[ "$POSITIONED_DML_TEST_ONLY" == "1" ]]; then
    # The DBUG hook invokes ha_exasol_gw::rnd_pos() immediately after its real
    # rnd_next() materializes each live row. It therefore exercises the actual
    # current Arrow-batch cache branch without a remote positioned fetch.
    if mysql -e "SET SESSION debug_dbug=''" >/dev/null 2>&1; then
        run_positioned_cache_hit_probe "positioned read live batch cache hit" \
            "SET SESSION debug_dbug='+d,exasol_gw_positioned_cache_hit'; SELECT @positioned_cache_hit:=ID FROM $SCHEMA.POS_T" \
            7
    else
        log "SKIP positioned live-batch cache-hit probe (MariaDB build has DBUG disabled)"
    fi

    # The low max_length_for_sort_data forces filesort to retain row positions
    # instead of the selected VARCHAR data. MariaDB closes the sequential scan
    # before rowid lookup, so this probe exercises vector positioned-fetch misses
    # only, while proving there is no cursor lifecycle for every rnd_pos() call.
    run_positioned_probe "positioned read rowid lookup" \
        "SELECT ID, NAME FROM $SCHEMA.POS_T ORDER BY ID DESC FETCH FIRST 7 ROWS WITH TIES" \
        7
fi

# One MariaDB THD maps to one SessionGateway transaction stream. Positioned
# DML after ROLLBACK and COMMIT must obtain fresh transaction-scoped handles,
# while explicit rollback must undo the completed remote operation.
mysql --table <<SQL | tee -a "$REPORT"
USE $SCHEMA;
CREATE TABLE TXN_POS_T(ID INT, NAME VARCHAR(40)) ENGINE=EXASOL;
INSERT INTO TXN_POS_T VALUES (1, 'One'), (2, 'Two'), (3, 'Three'), (4, 'Four'), (5, 'Five'), (6, 'Six');
CREATE TABLE TXN_POS_KEYS(ID INT) ENGINE=InnoDB;
INSERT INTO TXN_POS_KEYS VALUES (1), (2), (3), (4);
START TRANSACTION;
UPDATE TXN_POS_T AS p JOIN TXN_POS_KEYS AS k ON p.ID=k.ID SET p.NAME='before-rollback' WHERE k.ID=1;
ROLLBACK;
UPDATE TXN_POS_T AS p JOIN TXN_POS_KEYS AS k ON p.ID=k.ID SET p.NAME='after-rollback' WHERE k.ID=2;
COMMIT;
START TRANSACTION;
DELETE p FROM TXN_POS_T AS p JOIN TXN_POS_KEYS AS k ON p.ID=k.ID WHERE k.ID=3;
COMMIT;
DELETE p FROM TXN_POS_T AS p JOIN TXN_POS_KEYS AS k ON p.ID=k.ID WHERE k.ID=4;
SQL
TXN_POS_FINAL=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT COUNT(*) FROM TXN_POS_T; SELECT SUM(ID) FROM TXN_POS_T; SELECT COUNT(*) FROM TXN_POS_T WHERE ID=1 AND NAME='before-rollback'; SELECT COUNT(*) FROM TXN_POS_T WHERE ID=2 AND NAME='after-rollback'; SELECT COUNT(*) FROM TXN_POS_T WHERE ID IN (3, 4);" | paste -sd'|' -)
if [[ "$TXN_POS_FINAL" != "4|14|0|1|0" ]]; then
    echo "Unexpected positioned transaction-boundary state: $TXN_POS_FINAL" >&2
    exit 1
fi
TXN_POS_REMOTE=$(sql_exasol "select count(*), sum(id) from $SCHEMA.TXN_POS_T")
echo "$TXN_POS_REMOTE" | tee -a "$REPORT" | grep -Eq '"data":\[\[4\],\["?14"?\]\]'
log "PASS positioned transaction boundary cleanup = $TXN_POS_FINAL"

# SET autocommit and START TRANSACTION both synchronize remote autocommit. A
# read can start the implicit transaction; subsequent DML is visible in that
# transaction and is removed by rollback. The next transaction can commit on
# the same THD/session.
AUTOCOMMIT_SYNC_FINAL=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA;
  SET autocommit=0;
  SELECT COUNT(*) FROM TXN_POS_T;
  INSERT INTO TXN_POS_T VALUES (7, 'rollback-after-read');
  SELECT COUNT(*) FROM TXN_POS_T WHERE ID=7;
  ROLLBACK;
  INSERT INTO TXN_POS_T VALUES (8, 'commit-after-rollback');
  COMMIT;
  SET autocommit=1;
  SELECT CONCAT((SELECT COUNT(*) FROM TXN_POS_T WHERE ID=7), '|',
                (SELECT COUNT(*) FROM TXN_POS_T WHERE ID=8));" | paste -sd'|' -)
if [[ "$AUTOCOMMIT_SYNC_FINAL" != "4|1|0|1" ]]; then
    echo "Unexpected synchronized autocommit state: $AUTOCOMMIT_SYNC_FINAL" >&2
    exit 1
fi
log "PASS read-started rollback and synchronized autocommit = $AUTOCOMMIT_SYNC_FINAL"

expect_failure_contains "unsupported EXASOL savepoint" \
    "USE $SCHEMA; START TRANSACTION; SELECT COUNT(*) FROM TXN_POS_T; SAVEPOINT exasol_sp" \
    "SAVEPOINT with ENGINE=EXASOL"
expect_failure_contains "EXASOL access after savepoint" \
    "USE $SCHEMA; START TRANSACTION; SAVEPOINT before_exasol; SELECT COUNT(*) FROM TXN_POS_T" \
    "accessing ENGINE=EXASOL after SAVEPOINT"
log "PASS EXASOL user savepoints fail closed"

# Without remote statement savepoints, any statement error in an explicit
# transaction rolls back all preceding EXASOL work and marks the MariaDB
# transaction rollback-only. A later COMMIT must not preserve a prefix.
mysql -e "USE $SCHEMA; CREATE TABLE TXN_FAIL_T(ID INT NOT NULL) ENGINE=EXASOL; INSERT INTO TXN_FAIL_T VALUES (1)"
if mysql --force -e "USE $SCHEMA; SET autocommit=0; INSERT INTO TXN_FAIL_T VALUES (2); INSERT INTO TXN_FAIL_T VALUES (NULL); COMMIT" \
    >"$BASE_DIR/txn-failure.out" 2>"$BASE_DIR/txn-failure.err"; then
    echo "Explicit transaction with failing EXASOL statement unexpectedly succeeded" >&2
    exit 1
fi
TXN_FAIL_FINAL=$(mysql --batch --raw --skip-column-names -e \
    "USE $SCHEMA; SELECT COUNT(*), SUM(ID) FROM TXN_FAIL_T")
if [[ "$TXN_FAIL_FINAL" != $'1\t1' ]]; then
    echo "Failed EXASOL transaction preserved a committed prefix: $TXN_FAIL_FINAL" >&2
    exit 1
fi
log "PASS failed explicit transaction rolled back all EXASOL work"

# THD/session teardown must never commit explicit work.
mysql -e "USE $SCHEMA; SET autocommit=0; INSERT INTO TXN_FAIL_T VALUES (9)"
expect_scalar "disconnect rolls back active EXASOL transaction" \
    "USE $SCHEMA; SELECT COUNT(*) FROM TXN_FAIL_T WHERE ID=9" "0"

if [[ "$POSITIONED_DML_TEST_ONLY" == "1" ]]; then
    mysql -e "DROP DATABASE $SCHEMA" >/dev/null
    FOCUSED_TABLES=$(sql_exasol "select count(*) from sys.exa_all_tables where table_schema='$SCHEMA'")
    echo "$FOCUSED_TABLES" | grep -q '"data":\[\[0\]\]'
    log "SessionGW focused positioned DML workload passed"
    exit 0
fi

log "PASS positioned update delete"

expect_scalar "prepared statement count" \
    "USE $SCHEMA; PREPARE s FROM 'SELECT COUNT(*) FROM T WHERE ID > ?'; SET @p=1; EXECUTE s USING @p; DEALLOCATE PREPARE s;" \
    "2"

expect_failure "unsupported key definition" \
    "USE $SCHEMA; CREATE TABLE BAD_KEY(ID INT, KEY(ID)) ENGINE=EXASOL"
expect_handler_unsupported "unsupported default clause" \
    "USE $SCHEMA; CREATE TABLE BAD_DEFAULT(ID INT DEFAULT 1) ENGINE=EXASOL"
expect_failure "unsupported auto increment" \
    "USE $SCHEMA; CREATE TABLE BAD_AUTO(ID INT AUTO_INCREMENT PRIMARY KEY) ENGINE=EXASOL"
expect_handler_unsupported "unsupported binary string" \
    "USE $SCHEMA; CREATE TABLE BAD_BINARY(B BINARY(8)) ENGINE=EXASOL"
expect_handler_unsupported "unsupported multi-bit value" \
    "USE $SCHEMA; CREATE TABLE BAD_BIT(B BIT(2)) ENGINE=EXASOL"
expect_handler_unsupported "unsupported explicit table charset" \
    "USE $SCHEMA; CREATE TABLE BAD_CHARSET(V VARCHAR(8)) ENGINE=EXASOL DEFAULT CHARSET=latin1"
expect_handler_unsupported "unsupported per-column charset" \
    "USE $SCHEMA; CREATE TABLE BAD_COLUMN_CHARSET(V VARCHAR(8) CHARACTER SET latin1) ENGINE=EXASOL"
expect_handler_unsupported "unsupported explicit binary collation" \
    "USE $SCHEMA; CREATE TABLE BAD_COLUMN_COLLATION(V VARCHAR(8) COLLATE utf8mb4_bin) ENGINE=EXASOL"
expect_handler_unsupported "unsupported explicit non-default collation" \
    "USE $SCHEMA; CREATE TABLE BAD_COLUMN_COLLATION_CI(V VARCHAR(8) COLLATE utf8mb4_general_ci) ENGINE=EXASOL"
expect_handler_unsupported "unsupported varchar binary semantics" \
    "USE $SCHEMA; CREATE TABLE BAD_COLUMN_BINARY(V VARCHAR(8) BINARY) ENGINE=EXASOL"
expect_handler_unsupported "unsupported alter table" \
    "USE $SCHEMA; ALTER TABLE T ADD COLUMN EXTRA INT"
expect_handler_unsupported "unsupported truncate table" \
    "USE $SCHEMA; TRUNCATE TABLE T"
expect_handler_unsupported "unsupported rename table" \
    "USE $SCHEMA; RENAME TABLE T TO T_RENAMED"
expect_scalar "rejected DDL preserved table rows" "USE $SCHEMA; SELECT COUNT(*) FROM T" "3"
REJECTED_TABLES=$(sql_exasol "select count(*) from sys.exa_all_tables where table_schema='$SCHEMA' and table_name like 'BAD_%'")
echo "$REJECTED_TABLES" | grep -q '"data":\[\[0\]\]'
log "PASS rejected DDL did not mutate Exasol"

sql_exasol "CREATE TABLE $SCHEMA.REMOTE_GUARD(ID DECIMAL(18,0) NOT NULL)" >/dev/null
sql_exasol "INSERT INTO $SCHEMA.REMOTE_GUARD VALUES 42" >/dev/null
expect_failure "existing remote table is preserved" \
    "USE $SCHEMA; CREATE TABLE REMOTE_GUARD(ID BIGINT NOT NULL) ENGINE=EXASOL"
REMOTE_GUARD=$(sql_exasol "select count(*), min(id) from $SCHEMA.REMOTE_GUARD")
echo "$REMOTE_GUARD" | tee -a "$REPORT" | grep -Eq '"data":\[\[1\],\["?42"?\]\]'
log "PASS existing remote table contents preserved"

mysql -e "USE $SCHEMA; CREATE TABLE NULLABILITY_GUARD(REQUIRED_ID INT NOT NULL, OPTIONAL_NAME VARCHAR(8) NULL) ENGINE=EXASOL"
expect_exasol_failure "remote not null enforcement" \
    "INSERT INTO $SCHEMA.NULLABILITY_GUARD VALUES (NULL, NULL)"
sql_exasol "INSERT INTO $SCHEMA.NULLABILITY_GUARD VALUES (1, NULL)" >/dev/null
expect_scalar "nullable column mapping" \
    "USE $SCHEMA; SELECT COUNT(*) FROM NULLABILITY_GUARD WHERE OPTIONAL_NAME IS NULL" "1"
log "PASS nullability mapped to Exasol"

mysql -e "USE $SCHEMA; CREATE TABLE BIGINT_GUARD(ID BIGINT NOT NULL) ENGINE=EXASOL; INSERT INTO BIGINT_GUARD VALUES (-9223372036854775808), (9223372036854775807)"
expect_scalar "signed BIGINT boundary round trip" \
    "USE $SCHEMA; SELECT CONCAT(MIN(ID), '|', MAX(ID)) FROM BIGINT_GUARD" \
    "-9223372036854775808|9223372036854775807"
BIGINT_REMOTE_TYPE=$(sql_exasol "select column_type from sys.exa_all_columns where column_schema='$SCHEMA' and column_table='BIGINT_GUARD' and column_name='ID'")
echo "$BIGINT_REMOTE_TYPE" | tee -a "$REPORT" | grep -q '"data":\[\["DECIMAL(19,0)"\]\]'
BIGINT_REMOTE=$(sql_exasol "select min(id), max(id) from $SCHEMA.BIGINT_GUARD")
echo "$BIGINT_REMOTE" | tee -a "$REPORT" | grep -Eq '"data":\[\["?-9223372036854775808"?\],\["?9223372036854775807"?\]\]'
log "PASS signed BIGINT uses DECIMAL(19,0) and preserves boundaries"

mysql -e "USE $SCHEMA; CREATE TABLE ABORT_GUARD(ID INT, NAME VARCHAR(8) NULL) ENGINE=EXASOL"
sql_exasol "DROP TABLE $SCHEMA.ABORT_GUARD" >/dev/null
sql_exasol "CREATE TABLE $SCHEMA.ABORT_GUARD(ID DECIMAL(18,0), NAME VARCHAR(8) UTF8 NOT NULL)" >/dev/null
sql_exasol "INSERT INTO $SCHEMA.ABORT_GUARD VALUES (1, 'before')" >/dev/null
expect_failure "failed insert aborts accepted prefix" \
    "USE $SCHEMA; INSERT INTO ABORT_GUARD VALUES (2, 'valid'), (3, NULL)"
ABORT_INSERT_STATE=$(sql_exasol "select count(*), sum(id) from $SCHEMA.ABORT_GUARD")
echo "$ABORT_INSERT_STATE" | tee -a "$REPORT" | grep -Eq '"data":\[\[1\],\["?1"?\]\]'
expect_failure "failed update aborts accepted prefix" \
    "USE $SCHEMA; UPDATE ABORT_GUARD SET NAME=CASE WHEN ID=1 THEN NULL ELSE 'changed' END"
ABORT_UPDATE_STATE=$(sql_exasol "select count(*), min(name) from $SCHEMA.ABORT_GUARD")
echo "$ABORT_UPDATE_STATE" | tee -a "$REPORT" | grep -q '"data":\[\[1\],\["before"\]\]'
log "PASS failed DML left no committed prefix"

mysql --table <<SQL | tee -a "$REPORT"
USE $SCHEMA;
CREATE TABLE TYPES(
  ID INT,
  B BIT,
  TI TINYINT,
  SI SMALLINT,
  MI MEDIUMINT,
  I INT,
  BI BIGINT,
  D DECIMAL(12,2),
  F FLOAT,
  DBL DOUBLE,
  DT DATE,
  TS TIMESTAMP NULL,
  C CHAR(3),
  V VARCHAR(80)
) ENGINE=EXASOL;
INSERT INTO TYPES VALUES
  (1, b'1', -12, -1234, -123456, -1234567, -12345678901, 1234.56, 1.25, 2.5,
   '2026-07-10', '2026-07-10 11:12:13', 'abc', 'unicode äöü'),
  (2, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL, NULL);
SELECT COUNT(*) AS C FROM TYPES;
SELECT ID, B, TI, SI, MI, I, BI, D, ROUND(DBL,1) AS RDBL, DT, TS, C, V FROM TYPES ORDER BY ID;
DELETE FROM TYPES WHERE ID=2;
SELECT ID, D, DT, TS, V FROM TYPES ORDER BY ID;
CREATE TABLE TYPE_UPD(ID INT, D DECIMAL(12,2), DT DATE, TS TIMESTAMP(6) NULL, V VARCHAR(80)) ENGINE=EXASOL;
INSERT INTO TYPE_UPD VALUES (1, 1.25, '2026-07-10', '2026-07-10 11:12:13.123456', 'before');
UPDATE TYPE_UPD SET D=42.42, DT='2026-07-11', TS='2026-07-11 01:02:03.654321', V='updated' WHERE ID=1;
SELECT ID, D, DT, TS, V FROM TYPE_UPD ORDER BY ID;
CREATE TABLE TYPE_EDGE(ID INT, BI BIGINT, D DECIMAL(36,18), DT DATETIME(6)) ENGINE=EXASOL;
INSERT INTO TYPE_EDGE VALUES
  (1, -9223372036854775808, -999999999999999999.999999999999999999, '1900-01-01 00:00:00.000001'),
  (2,  9223372036854775807,  999999999999999999.999999999999999999, '2038-01-19 03:14:07.999999');
SELECT ID, BI, D, DT FROM TYPE_EDGE ORDER BY ID;
SQL
expect_scalar "type matrix final count" "USE $SCHEMA; SELECT COUNT(*) FROM TYPES" "1"
TYPE_UPD_FINAL=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT ID FROM TYPE_UPD; SELECT D FROM TYPE_UPD; SELECT DT FROM TYPE_UPD; SELECT TS FROM TYPE_UPD; SELECT V FROM TYPE_UPD;" | paste -sd'|' -)
if [[ "$TYPE_UPD_FINAL" != "1|42.42|2026-07-11|2026-07-11 01:02:03.654321|updated" ]]; then
    echo "Unexpected type update final state: $TYPE_UPD_FINAL" >&2
    exit 1
fi
log "PASS type update final $TYPE_UPD_FINAL"
mysql -e "SET time_zone='+02:00'; USE $SCHEMA;
  CREATE TABLE TYPE_TZ(ID INT, TS TIMESTAMP(6)) ENGINE=EXASOL;
  INSERT INTO TYPE_TZ VALUES (1, '2026-07-10 11:12:13.123456');"
TYPE_TZ_FINAL=$(mysql --batch --raw --skip-column-names -e \
  "SET time_zone='-03:00'; USE $SCHEMA; SELECT TS FROM TYPE_TZ WHERE ID=1")
if [[ "$TYPE_TZ_FINAL" != "2026-07-10 06:12:13.123456" ]]; then
    echo "Unexpected cross-session timezone result: $TYPE_TZ_FINAL" >&2
    exit 1
fi
TYPE_TZ_REMOTE=$(sql_exasol "select column_type from exa_all_columns where column_schema='$SCHEMA' and column_table='TYPE_TZ' and column_name='TS'")
echo "$TYPE_TZ_REMOTE" | tee -a "$REPORT" | grep -q 'TIMESTAMP(6) WITH LOCAL TIME ZONE'
log "PASS TIMESTAMP cross-session timezone conversion $TYPE_TZ_FINAL"
TYPE_EDGE_FINAL=$(mysql --batch --raw --skip-column-names -e \
  "USE $SCHEMA; SELECT ID, BI, D, DT FROM TYPE_EDGE ORDER BY ID" | tr '\t' '|' | paste -sd';' -)
if [[ "$TYPE_EDGE_FINAL" != "1|-9223372036854775808|-999999999999999999.999999999999999999|1900-01-01 00:00:00.000001;2|9223372036854775807|999999999999999999.999999999999999999|2038-01-19 03:14:07.999999" ]]; then
    echo "Unexpected type edge final state: $TYPE_EDGE_FINAL" >&2
    exit 1
fi
log "PASS decimal, BIGINT, and timestamp edge round-trip $TYPE_EDGE_FINAL"
expect_failure "timestamp outside Arrow ns range" \
    "USE $SCHEMA; INSERT INTO TYPE_EDGE VALUES (3, 0, 0, '1000-01-01 00:00:00')"
expect_failure "zero date rejected before mutation" \
    "SET sql_mode='ALLOW_INVALID_DATES'; USE $SCHEMA; INSERT INTO TYPE_EDGE VALUES (4, 0, 0, '0000-00-00 00:00:00')"
expect_scalar "rejected temporal rows did not mutate" "USE $SCHEMA; SELECT COUNT(*) FROM TYPE_EDGE" "2"
log "PASS supported type matrix"

mysql -e "USE $SCHEMA;
  CREATE TABLE SPARSE_UPD(ID INT, KEEP_COL VARCHAR(20), NULLABLE_COL VARCHAR(20), VALUE_COL INT) ENGINE=EXASOL;
  INSERT INTO SPARSE_UPD VALUES (1, 'keep_1', 'before_1', 10), (2, 'keep_2', 'before_2', 20);
  UPDATE SPARSE_UPD
     SET NULLABLE_COL=CASE WHEN ID=1 THEN NULL ELSE 'changed_2' END,
         VALUE_COL=VALUE_COL+1;"
SPARSE_FINAL=$(mysql --batch --raw --skip-column-names -e \
  "USE $SCHEMA; SELECT ID, KEEP_COL, COALESCE(NULLABLE_COL, 'NULL'), VALUE_COL FROM SPARSE_UPD ORDER BY ID" \
  | tr '\t' '|' | paste -sd';' -)
if [[ "$SPARSE_FINAL" != "1|keep_1|NULL|11;2|keep_2|changed_2|21" ]]; then
    echo "Unexpected sparse update state: $SPARSE_FINAL" >&2
    exit 1
fi
log "PASS sparse mixed-null update $SPARSE_FINAL"

LOAD_FILE=$BASE_DIR/load_data.csv
printf '20,LoadA\n21,LoadB\n22,LoadC\n' > "$LOAD_FILE"
mysql --table <<SQL | tee -a "$REPORT"
USE $SCHEMA;
CREATE TABLE LOAD_T(ID INT, NAME VARCHAR(40)) ENGINE=EXASOL;
LOAD DATA LOCAL INFILE '$LOAD_FILE' INTO TABLE LOAD_T FIELDS TERMINATED BY ',';
SELECT COUNT(*) AS C, SUM(ID) AS S, MAX(NAME) AS M FROM LOAD_T;
SQL
expect_scalar "load data count" "USE $SCHEMA; SELECT COUNT(*) FROM LOAD_T" "3"
log "PASS load data"

PERF_SQL=$BASE_DIR/perf.sql
{
    echo "USE $SCHEMA;"
    echo "CREATE TABLE PERF_T(ID INT, NAME VARCHAR(40)) ENGINE=EXASOL;"
    for start in $(seq 1 "$PERF_INSERT_BATCH_ROWS" "$PERF_ROWS"); do
        end=$((start + PERF_INSERT_BATCH_ROWS - 1))
        if (( end > PERF_ROWS )); then end=$PERF_ROWS; fi
        printf 'INSERT INTO PERF_T VALUES '
        first=1
        for id in $(seq "$start" "$end"); do
            if (( first )); then first=0; else printf ','; fi
            printf '(%s, '\''Perf_%s'\'')' "$id" "$id"
        done
        printf ';\n'
    done
    echo "SELECT COUNT(*) AS C, SUM(ID) AS S FROM PERF_T;"
} > "$PERF_SQL"
perf_start=$(date +%s)
insert_start=$(date +%s)
mysql --table < "$PERF_SQL" | tee -a "$REPORT"
insert_end=$(date +%s)

# Wide 100k+ profile for every MariaDB type accepted by ENGINE=EXASOL. INSERT
# and UPDATE traverse native multi-vector writes; SELECT * forces Arrow decoding
# and MariaDB field materialization for every value rather than pushdown-only
# aggregate validation.
TYPE_PERF_FILE=$BASE_DIR/perf_types.tsv
TYPE_BOOL_SQL=$BASE_DIR/perf_bool.sql
python3 - "$PERF_ROWS" "$TYPE_PERF_FILE" "$TYPE_BOOL_SQL" "$PERF_INSERT_BATCH_ROWS" "$SCHEMA" <<'PY_TYPES'
import datetime
import sys
rows = int(sys.argv[1])
path = sys.argv[2]
bool_path = sys.argv[3]
batch_rows = int(sys.argv[4])
schema = sys.argv[5]
with open(path, "w", encoding="utf-8") as out:
    for row_id in range(1, rows + 1):
        dt = datetime.date(2000, 1, 1) + datetime.timedelta(days=row_id % 7000)
        dts = datetime.datetime(2000, 1, 1) + datetime.timedelta(seconds=row_id, microseconds=row_id % 1000000)
        ts = datetime.datetime(2020, 1, 1) + datetime.timedelta(seconds=row_id, microseconds=row_id % 1000000)
        values = [
            row_id,
            row_id % 255 - 127, row_id % 256,
            row_id % 65535 - 32767, row_id % 65536,
            row_id % 16777215 - 8388607, row_id % 16777216,
            row_id - 50000, row_id,
            row_id - 50000, 2000 + row_id % 26,
            f"{row_id / 100.0:.2f}", f"{row_id / 1000.0:.3f}",
            f"{row_id / 10000.0:.4f}",
            f"{row_id}.{row_id % 1000000:06d}", dt.isoformat(),
            dts.strftime("%Y-%m-%d %H:%M:%S.%f"),
            ts.strftime("%Y-%m-%d %H:%M:%S.%f"),
            f"C{row_id % 100000000000:011d}", f"typed_{row_id}_ä",
        ]
        out.write("\t".join(map(str, values)) + "\n")
with open(bool_path, "w", encoding="utf-8") as out:
    out.write(f"USE {schema}; CREATE TABLE PERF_BOOL(ID INT, B BIT(1)) ENGINE=EXASOL;\n")
    for first in range(1, rows + 1, batch_rows):
        last = min(rows, first + batch_rows - 1)
        values = ",".join(f"({row_id},b'{row_id % 2}')" for row_id in range(first, last + 1))
        out.write(f"INSERT INTO PERF_BOOL VALUES {values};\n")
PY_TYPES

type_perf_insert_start=$(date +%s%3N)
mysql -e "USE $SCHEMA;
  CREATE TABLE PERF_TYPES(
    ID INT, TI TINYINT, UTI TINYINT UNSIGNED,
    SI SMALLINT, USI SMALLINT UNSIGNED,
    MI MEDIUMINT, UMI MEDIUMINT UNSIGNED,
    I INT, UI INT UNSIGNED, BI BIGINT, Y YEAR,
    F FLOAT, DBL DOUBLE, D18 DECIMAL(18,4), D36 DECIMAL(36,18),
    DT DATE, DTS DATETIME(6), TS TIMESTAMP(6),
    C CHAR(12), V VARCHAR(80)
  ) ENGINE=EXASOL;
  LOAD DATA LOCAL INFILE '$TYPE_PERF_FILE' INTO TABLE PERF_TYPES
    (ID,TI,UTI,SI,USI,MI,UMI,I,UI,BI,Y,F,DBL,D18,D36,DT,DTS,TS,C,V);"
mysql < "$TYPE_BOOL_SQL"
type_perf_insert_end=$(date +%s%3N)

type_perf_read_start=$(date +%s%3N)
mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT * FROM PERF_TYPES ORDER BY ID; SELECT * FROM PERF_BOOL ORDER BY ID" >/dev/null
type_perf_read_end=$(date +%s%3N)

type_perf_update_start=$(date +%s%3N)
mysql -e "USE $SCHEMA;
  UPDATE PERF_TYPES SET
    TI=TI, UTI=UTI, SI=SI, USI=USI, MI=MI, UMI=UMI,
    I=I, UI=UI, BI=BI, Y=Y, F=F, DBL=DBL,
    D18=CASE WHEN MOD(ID,10)=0 THEN ID/10000.0 ELSE D18 END,
    D36=-D36, DT=DT,
    DTS=TIMESTAMPADD(MICROSECOND,1,DTS),
    TS=CASE WHEN MOD(ID,10)=0 THEN '2020-01-01 00:00:00.123456' ELSE TS END,
    C=C, V=CASE WHEN MOD(ID,10)=0 THEN CONCAT('updated_',ID) ELSE V END;
  UPDATE PERF_BOOL SET B=B;"
type_perf_update_end=$(date +%s%3N)

type_perf_delete_start=$(date +%s%3N)
mysql -e "USE $SCHEMA;
  DELETE FROM PERF_TYPES WHERE ID > $((PERF_ROWS - PERF_DELETE_ROWS));
  DELETE FROM PERF_BOOL WHERE ID > $((PERF_ROWS - PERF_DELETE_ROWS));"
type_perf_delete_end=$(date +%s%3N)
TYPE_PERF_FINAL=$(mysql --batch --raw --skip-column-names -e \
  "USE $SCHEMA; SELECT COUNT(*), SUM(ID), COUNT(D18), COUNT(TS), COUNT(V), (SELECT COUNT(B) FROM PERF_BOOL) FROM PERF_TYPES" | tr '\t' '|')
type_perf_count=$((PERF_ROWS - PERF_DELETE_ROWS))
type_perf_sum=$((type_perf_count * (type_perf_count + 1) / 2))
EXPECTED_TYPE_PERF="$type_perf_count|$type_perf_sum|$type_perf_count|$type_perf_count|$type_perf_count|$type_perf_count"
if [[ "$TYPE_PERF_FINAL" != "$EXPECTED_TYPE_PERF" ]]; then
    echo "Unexpected typed performance final state: expected $EXPECTED_TYPE_PERF got $TYPE_PERF_FINAL" >&2
    exit 1
fi
log "PASS supported-type performance rows=$PERF_ROWS columns=21 insert_ms=$((type_perf_insert_end-type_perf_insert_start)) read_ms=$((type_perf_read_end-type_perf_read_start)) update_ms=$((type_perf_update_end-type_perf_update_start)) delete_ms=$((type_perf_delete_end-type_perf_delete_start)) final=$TYPE_PERF_FINAL"

# One statement, one update operation, and repeated native vectors over 100k+
# rows. Two sparse columns change, NULL/non-NULL values share batches, and the
# omitted column proves the full-row fallback is gone.
sparse_scale_start=$(date +%s)
mysql --table -e "USE $SCHEMA;
  UPDATE PERF_T
     SET ID=ID,
         NAME=CASE WHEN MOD(ID, 2)=0 THEN NULL ELSE CONCAT('Sparse_', ID) END;
  SELECT COUNT(*) AS C,
         COUNT(CASE WHEN NAME IS NULL THEN 1 END) AS N,
         COUNT(CASE WHEN NAME LIKE 'Sparse_%' THEN 1 END) AS V
    FROM PERF_T;" | tee -a "$REPORT"
sparse_scale_end=$(date +%s)
expected_sparse_nulls=$((PERF_ROWS / 2))
expected_sparse_values=$((PERF_ROWS - expected_sparse_nulls))
expected_sparse_id_sum=$((PERF_ROWS * (PERF_ROWS + 1) / 2))
SPARSE_SCALE_FINAL=$(mysql --batch --raw --skip-column-names -e \
  "USE $SCHEMA; SELECT COUNT(*), COUNT(CASE WHEN NAME IS NULL THEN 1 END), COUNT(CASE WHEN NAME LIKE 'Sparse_%' THEN 1 END), SUM(ID) FROM PERF_T" \
  | tr '\t' '|')
EXPECTED_SPARSE_SCALE="$PERF_ROWS|$expected_sparse_nulls|$expected_sparse_values|$expected_sparse_id_sum"
if [[ "$SPARSE_SCALE_FINAL" != "$EXPECTED_SPARSE_SCALE" ]]; then
    echo "Unexpected multi-vector sparse update state: expected $EXPECTED_SPARSE_SCALE got $SPARSE_SCALE_FINAL" >&2
    exit 1
fi
log "PASS multi-vector sparse update rows=$PERF_ROWS batch_rows=$UPDATE_BATCH_ROWS changed_columns=2 final=$SPARSE_SCALE_FINAL seconds=$((sparse_scale_end - sparse_scale_start))"

update_start=$(date +%s)
mysql --table -e "USE $SCHEMA; UPDATE PERF_T SET NAME='Perf_updated' WHERE ID <= $PERF_UPDATE_ROWS; SELECT COUNT(*) AS U FROM PERF_T WHERE NAME='Perf_updated';" | tee -a "$REPORT"
update_end=$(date +%s)
delete_start=$(date +%s)
mysql --table -e "USE $SCHEMA; DELETE FROM PERF_T WHERE ID > $((PERF_ROWS - PERF_DELETE_ROWS)); SELECT COUNT(*) AS C, SUM(ID) AS S, COUNT(CASE WHEN NAME='Perf_updated' THEN 1 END) AS U FROM PERF_T;" | tee -a "$REPORT"
delete_end=$(date +%s)
perf_end=$(date +%s)
expected_perf_count=$((PERF_ROWS - PERF_DELETE_ROWS))
deleted_sum=$(((PERF_ROWS - PERF_DELETE_ROWS + 1 + PERF_ROWS) * PERF_DELETE_ROWS / 2))
expected_perf_sum=$((PERF_ROWS * (PERF_ROWS + 1) / 2 - deleted_sum))
PERF_FINAL=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT COUNT(*) FROM PERF_T; SELECT SUM(ID) FROM PERF_T; SELECT COUNT(*) FROM PERF_T WHERE NAME='Perf_updated';" | paste -sd'|' -)
EXPECTED_PERF="$expected_perf_count|$expected_perf_sum|$PERF_UPDATE_ROWS"
if [[ "$PERF_FINAL" != "$EXPECTED_PERF" ]]; then
    echo "Unexpected performance baseline final state: expected $EXPECTED_PERF got $PERF_FINAL" >&2
    exit 1
fi
log "PASS performance baseline rows=$PERF_ROWS insert_batch=$PERF_INSERT_BATCH_ROWS update_rows=$PERF_UPDATE_ROWS delete_rows=$PERF_DELETE_ROWS insert_seconds=$((insert_end - insert_start)) update_seconds=$((update_end - update_start)) delete_seconds=$((delete_end - delete_start)) total_seconds=$((perf_end - perf_start)) final=$PERF_FINAL"

# Concurrent reads.
read_pids=()
for i in $(seq 1 "$READ_CLIENTS"); do
    (mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT COUNT(*) FROM T; SELECT SUM(ID) FROM T;" >"$BASE_DIR/read_$i.out") &
    read_pids+=("$!")
done
for pid in "${read_pids[@]}"; do
    wait "$pid"
done
log "PASS concurrent reads clients=$READ_CLIENTS"

# Exercise transport/session concurrency without intentionally serializing all
# writers on one DMP table lock. Transaction-conflict behavior on a shared table
# is covered by the dedicated conflict workload; each client here owns a target.
CONCURRENT_DDL="USE $SCHEMA;"
for client in $(seq 0 $((INSERT_CLIENTS - 1))); do
    CONCURRENT_DDL+=" CREATE TABLE CONCURRENT_INSERT_$client(ID INT, NAME VARCHAR(40)) ENGINE=EXASOL;"
done
mysql -e "$CONCURRENT_DDL"

for round in $(seq 0 $((INSERT_ROUNDS - 1))); do
    insert_pids=()
    for client in $(seq 0 $((INSERT_CLIENTS - 1))); do
        first_id=$((5 + (round * INSERT_CLIENTS + client) * INSERT_ROWS_PER_CLIENT))
        last_id=$((first_id + INSERT_ROWS_PER_CLIENT - 1))
        result_file="$BASE_DIR/insert_${round}_${client}.result"
        error_file="$BASE_DIR/insert_${round}_${client}.err"
        values=""
        for i in $(seq "$first_id" "$last_id"); do
            values+="${values:+,}($i, 'Name_$i')"
        done
        (
            if mysql --batch --raw --skip-column-names \
                -e "USE $SCHEMA; INSERT INTO CONCURRENT_INSERT_$client VALUES $values;" \
                2>"$error_file"; then
                echo "committed $first_id $last_id" >"$result_file"
            else
                cat "$error_file" >&2
                exit 1
            fi
        ) &
        insert_pids+=("$!")
    done
    for pid in "${insert_pids[@]}"; do
        wait "$pid"
    done
done

EXPECTED_CONCURRENT_COUNT=0
EXPECTED_CONCURRENT_SUM=0
COMMITTED_WRITERS=0
for result_file in "$BASE_DIR"/insert_*.result; do
    read -r outcome first_id last_id <"$result_file"
    if [[ "$outcome" != "committed" ]]; then
        echo "Unexpected concurrent writer outcome: $outcome" >&2
        exit 1
    fi
    rows=$((last_id - first_id + 1))
    EXPECTED_CONCURRENT_COUNT=$((EXPECTED_CONCURRENT_COUNT + rows))
    EXPECTED_CONCURRENT_SUM=$((EXPECTED_CONCURRENT_SUM + (first_id + last_id) * rows / 2))
    COMMITTED_WRITERS=$((COMMITTED_WRITERS + 1))
done

CONCURRENT_UNION=""
for client in $(seq 0 $((INSERT_CLIENTS - 1))); do
    CONCURRENT_UNION+="${CONCURRENT_UNION:+ UNION ALL }SELECT ID, NAME FROM CONCURRENT_INSERT_$client"
done
CONCURRENT_FINAL=$(mysql --batch --raw --skip-column-names -e \
    "USE $SCHEMA; SELECT COUNT(*), SUM(ID), COUNT(CASE WHEN NAME LIKE 'Name_%' THEN 1 END) FROM ($CONCURRENT_UNION) AS WRITES;")
EXPECTED_CONCURRENT="$EXPECTED_CONCURRENT_COUNT|$EXPECTED_CONCURRENT_SUM|$EXPECTED_CONCURRENT_COUNT"
CONCURRENT_FINAL=$(echo "$CONCURRENT_FINAL" | tr '\t' '|')
if [[ "$CONCURRENT_FINAL" != "$EXPECTED_CONCURRENT" ]]; then
    echo "Unexpected concurrent target state: expected $EXPECTED_CONCURRENT got $CONCURRENT_FINAL" >&2
    exit 1
fi
log "PASS concurrent inserts clients=$INSERT_CLIENTS rounds=$INSERT_ROUNDS rows_per_client=$INSERT_ROWS_PER_CLIENT committed=$COMMITTED_WRITERS distinct_targets=$INSERT_CLIENTS final=$CONCURRENT_FINAL"

FINAL_ROWS=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT COUNT(*) FROM T; SELECT SUM(ID) FROM T; SELECT COUNT(*) FROM T WHERE NAME LIKE 'Name_%';")
FINAL=$(echo "$FINAL_ROWS" | paste -sd'|' -)
EXPECTED="3|7|0"
if [[ "$FINAL" != "$EXPECTED" ]]; then
    echo "Unexpected MariaDB final state: expected $EXPECTED got $FINAL" >&2
    exit 1
fi
log "PASS mariadb final $FINAL"

DIRECT=$(sql_exasol "select count(*), sum(id), count(case when name like 'Name_%' then 1 end) from $SCHEMA.T")
echo "$DIRECT" | tee -a "$REPORT" >/dev/null
log "PASS direct Exasol final verification"

sql_exasol "DROP TABLE $SCHEMA.REMOTE_GUARD" >/dev/null
mysql -e "USE $SCHEMA; DROP TABLE ABORT_GUARD; DROP TABLE NULLABILITY_GUARD; DROP TABLE T" >/dev/null
ABSENT=$(sql_exasol "select count(*) from sys.exa_all_tables where table_schema='$SCHEMA' and table_name='T'")
echo "$ABSENT" | grep -q '"data":\[\[0\]\]'
log "PASS drop table removed backing Exasol table"

PERFORMANCE_SUMMARY=$(python3 - "$MDB/mariadb.err" <<'PY'
import re
import sys

values = {}
lines = 0
with open(sys.argv[1], encoding="utf-8", errors="replace") as log_file:
    for line in log_file:
        if "SessionGW performance:" not in line:
            continue
        lines += 1
        for name, value in re.findall(r"([a-z_]+)=([0-9]+)", line):
            values[name] = values.get(name, 0) + int(value)
if lines == 0:
    raise SystemExit("no SessionGW performance records found")
print("SessionGW performance aggregate: sessions=" + str(lines) + " " +
      " ".join(f"{name}={values[name]}" for name in sorted(values)))
PY
)
log "$PERFORMANCE_SUMMARY"

log "SessionGW MariaDB engine workload passed"
