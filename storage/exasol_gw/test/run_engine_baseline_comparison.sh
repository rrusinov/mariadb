#!/usr/bin/env bash
set -euo pipefail

# Compare the same MariaDB SQL DML baseline on local InnoDB and on the
# SessionGW-backed EXASOL storage engine. This is a baseline comparison, not a
# claim that a local embedded engine and a remote-gateway engine have identical
# responsibilities.

SCRIPT_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
MARIADB_SRC=${MARIADB_SRC:-$(cd "$SCRIPT_DIR/../../.." && pwd -P)}
MARIADB_BUILD=${MARIADB_BUILD:-$MARIADB_SRC/build-sessiongw}
DB_EXANANO=${DB_EXANANO:-$(cd "$MARIADB_SRC/../db.exanano" && pwd -P)}
NANO_RUN=${NANO_RUN:-$DB_EXANANO/.build/exasol-nano-db-2026.2.0-nano.3-x86_64.run}
BASE_DIR=${BASE_DIR:-${TMPDIR:-/tmp}/exasol-gw-engine-compare.$$}
EXASOL_PORT=${EXASOL_PORT:-8592}
NANO_DBRAM=${EXASOL_NANO_DBRAM:-12288}
PERF_ROWS=${PERF_ROWS:-100000}
PERF_INSERT_BATCH_ROWS=${PERF_INSERT_BATCH_ROWS:-10000}
PERF_UPDATE_ROWS=${PERF_UPDATE_ROWS:-$((PERF_ROWS / 10))}
PERF_DELETE_ROWS=${PERF_DELETE_ROWS:-$((PERF_ROWS / 20))}
BULK_LOAD_ROWS=${BULK_LOAD_ROWS:-1000000}
BULK_LOAD_CHAR_LENGTH=${BULK_LOAD_CHAR_LENGTH:-64}
BULK_LOAD_VARCHAR_A_LENGTH=${BULK_LOAD_VARCHAR_A_LENGTH:-520}
BULK_LOAD_VARCHAR_B_LENGTH=${BULK_LOAD_VARCHAR_B_LENGTH:-520}
BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES=${BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES:-1073741824}
BULK_LOAD_REPETITIONS=${BULK_LOAD_REPETITIONS:-2}
# Wide bulk rows need a lower row ceiling so each native batch remains below
# the negotiated SessionGateway frame limit. This affects only the restarted
# MariaDB used for the bulk phase, not the existing narrow-row baseline.
BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS=${BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS:-512}
if (( PERF_UPDATE_ROWS == 0 )); then PERF_UPDATE_ROWS=1; fi
if (( PERF_DELETE_ROWS == 0 )); then PERF_DELETE_ROWS=1; fi
for value in "$EXASOL_PORT" "$NANO_DBRAM" "$BULK_LOAD_ROWS" "$BULK_LOAD_CHAR_LENGTH" \
             "$BULK_LOAD_VARCHAR_A_LENGTH" "$BULK_LOAD_VARCHAR_B_LENGTH" \
             "$BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS" "$BULK_LOAD_REPETITIONS"; do
    if [[ ! "$value" =~ ^[1-9][0-9]*$ ]]; then
        echo "Benchmark sizes and Nano DBRAM must be positive integers" >&2
        exit 2
    fi
done
if [[ ! "$BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES" =~ ^[0-9]+$ ]]; then
    echo "BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES must be a non-negative integer" >&2
    exit 2
fi
if (( BULK_LOAD_REPETITIONS < 2 )); then
    echo "BULK_LOAD_REPETITIONS must be at least 2 for an order-controlled comparison" >&2
    exit 2
fi

# Bound the native request conservatively: numeric values, three variable-size
# length vectors, null markers, alignment, and protocol metadata all fit in the
# per-row/constant allowances below. Reject overrides rather than discovering
# an oversized frame after a long input generation pass.
SESSIONGW_MAX_PAYLOAD_BYTES=$((1024 * 1024))
BULK_NATIVE_FRAME_UPPER_BOUND_BYTES=$((4096 + BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS *
    (BULK_LOAD_CHAR_LENGTH + BULK_LOAD_VARCHAR_A_LENGTH + BULK_LOAD_VARCHAR_B_LENGTH + 12 + 64)))
