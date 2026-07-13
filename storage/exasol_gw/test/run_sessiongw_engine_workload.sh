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
    pkill -f "$BASE_DIR" >/dev/null 2>&1 || true
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

expect_exasol_failure() {
    local label=$1
    local sql=$2
    if sql_exasol "$sql" >"$BASE_DIR/${label//[^A-Za-z0-9_]/_}.out" 2>&1; then
        echo "Expected Exasol failure for $label but command succeeded" >&2
        exit 1
    fi
    log "PASS expected Exasol failure: $label"
}

log "SessionGW MariaDB engine workload"
log "base=$BASE_DIR"
log "mariadb_build=$MARIADB_BUILD"
log "nano_port=$EXASOL_PORT"
log "perf_rows=$PERF_ROWS"
log "perf_insert_batch_rows=$PERF_INSERT_BATCH_ROWS"
log "perf_update_rows=$PERF_UPDATE_ROWS"
log "perf_delete_rows=$PERF_DELETE_ROWS"
log "insert_rounds=$INSERT_ROUNDS"
log "insert_rows_per_client=$INSERT_ROWS_PER_CLIENT"
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
EXASOL_SESSIONGW_INSTRUMENTATION=${EXASOL_SESSIONGW_INSTRUMENTATION:-1} \
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
log "PASS show engines"

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
    expect_failure_contains "cursor constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_cursor_constructor_oom'; USE $SCHEMA; SELECT * FROM T" \
        "out of memory"
    expect_failure_contains "insert context constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_insert_context_constructor_oom'; USE $SCHEMA; INSERT INTO T VALUES (10, 'fault')" \
        "out of memory"
    expect_failure_contains "update context constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_update_context_constructor_oom'; USE $SCHEMA; UPDATE T SET NAME='fault' WHERE ID=1" \
        "out of memory"
    expect_failure_contains "delete context constructor allocation fault" \
        "SET SESSION debug_dbug='+d,exasol_gw_delete_context_constructor_oom'; USE $SCHEMA; DELETE FROM T WHERE ID=1" \
        "out of memory"
    expect_scalar "constructor faults preserve server and rows" \
        "USE $SCHEMA; SELECT CONCAT(COUNT(*), '|', MIN(NAME)) FROM T" "3|Alice"
else
    log "SKIP constructor fault injection (MariaDB build has DBUG disabled)"
fi

if [[ "$FAULT_INJECTION_ONLY" == "1" ]]; then
    log "SessionGW MariaDB constructor fault-injection workload passed"
    exit 0
fi

mysql --table <<SQL | tee -a "$REPORT"
USE $SCHEMA;
CREATE TABLE POS_T(ID INT, NAME VARCHAR(40)) ENGINE=EXASOL;
INSERT INTO POS_T VALUES (1, 'One'), (2, 'Two'), (3, 'Three'), (4, 'Four');
CREATE TABLE POS_KEYS(ID INT) ENGINE=InnoDB;
INSERT INTO POS_KEYS VALUES (2), (3);
UPDATE POS_T AS p JOIN POS_KEYS AS k ON p.ID=k.ID SET p.NAME='Matched' WHERE k.ID=2;
DELETE p FROM POS_T AS p JOIN POS_KEYS AS k ON p.ID=k.ID WHERE k.ID=3;
SELECT * FROM POS_T ORDER BY ID;
SQL
expect_scalar "positioned update/delete final" \
    "USE $SCHEMA; SELECT CONCAT(COUNT(*), '|', SUM(CASE WHEN ID=2 AND NAME='Matched' THEN 1 ELSE 0 END), '|', SUM(CASE WHEN ID=3 THEN 1 ELSE 0 END)) FROM POS_T" \
    "3|1|0"
log "PASS positioned update delete"

expect_scalar "prepared statement count" \
    "USE $SCHEMA; PREPARE s FROM 'SELECT COUNT(*) FROM T WHERE ID > ?'; SET @p=1; EXECUTE s USING @p; DEALLOCATE PREPARE s;" \
    "2"

expect_failure "unsupported key definition" \
    "USE $SCHEMA; CREATE TABLE BAD_KEY(ID INT, KEY(ID)) ENGINE=EXASOL"
expect_failure "unsupported default clause" \
    "USE $SCHEMA; CREATE TABLE BAD_DEFAULT(ID INT DEFAULT 1) ENGINE=EXASOL"
