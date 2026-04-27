# MariaDB AST to Exasol SQL Generator Plan

## Goal

Replace the current `SELECT_LEX::print` plus string-rewrite pushdown path with an allowlisted
MariaDB AST to Exasol SQL generator.

The generator must fail closed. Unsupported MariaDB AST nodes return an explicit unsupported reason
instead of falling back to MariaDB `SELECT_LEX::print` SQL rendering.

## SQLGlot Reference

The checked source at `/tmp/sqlglot` commit `cf0ffd9` is useful as a design reference, not as a
runtime dependency. The relevant patterns are:

- dialect-specific generator class
- explicit type mapping table
- explicit function/operator transform table
- dialect NULL-ordering model
- preprocess steps for dialect restrictions such as Exasol's `LOCAL` alias behavior

The SQLGlot checkout is MIT licensed, but copied code should still be reviewed explicitly before
being imported.

## Implementation Slices

1. Add an isolated generator skeleton.
   It exposes `generate_exasol_sql(THD *, st_select_lex_unit *)` and
   `generate_exasol_sql(THD *, st_select_lex *)`, but it is not wired into runtime pushdown yet.

2. Implement core `SELECT_LEX` emission.
   Cover projection, base table references, aliases, simple joins, `WHERE`, `GROUP BY`, `HAVING`,
   `ORDER BY`, `LIMIT`, `OFFSET`, and simple `UNION` shapes already accepted by proxy checks.

3. Implement expression emission.
   Add allowlisted `Item`, `Item_func`, and `Item_sum` handlers for comparisons, boolean logic,
   NULL predicates, arithmetic, casts, common scalar functions, and aggregates.

4. Switch supported query shapes to the AST generator.
   The proxy pushdown path uses the AST generator unconditionally. Unsupported shapes fail closed
   with an explicit error; no legacy printer/rewrite fallback remains.

5. Add differential tests.
   Compare MariaDB-native execution with EXASOL proxy pushdown for quoting, NULL ordering,
   LIMIT/OFFSET, aggregates, DISTINCT staging, derived tables, UNION, numeric/temporal/string
   expressions, and unsupported-shape errors.

6. Add SQLGlot reference tests.
   Use a local SQLGlot checkout as an optional offline oracle for renderer decisions where SQLGlot
   and the MariaDB AST generator overlap. This must stay out of the proxy runtime path.
   Implemented by `Server/test/mariadb/test_mariadb_proxy_sqlglot_reference`; set
   `SQLGLOT_SRC=/path/to/sqlglot` or let the script skip when no checkout is present.

## Runtime Boundary

Direct row/iterator paths must continue to use the storage-engine/core readable or write transport.
The SQL generator is only for higher-level pushdown logic where MariaDB asks the storage engine to
execute a logical query.

## Semantic Decisions

- `ORDER BY` NULL ordering: generated EXASOL SQL intentionally follows Exasol defaults
  (`NULLS LAST`) instead of injecting MariaDB-style `ASC NULLS FIRST`. This preserves the existing
  proxy behavior and avoids unsafe rewriting of positional `ORDER BY` terms. Queries that require
  MariaDB-compatible ascending NULL placement can express it explicitly with sort keys such as
  `expr IS NULL, expr`.