if (( BULK_NATIVE_FRAME_UPPER_BOUND_BYTES > SESSIONGW_MAX_PAYLOAD_BYTES )); then
    echo "Bulk width/batch upper bound $BULK_NATIVE_FRAME_UPPER_BOUND_BYTES exceeds the SessionGateway 1 MiB payload limit; reduce BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS or column lengths" >&2
    exit 2
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
BULK_LOAD_FILE=$BASE_DIR/bulk-load.tsv
MARIADB_PID=
NANO_PID=
BULK_LAST_LOAD_MS=
mkdir -p "$NANO_APP" "$NANO_DB" "$MDB"
: >"$REPORT"

# Stop the current benchmark server cleanly enough for the same data directory
# to be reopened with bulk-specific SessionGateway batch sizing.
stop_mariadb() {
    if [[ -n "$MARIADB_PID" ]] && kill -0 "$MARIADB_PID" >/dev/null 2>&1; then
        kill "$MARIADB_PID" >/dev/null 2>&1 || true
        wait "$MARIADB_PID" >/dev/null 2>&1 || true
    fi
    MARIADB_PID=
    rm -f "$PIDFILE" "$SOCKET"
}

# Nano is launched as this shell's child so cleanup can wait for shutdown and
# never leave a port-owning process behind for the next benchmark.
stop_nano() {
    if [[ -n "$NANO_PID" ]] && kill -0 "$NANO_PID" >/dev/null 2>&1; then
        kill "$NANO_PID" >/dev/null 2>&1 || true
        wait "$NANO_PID" >/dev/null 2>&1 || true
    fi
    NANO_PID=
    rm -f "$NANO_BASE/pid"
}

cleanup() {
    stop_mariadb
    stop_nano
}
trap cleanup EXIT

log() { printf '%s\n' "$*" | tee -a "$REPORT"; }

sql_exasol() {
    (cd "$DB_EXANANO" && c4 sqlclient --usetls --skiptlsverify --user sys --password exasol \
        --connection "localhost:$EXASOL_PORT" --query "$1")
}

mysql() {
    "$MARIADB_BUILD/client/mariadb" --no-defaults --local-infile=1 --socket="$SOCKET" "$@"
}

now_ns() { date +%s%N; }
elapsed_ms() { echo $(( ($2 - $1) / 1000000 )); }

# Start MariaDB with an explicit insert batch ceiling. The initial invocation
# retains the historical baseline setting; the bulk invocation uses a safe
# ceiling for the deliberately wide rows.
start_mariadb() {
    local insert_batch_rows=$1
    local instrumentation=$2
    EXASOL_SESSIONGW_HOST=localhost \
    EXASOL_SESSIONGW_PORT=$EXASOL_PORT \
    EXASOL_SESSIONGW_USER=sys \
    EXASOL_SESSIONGW_PASSWORD=exasol \
    EXASOL_SESSIONGW_IDENTITY_MODE=service_account \
    EXASOL_SESSIONGW_ALLOWED_MARIADB_USERS="$(id -un)" \
    EXASOL_SESSIONGW_TLS=skip_verify \
    EXASOL_SESSIONGW_INSERT_BATCH_ROWS=$insert_batch_rows \
    EXASOL_SESSIONGW_INSTRUMENTATION=$instrumentation \
    "$MARIADB_BUILD/sql/mariadbd" --no-defaults \
        --datadir="$MDB/data" --socket="$SOCKET" --pid-file="$PIDFILE" \
        --port=0 --skip-networking \
        --plugin-dir="$MARIADB_BUILD/storage/exasol_gw" \
        --plugin-load-add=ha_exasol_gw.so \
        --log-error="$MDB/mariadb.err" --skip-grant-tables --local-infile=1 --user="$(id -un)" \
        >"$MDB/stdout.log" 2>&1 &
    MARIADB_PID=$!

    for _ in $(seq 1 60); do
        if mysql -e 'select 1' >/dev/null 2>&1; then break; fi
        sleep 1
    done
    mysql -e 'select 1' >/dev/null
}

