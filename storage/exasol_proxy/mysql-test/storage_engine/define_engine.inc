let $ENGINE = EXASOL;

# EXASOL ENGINE=EXASOL tables currently do not support MariaDB key definitions
# in this branch, so the generic storage_engine suite should not assume indexes.
let $default_index = /*!*/;
