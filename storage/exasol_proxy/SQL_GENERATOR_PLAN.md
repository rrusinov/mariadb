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

## Non-transformer Path Audit

Current generator structure is intentionally mixed:

- **Transformer-driven expression paths**
  - scalar `Item_func` dispatch via `function_transforms` and `function_name_transforms`
  - aggregate `Item_sum` dispatch via `aggregate_transforms`
  - window-function call dispatch via `window_transforms`
- **Procedural statement / structural paths**
  - `SELECT` / compound `SELECT` assembly
  - projection list iteration
  - table refs, derived tables, join flattening, right-join normalization, NATURAL/USING joins
  - `WHERE`, `GROUP BY`, `HAVING`, `ORDER BY`, `LIMIT/OFFSET`
  - subquery wrapping and set-operation assembly
  - window specification / frame / bound rendering
  - identifier, constant, and alias-reference rendering

This split is **intentional**. The transform tables are the allowlist for scalar/aggregate/window
expression operators, while statement-shape emission remains procedural because it depends on
MariaDB-specific tree normalization (`TABLE_LIST`, `next_local`, join representatives, `SELECT_LEX`
linkage, window-spec inheritance, etc.).

Audit conclusion:

- there is **no remaining broad SQL-printer fallback** for unsupported AST nodes
- scalar/operator coverage should continue to grow through the transform tables
- structural paths should stay procedural unless a concrete repetition or fail-open risk appears
- stale duplicate helper paths are undesirable because they can preserve obsolete unsupported
  messages after the main dispatch path has moved on

Concrete follow-up from this audit:

- removed the obsolete unused `emit_window_function(Item_window_func *)` helper, which still had
  the old `named window references are not implemented yet` failure path after named-window support
  moved into `dispatch_window_function()`
- keep future work focused on real gaps such as `GROUPING()` rather than mechanically migrating
  join / `SELECT` assembly into transform tables

## Semantic Decisions

- `ORDER BY` NULL ordering: generated EXASOL SQL intentionally follows Exasol defaults
  (`NULLS LAST`) instead of injecting MariaDB-style `ASC NULLS FIRST`. This preserves the existing
  proxy behavior and avoids unsafe rewriting of positional `ORDER BY` terms. Queries that require
  MariaDB-compatible ascending NULL placement can express it explicitly with sort keys such as
  `expr IS NULL, expr`.
- `RIGHT JOIN`: MariaDB normalizes simple right joins internally by swapping the inputs and
  carrying the original `ON` expression on the nullable side. The generator emits these simple
  two-table shapes as equivalent `LEFT JOIN` SQL and fails closed for more complex right-join
  arrangements.
- `FULL OUTER JOIN`: raw syntax is not handled by the generator because MariaDB does not expose it
  as a supported AST shape. TPC-DS q51/q97-style cases are covered by explicit
  matched-rows plus anti-semi `UNION ALL` rewrites that use generator-supported set operation,
  comma join, and `NOT EXISTS` nodes.
- `GROUPING()`: raw function syntax is not handled by the generator. TPC-DS q27/q36/q70/q86-style
  cases are covered by explicit `WITH ROLLUP` plus `CASE ... IS NULL` marker rewrites for compact
  fixtures whose hierarchy keys are non-null.