write_perf_sql() {
    local schema=$1
    local engine=$2
    local file=$3
    {
        echo "DROP DATABASE IF EXISTS $schema;"
        echo "CREATE DATABASE $schema;"
        echo "USE $schema;"
        echo "CREATE TABLE PERF_T(ID INT, NAME VARCHAR(40)) ENGINE=$engine;"
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
    } > "$file"
}

run_engine() {
    local label=$1
    local schema=$2
    local engine=$3
    local sql_file=$BASE_DIR/${label}.insert.sql
    write_perf_sql "$schema" "$engine" "$sql_file"

    local total_start insert_start insert_end update_start update_end delete_start delete_end total_end
    total_start=$(now_ns)
    insert_start=$(now_ns)
    mysql --table < "$sql_file" >"$BASE_DIR/${label}.insert.out"
    insert_end=$(now_ns)

    update_start=$(now_ns)
    mysql --table -e "USE $schema; UPDATE PERF_T SET NAME='Perf_updated' WHERE ID <= $PERF_UPDATE_ROWS; SELECT COUNT(*) AS U FROM PERF_T WHERE NAME='Perf_updated';" >"$BASE_DIR/${label}.update.out"
    update_end=$(now_ns)

    delete_start=$(now_ns)
    mysql --table -e "USE $schema; DELETE FROM PERF_T WHERE ID > $((PERF_ROWS - PERF_DELETE_ROWS)); SELECT COUNT(*) AS C, SUM(ID) AS S, COUNT(CASE WHEN NAME='Perf_updated' THEN 1 END) AS U FROM PERF_T;" >"$BASE_DIR/${label}.delete.out"
    delete_end=$(now_ns)
    total_end=$(now_ns)

    local expected_count=$((PERF_ROWS - PERF_DELETE_ROWS))
    local deleted_sum=$(((PERF_ROWS - PERF_DELETE_ROWS + 1 + PERF_ROWS) * PERF_DELETE_ROWS / 2))
    local expected_sum=$((PERF_ROWS * (PERF_ROWS + 1) / 2 - deleted_sum))
    local final expected
    final=$(mysql --batch --raw --skip-column-names -e "USE $schema; SELECT COUNT(*) FROM PERF_T; SELECT SUM(ID) FROM PERF_T; SELECT COUNT(*) FROM PERF_T WHERE NAME='Perf_updated';" | paste -sd'|' -)
    expected="$expected_count|$expected_sum|$PERF_UPDATE_ROWS"
    if [[ "$final" != "$expected" ]]; then
        echo "Unexpected $label final state: expected $expected got $final" >&2
        exit 1
    fi

    local insert_ms update_ms delete_ms total_ms
    insert_ms=$(elapsed_ms "$insert_start" "$insert_end")
    update_ms=$(elapsed_ms "$update_start" "$update_end")
    delete_ms=$(elapsed_ms "$delete_start" "$delete_end")
    total_ms=$(elapsed_ms "$total_start" "$total_end")
    log "RESULT label=$label engine=$engine rows=$PERF_ROWS insert_batch=$PERF_INSERT_BATCH_ROWS update_rows=$PERF_UPDATE_ROWS delete_rows=$PERF_DELETE_ROWS insert_ms=$insert_ms update_ms=$update_ms delete_ms=$delete_ms total_ms=$total_ms final=$final"
}

