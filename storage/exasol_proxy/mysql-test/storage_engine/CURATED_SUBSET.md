# Curated upstream `storage_engine` subset for `EXASOL`

This directory provides the first EXASOL-specific overlay for MariaDB's generic
`mysql-test/suite/storage_engine` suite.

## Runnable overlay tests

These tests were validated against the joint controller-managed MariaDB+EXASOL
product using `mysql-test-run.pl --extern`:

- `storage_engine-exasol_proxy.show_engine`
- `storage_engine-exasol_proxy.1st`
- `storage_engine-exasol_proxy.insert`
- `storage_engine-exasol_proxy.update`

The overlay is activated by running the suite as `storage_engine-exasol_proxy`.
Invoking plain `storage_engine.<test>` does **not** pick up EXASOL-specific
`define_engine.inc` or `.rdiff` files from this directory.

## Why this subset is small

The generic `storage_engine` suite contains many tests that are either outside
current EXASOL adapter scope or need EXASOL-specific expectation overrides.
The current curated subset keeps only tests that are already high-confidence and
runnable end-to-end without broadening scope.

## Explicitly excluded for now

- `describe`
  - expectation mismatch only: EXASOL-backed `INT` columns currently surface as
    `decimal(18,0)` in MariaDB metadata/discovery
- `show_table_status`
  - expectation mismatch only: collation is reported as
    `utf8mb4_uca1400_ai_ci` rather than the generic suite's default collation
- `select`
  - blocked by `mysqltest` `--extern` path rules for `INTO OUTFILE` / `DUMPFILE`
    because the live server datadir is outside `MYSQLTEST_VARDIR`
- `replace`
  - depends on `UNIQUE` / `PRIMARY KEY` semantics that are intentionally
    unsupported for `ENGINE=EXASOL` in this branch
- `delete`
  - currently exposes a likely real bridge bug in multi-table `DELETE`; tracked
    separately as `td-0691c1`
- index / handler / repair / optimize / truncate / XA / lock-heavy tests
  - outside current supported adapter semantics, already known unsupported, or
    not yet justified for this minimal upstream subset

## Transactional sub-suite note

The generic `storage_engine/trx/*` tests are good future candidates, but they
are not yet part of this first curated subset. If we want to run them through
this overlay suite directly, we will likely need small EXASOL-specific wrapper
entries or a dedicated harness invocation strategy.
