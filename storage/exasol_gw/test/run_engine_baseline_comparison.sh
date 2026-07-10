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
PERF_ROWS=${PERF_ROWS:-100000}
PERF_INSERT_BATCH_ROWS=${PERF_INSERT_BATCH_ROWS:-10000}
PERF_UPDATE_ROWS=${PERF_UPDATE_ROWS:-$((PERF_ROWS / 10))}
PERF_DELETE_ROWS=${PERF_DELETE_ROWS:-$((PERF_ROWS / 20))}
if (( PERF_UPDATE_ROWS == 0 )); then PERF_UPDATE_ROWS=1; fi
if (( PERF_DELETE_ROWS == 0 )); then PERF_DELETE_ROWS=1; fi

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
        kill "$(cat "$NANO_BASE/pid")" >/dev/null 2>&1 || true
    fi
}
trap cleanup EXIT

log() { printf '%s\n' "$*" | tee -a "$REPORT"; }

sql_exasol() {
    (cd "$DB_EXANANO" && c4 sqlclient --usetls --skiptlsverify --user sys --password exasol \
        --connection localhost:$EXASOL_PORT --query "$1")
}

mysql() {
    "$MARIADB_BUILD/client/mariadb" --no-defaults --local-infile=1 --socket="$SOCKET" "$@"
}

now_ns() { date +%s%N; }
elapsed_ms() { echo $(( ($2 - $1) / 1000000 )); }

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

log "Engine baseline comparison"
log "base=$BASE_DIR"
log "rows=$PERF_ROWS insert_batch=$PERF_INSERT_BATCH_ROWS update_rows=$PERF_UPDATE_ROWS delete_rows=$PERF_DELETE_ROWS"
log "sessiongw_insert_batch_rows=${EXASOL_SESSIONGW_INSERT_BATCH_ROWS:-10000}"
log "sessiongw_update_batch_rows=${EXASOL_SESSIONGW_UPDATE_BATCH_ROWS:-10000}"
log "sessiongw_delete_batch_rows=${EXASOL_SESSIONGW_DELETE_BATCH_ROWS:-10000}"

"$NANO_RUN" --target "$NANO_APP" --noexec >/dev/null
APPDIR=$(find "$NANO_APP" -maxdepth 1 -type d -name '*.AppDir' | head -n 1)
("$APPDIR/AppRun" --db-files-dir "$NANO_DB" --port "$EXASOL_PORT" >"$NANO_BASE/nano.log" 2>&1 & echo $! > "$NANO_BASE/pid")
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

run_engine "innodb" "CMP_INNODB" "InnoDB"
run_engine "exasol_sgw" "CMP_EXASOL" "EXASOL"

log "Comparison complete; report=$REPORT"