# Materialize one deterministic TSV input shared by both engines. The repeated
# ASCII values have exact byte lengths, making logical character payload and
# post-load length validation independent of filesystem representation.
generate_bulk_load_file() {
    local fixed_text varchar_a varchar_b
    printf -v fixed_text '%*s' "$BULK_LOAD_CHAR_LENGTH" ''
    printf -v varchar_a '%*s' "$BULK_LOAD_VARCHAR_A_LENGTH" ''
    printf -v varchar_b '%*s' "$BULK_LOAD_VARCHAR_B_LENGTH" ''
    fixed_text=${fixed_text// /C}
    varchar_a=${varchar_a// /A}
    varchar_b=${varchar_b// /B}
    awk -v rows="$BULK_LOAD_ROWS" -v fixed="$fixed_text" \
        -v text_a="$varchar_a" -v text_b="$varchar_b" \
        'BEGIN { OFS="\t"; for (id=1; id<=rows; ++id) print id, id % 1000, fixed, text_a, text_b }' \
        >"$BULK_LOAD_FILE"
}

# LOAD DATA is one MariaDB statement, so the EXASOL handler owns one insert
# operation while write_row() repeatedly flushes vector batches underneath it.
run_bulk_engine() {
    local label=$1
    local schema=$2
    local engine=$3
    local escaped_file=${BULK_LOAD_FILE//\'/\'\'}
    mysql -e "DROP DATABASE IF EXISTS $schema; CREATE DATABASE $schema; USE $schema; CREATE TABLE BULK_T(ID BIGINT, BUCKET INT, FIXED_TEXT CHAR($BULK_LOAD_CHAR_LENGTH), TEXT_A VARCHAR($BULK_LOAD_VARCHAR_A_LENGTH), TEXT_B VARCHAR($BULK_LOAD_VARCHAR_B_LENGTH)) ENGINE=$engine;"

    local load_start load_end load_ms
    load_start=$(now_ns)
    mysql -e "USE $schema; LOAD DATA LOCAL INFILE '$escaped_file' INTO TABLE BULK_T FIELDS TERMINATED BY '\\t' LINES TERMINATED BY '\\n' (ID, BUCKET, FIXED_TEXT, TEXT_A, TEXT_B);"
    load_end=$(now_ns)
    load_ms=$(elapsed_ms "$load_start" "$load_end")
    BULK_LAST_LOAD_MS=$load_ms

    local expected_id_sum expected_bucket_sum complete_blocks remainder
    expected_id_sum=$((BULK_LOAD_ROWS * (BULK_LOAD_ROWS + 1) / 2))
    complete_blocks=$((BULK_LOAD_ROWS / 1000))
    remainder=$((BULK_LOAD_ROWS % 1000))
    expected_bucket_sum=$((complete_blocks * 499500 + remainder * (remainder + 1) / 2))
    local aggregates expected_aggregates
    aggregates=$(mysql --batch --raw --skip-column-names -e "USE $schema; SELECT COUNT(*), SUM(ID), SUM(BUCKET) FROM BULK_T;")
    printf -v expected_aggregates '%s\t%s\t%s' \
        "$BULK_LOAD_ROWS" "$expected_id_sum" "$expected_bucket_sum"
    if [[ "$aggregates" != "$expected_aggregates" ]]; then
        echo "Unexpected $label bulk aggregates: expected $expected_aggregates got $aggregates" >&2
        exit 1
    fi

    # Numeric aggregates are evaluated server-side. Only the first and last
    # strings are transferred for representative width checks, avoiding a 1 GiB
    # payload scan back through MariaDB while still catching truncation.
    local sample_id sample fixed_sample varchar_a_sample varchar_b_sample lengths=
    for sample_id in 1 "$BULK_LOAD_ROWS"; do
        sample=$(mysql --batch --raw --skip-column-names -e "USE $schema; SELECT FIXED_TEXT, TEXT_A, TEXT_B FROM BULK_T WHERE ID=$sample_id;")
        IFS=$'\t' read -r fixed_sample varchar_a_sample varchar_b_sample <<<"$sample"
        if (( ${#fixed_sample} != BULK_LOAD_CHAR_LENGTH ||
              ${#varchar_a_sample} != BULK_LOAD_VARCHAR_A_LENGTH ||
              ${#varchar_b_sample} != BULK_LOAD_VARCHAR_B_LENGTH )); then
            echo "Unexpected $label bulk character lengths at ID $sample_id" >&2
            exit 1
        fi
        lengths+="${lengths:+,}$sample_id:${#fixed_sample}/${#varchar_a_sample}/${#varchar_b_sample}"
    done
    log "BULK_RESULT label=$label engine=$engine rows=$BULK_LOAD_ROWS load_ms=$load_ms aggregates=$(printf '%s' "$aggregates" | tr '\t' '|') representative_lengths=$lengths"
}

# The bulk MariaDB phase forces instrumentation so the report proves that LOAD
# DATA used one remote operation while retaining vector-valued InsertRows calls.
report_bulk_lifecycle() {
    local round=$1
    local first_log_line=$2
    local counters
    # `|| true` is intentional: a missing match must reach the explicit error
    # below rather than terminating during assignment under set -e/pipefail.
    counters=$(tail -n "+$first_log_line" "$MDB/mariadb.err" 2>/dev/null \
        | grep "SessionGW performance:.*insert_rows=$BULK_LOAD_ROWS " \
        | tail -n 1 | sed 's/^.*SessionGW performance: //' || true)
    if [[ -z "$counters" ]]; then
        echo "Missing SessionGateway bulk lifecycle counters for round $round" >&2
        exit 1
    fi

    local expected_batches=$(((BULK_LOAD_ROWS + BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS - 1) /
        BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS))
    local key expected actual
    for key in operations insert_rows insert_batches; do
        case "$key" in
            operations) expected=1 ;;
            insert_rows) expected=$BULK_LOAD_ROWS ;;
            insert_batches) expected=$expected_batches ;;
        esac
        actual=$(awk -v key="$key" '{ for (i=1; i<=NF; ++i) { split($i, pair, "="); if (pair[1] == key) print pair[2] } }' <<<"$counters")
        if [[ "$actual" != "$expected" ]]; then
            echo "Unexpected SessionGateway $key for bulk round $round: expected $expected got ${actual:-missing}" >&2
            exit 1
        fi
    done
    log "BULK_LIFECYCLE round=$round expected_batches=$expected_batches $counters"
}

# Execute EXASOL and assert counters emitted only by this round, preventing a
# stale prior line from satisfying the one-open/many-batches/one-close contract.
run_exasol_bulk_round() {
    local round=$1
    local first_log_line=$(( $(wc -l <"$MDB/mariadb.err") + 1 ))
    run_bulk_engine "exasol_sgw_bulk_round_$round" "BULK_EXASOL" "EXASOL"
    report_bulk_lifecycle "$round" "$first_log_line"
}

mean_milliseconds() {
    local sum=0 value
    for value in "$@"; do
        sum=$((sum + value))
    done
    echo $((sum / $#))
}

log "Engine baseline comparison"
log "base=$BASE_DIR"
log "rows=$PERF_ROWS insert_batch=$PERF_INSERT_BATCH_ROWS update_rows=$PERF_UPDATE_ROWS delete_rows=$PERF_DELETE_ROWS"
log "sessiongw_insert_batch_rows=${EXASOL_SESSIONGW_INSERT_BATCH_ROWS:-10000}"
log "sessiongw_update_batch_rows=${EXASOL_SESSIONGW_UPDATE_BATCH_ROWS:-10000}"
log "sessiongw_delete_batch_rows=${EXASOL_SESSIONGW_DELETE_BATCH_ROWS:-10000}"
log "nano_dbram_mib=$NANO_DBRAM"

if ss -H -ltn "sport = :$EXASOL_PORT" | grep -q .; then
    echo "EXASOL_PORT $EXASOL_PORT is already listening; refusing to reuse an unrelated Nano instance" >&2
    exit 2
fi

"$NANO_RUN" --target "$NANO_APP" --noexec >/dev/null
APPDIR=$(find "$NANO_APP" -maxdepth 1 -type d -name '*.AppDir' | head -n 1)
APPDIR=$(cd "$APPDIR" && pwd -P)
(
    cd "$NANO_BASE"
    exec "$APPDIR/AppRun" --db-files-dir "$NANO_DB" --port "$EXASOL_PORT" -dbram="$NANO_DBRAM"
) >"$NANO_BASE/nano.log" 2>&1 &
NANO_PID=$!
echo "$NANO_PID" >"$NANO_BASE/pid"
nano_ready=0
for _ in $(seq 1 90); do
    if ! kill -0 "$NANO_PID" >/dev/null 2>&1; then
        echo "Nano exited before becoming ready; see $NANO_BASE/nano.log" >&2
        tail -n 50 "$NANO_BASE/nano.log" >&2 || true
        wait "$NANO_PID" >/dev/null 2>&1 || true
        NANO_PID=
        exit 1
    fi
    if sql_exasol 'select 1' >/dev/null 2>&1; then
        nano_ready=1
        break
    fi
    sleep 2
done
if (( nano_ready == 0 )); then
    echo "Nano did not become ready on port $EXASOL_PORT" >&2
    exit 1
fi
log "PASS nano ready"

"$MARIADB_BUILD/scripts/mariadb-install-db" --force --no-defaults \
    --srcdir="$MARIADB_SRC" --builddir="$MARIADB_BUILD" --datadir="$MDB/data" \
    --auth-root-authentication-method=normal >"$MDB/install.log" 2>&1

start_mariadb "${EXASOL_SESSIONGW_INSERT_BATCH_ROWS:-10000}" "${EXASOL_SESSIONGW_INSTRUMENTATION:-0}"
log "PASS mariadb ready"

run_engine "innodb" "CMP_INNODB" "InnoDB"
run_engine "exasol_sgw" "CMP_EXASOL" "EXASOL"

# Reopen MariaDB with a wide-row-safe SessionGateway ceiling without changing
# the historical narrow-row baseline configuration above.
stop_mariadb
start_mariadb "$BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS" 1
log "PASS mariadb bulk ready"

bulk_logical_character_bytes=$((BULK_LOAD_ROWS * (BULK_LOAD_CHAR_LENGTH + BULK_LOAD_VARCHAR_A_LENGTH + BULK_LOAD_VARCHAR_B_LENGTH)))
if (( bulk_logical_character_bytes < BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES )); then
    echo "Bulk logical character payload $bulk_logical_character_bytes is below required minimum $BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES" >&2
    exit 2
fi
generate_bulk_load_file
bulk_input_file_bytes=$(stat -c %s "$BULK_LOAD_FILE")
log "BULK_CONFIG rows=$BULK_LOAD_ROWS repetitions=$BULK_LOAD_REPETITIONS char_length=$BULK_LOAD_CHAR_LENGTH varchar_a_length=$BULK_LOAD_VARCHAR_A_LENGTH varchar_b_length=$BULK_LOAD_VARCHAR_B_LENGTH logical_character_bytes=$bulk_logical_character_bytes minimum_logical_character_bytes=$BULK_LOAD_MIN_LOGICAL_CHARACTER_BYTES input_file_bytes=$bulk_input_file_bytes sessiongw_insert_batch_rows=$BULK_LOAD_SESSIONGW_INSERT_BATCH_ROWS native_frame_upper_bound_bytes=$BULK_NATIVE_FRAME_UPPER_BOUND_BYTES sessiongw_payload_limit_bytes=$SESSIONGW_MAX_PAYLOAD_BYTES"

declare -a innodb_bulk_times=()
declare -a exasol_bulk_times=()
for ((round=1; round<=BULK_LOAD_REPETITIONS; ++round)); do
    if (( round % 2 == 1 )); then
        log "BULK_ROUND round=$round order=InnoDB,EXASOL"
        run_bulk_engine "innodb_bulk_round_$round" "BULK_INNODB" "InnoDB"
        innodb_bulk_times+=("$BULK_LAST_LOAD_MS")
        run_exasol_bulk_round "$round"
        exasol_bulk_times+=("$BULK_LAST_LOAD_MS")
    else
        log "BULK_ROUND round=$round order=EXASOL,InnoDB"
        run_exasol_bulk_round "$round"
        exasol_bulk_times+=("$BULK_LAST_LOAD_MS")
        run_bulk_engine "innodb_bulk_round_$round" "BULK_INNODB" "InnoDB"
        innodb_bulk_times+=("$BULK_LAST_LOAD_MS")
    fi
done
innodb_mean_ms=$(mean_milliseconds "${innodb_bulk_times[@]}")
exasol_mean_ms=$(mean_milliseconds "${exasol_bulk_times[@]}")
innodb_rounds_ms=$(IFS=,; echo "${innodb_bulk_times[*]}")
exasol_rounds_ms=$(IFS=,; echo "${exasol_bulk_times[*]}")
log "BULK_AGGREGATE engine=InnoDB repetitions=$BULK_LOAD_REPETITIONS mean_load_ms=$innodb_mean_ms rounds_ms=$innodb_rounds_ms"
log "BULK_AGGREGATE engine=EXASOL repetitions=$BULK_LOAD_REPETITIONS mean_load_ms=$exasol_mean_ms rounds_ms=$exasol_rounds_ms"

log "Comparison complete; report=$REPORT"
