# Exasol Gateway storage engine

The canonical `ENGINE=EXASOL` plugin is maintained out of tree at:

<https://github.com/exasol-labs/mariadb-exasol-gateway>

That repository injects its `plugin/` directory as `storage/exasol_gw` into a
clean supported MariaDB source checkout and consumes the separately released
Exasol Gateway SDK through its stable C ABI. This MariaDB fork is retained only
for integration/upstream work and does not carry a duplicate canonical plugin
copy.