expect_failure "unsupported auto increment" \
    "USE $SCHEMA; CREATE TABLE BAD_AUTO(ID INT AUTO_INCREMENT PRIMARY KEY) ENGINE=EXASOL"
expect_failure "unsupported binary string" \
    "USE $SCHEMA; CREATE TABLE BAD_BINARY(B BINARY(8)) ENGINE=EXASOL"
expect_failure "unsupported multi-bit value" \
    "USE $SCHEMA; CREATE TABLE BAD_BIT(B BIT(2)) ENGINE=EXASOL"
expect_failure "unsupported explicit table charset" \
    "USE $SCHEMA; CREATE TABLE BAD_CHARSET(V VARCHAR(8)) ENGINE=EXASOL DEFAULT CHARSET=latin1"
expect_failure "unsupported alter table" \
    "USE $SCHEMA; ALTER TABLE T ADD COLUMN EXTRA INT"
expect_failure "unsupported truncate table" \
    "USE $SCHEMA; TRUNCATE TABLE T"
expect_failure "unsupported rename table" \
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
CREATE TABLE TYPE_UPD(ID INT, D DECIMAL(12,2), DT DATE, TS TIMESTAMP NULL, V VARCHAR(80)) ENGINE=EXASOL;
INSERT INTO TYPE_UPD VALUES (1, 1.25, '2026-07-10', '2026-07-10 11:12:13', 'before');
UPDATE TYPE_UPD SET D=42.42, DT='2026-07-11', TS='2026-07-11 01:02:03', V='updated' WHERE ID=1;
SELECT ID, D, DT, TS, V FROM TYPE_UPD ORDER BY ID;
SQL
expect_scalar "type matrix final count" "USE $SCHEMA; SELECT COUNT(*) FROM TYPES" "1"
TYPE_UPD_FINAL=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT ID FROM TYPE_UPD; SELECT D FROM TYPE_UPD; SELECT DT FROM TYPE_UPD; SELECT TS FROM TYPE_UPD; SELECT V FROM TYPE_UPD;" | paste -sd'|' -)
if [[ "$TYPE_UPD_FINAL" != "1|42.42|2026-07-11|2026-07-11 01:02:03|updated" ]]; then
    echo "Unexpected type update final state: $TYPE_UPD_FINAL" >&2
    exit 1
fi
log "PASS type update final $TYPE_UPD_FINAL"
log "PASS supported type matrix"

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

# Concurrent insert statements either commit completely or propagate a MariaDB
# deadlock error for Exasol transaction conflicts. The adapter must not replay.
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
                -e "USE $SCHEMA; INSERT INTO T VALUES $values;" 2>"$error_file"; then
                echo "committed $first_id $last_id" >"$result_file"
            elif grep -q "ERROR 1213" "$error_file"; then
                echo "conflict $first_id $last_id" >"$result_file"
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

EXPECTED_COUNT=3
EXPECTED_SUM=7
EXPECTED_NAMES=0
COMMITTED_WRITERS=0
CONFLICTED_WRITERS=0
for result_file in "$BASE_DIR"/insert_*.result; do
    read -r outcome first_id last_id <"$result_file"
    if [[ "$outcome" == "committed" ]]; then
        rows=$((last_id - first_id + 1))
        EXPECTED_COUNT=$((EXPECTED_COUNT + rows))
        EXPECTED_SUM=$((EXPECTED_SUM + (first_id + last_id) * rows / 2))
        EXPECTED_NAMES=$((EXPECTED_NAMES + rows))
        COMMITTED_WRITERS=$((COMMITTED_WRITERS + 1))
    else
        CONFLICTED_WRITERS=$((CONFLICTED_WRITERS + 1))
    fi
done
log "PASS concurrent inserts clients=$INSERT_CLIENTS rounds=$INSERT_ROUNDS rows_per_client=$INSERT_ROWS_PER_CLIENT committed=$COMMITTED_WRITERS conflicts=$CONFLICTED_WRITERS"

FINAL_ROWS=$(mysql --batch --raw --skip-column-names -e "USE $SCHEMA; SELECT COUNT(*) FROM T; SELECT SUM(ID) FROM T; SELECT COUNT(*) FROM T WHERE NAME LIKE 'Name_%';")
FINAL=$(echo "$FINAL_ROWS" | paste -sd'|' -)
EXPECTED="$EXPECTED_COUNT|$EXPECTED_SUM|$EXPECTED_NAMES"
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
