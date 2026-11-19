# ORM mapping

A rule package can map its deftemplates to database tables and name the database sequences its rules use (S7, S11, FR-22). The mapping lives in a separate file named by the manifest key `orm` ([`packages.md`](packages.md#manifest)). Nothing in it is implicit: every slot of a mapped template gets its own line with the column, the SQL type, the CLIPS type and what a NULL becomes.

The mapped templates of one workflow definition form its aggregate (FR-22); their tables carry the persistent state of each workflow instance. This page describes the file, the checks made when the package loads (T9a, T9b) and the [DDL functions](#ddl-functions) `db-create-table`, `db-check-schema` and `db-drop-table` (T9b, FR-23). `db-nextval` (T9c) and the per-cycle synchronization (T10) build on this model.

## File format

The mapping file uses the same `key = value` format as the daemon configuration ([`configuration.md`](configuration.md#file-format)): one pair per line, `#` comments, LF or CRLF, spaces and tabs around keys and values trimmed, each key at most once.

```ini
# /etc/clpdaemon/packages/shop@v1/orm.conf  (package.conf: orm = orm.conf)
order.table          = shop_order
order.owned          = yes
order.key            = line-no
order.slot.wf-id     = wf_id      text        string  error
order.slot.line-no   = line_no    bigint      integer error
order.slot.status    = status     text        symbol  default
order.slot.amount    = amount     numeric     float   nil
order.slot.placed-at = placed_at  timestamptz string  nil

# an existing table owned by another system (D21): never created or dropped
customer.table       = crm_customer
customer.owned       = no
customer.key         =
customer.slot.wf-id  = wf_id      text        string  error
customer.slot.name   = full_name  text        string  default

seq.order_no         = shop_order_no_seq
seq.order_no.owned   = yes
```

### Template keys

`<tpl>` is the name of a deftemplate of the package (not an implied one). The template name ends at the first `.` of the key, so a deftemplate whose name contains `.` cannot be mapped; a template named `seq` cannot be mapped either (the `seq.` prefix is reserved).

| Key | Value | Default |
|---|---|---|
| `<tpl>.table` | table name, an [identifier](#identifiers) | required |
| `<tpl>.owned` | `yes`: the table belongs to the daemon (`db-create-table` may create it, `db-drop-table` may drop it when `orm_allow_drop = yes`); `no`: an existing table, never created nor dropped | required |
| `<tpl>.key` | comma-separated list of zero or more slots, the local key in order; `wf-id` is always the first key column and is not listed | empty (the key is `wf-id` alone) |
| `<tpl>.slot.<slot>` | `<column> <sql-type> <clips-type> <null>`, four fields separated by spaces or tabs | one line required for every slot |

The primary key of a table is `wf-id` followed by the local key (FR-22). Every slot of the deftemplate, `wf-id` included, needs its `<tpl>.slot.<slot>` line; key slots (`wf-id` and the slots in `<tpl>.key`) must use the null policy `error`. Two slots of one template cannot use the same column, and two templates of one file cannot use the same table. Multislots cannot be mapped.

### Sequences

| Key | Value | Default |
|---|---|---|
| `seq.<alias>` | name of the database sequence, an [identifier](#identifiers); `<alias>` is the name rules use (`(db-nextval order_no)`, T9c) and is an identifier too | required |
| `seq.<alias>.owned` | `yes`: the daemon may create the sequence; `no`: an existing sequence | required |

Two aliases of one file cannot name the same sequence.

### Identifiers

Table, column, sequence names and sequence aliases match `[a-z_][a-z0-9_]*` and have at most 63 bytes (PostgreSQL's limit). They are the only names that ever reach SQL (FR-24), always quoted by the DDL and DML functions.

### SQL types

The SQL type is logical; each backend has its own column type, used by `db-create-table`. SQLite only stands in for PostgreSQL in tests, and its names never get NUMERIC affinity, so values come back as stored. The last two columns are the existing column types `db-check-schema` accepts as [compatible](#db-check-schema).

| `sql-type` | PostgreSQL | SQLite | Holds `clips-type` | Compatible on PostgreSQL (`udt_name`) | Compatible on SQLite (affinity) |
|---|---|---|---|---|---|
| `bigint` | `BIGINT` | `INTEGER` | `integer` | `int8`, `int4`, `int2` | INTEGER |
| `double` | `DOUBLE PRECISION` | `REAL` | `float` | `float8`, `float4` | REAL |
| `text` | `TEXT` | `TEXT` | `string`, `symbol` | `text`, `varchar` | TEXT |
| `bytea` | `BYTEA` | `BLOB` | `string` | `bytea` | BLOB |
| `boolean` | `BOOLEAN` | `INTEGER` | `integer` | `bool` | INTEGER |
| `timestamptz` | `TIMESTAMPTZ` | `TEXT` | `string`, `symbol` | `timestamptz` | TEXT |
| `jsonb` | `JSONB` | `TEXT` | `string`, `symbol` | `jsonb` | TEXT |
| `numeric` | `NUMERIC` | `TEXT` | `float`, `string`, `symbol` | `numeric` | TEXT |

### CLIPS types

`clips-type` is the type the slot value has in the fact: `integer`, `float`, `string` or `symbol`. The SQL type must hold it (column *Holds `clips-type`* above: `bigint` cannot be mapped as `string`). The slot's own type constraint must allow it too (`(slot n (type INTEGER))` cannot be mapped as `string`; a slot without a `type` attribute allows all four; `LEXEME` allows `string` and `symbol`, `NUMBER` allows `integer` and `float`).

### Null policies

What a NULL column becomes when a row is loaded into a fact (T10):

| `null` | Effect |
|---|---|
| `nil` | the slot gets the symbol `nil`, so the slot's type constraint must allow SYMBOL |
| `default` | the slot gets its deftemplate default |
| `error` | the row is rejected; required for key slots |

## Loading

The mapping is read after the package's files, the `wf-id` lint and the `ingest` check ([`packages.md`](packages.md#loading)), at startup and under `--check`. A package with `orm` needs a database: without the `database` key in the configuration ([`database.md`](database.md)) it is an error. The checks are:

1. The file itself: syntax, unknown or repeated keys, values, required keys, null policy of key slots, a column, table or sequence used twice in the file.
2. A configured `database`.
3. Each mapped template, in file order: it is a deftemplate of the package; it has a `wf-id` slot; each slot line, in file order, has an SQL type that holds its CLIPS type, names a slot of the template, and that slot's type constraint allows the declared CLIPS type and, with null policy `nil`, SYMBOL; each key slot is a slot of the template; every slot of the template is mapped and none is a multislot.
4. Tables, then sequences, already mapped by an earlier package of another definition. Versions of one definition (`shop@1` and `shop@2`) may map the same tables and sequences; two definitions may not. A package whose mapping file is valid claims its tables and sequences even if a later check failed. The mapping is not read when a file of the package fails to load (step 2 of [`packages.md`](packages.md#loading)).
5. Names shared by a sequence and a table (PostgreSQL keeps both in one namespace): each sequence of the file against the tables of earlier packages (any definition) and of the file itself, then each table of the file against the sequences of earlier packages.
6. Each table of the file that an earlier version of the same definition maps: both must map it identically, compared with the first such version. Identical means the same `owned`, the same primary key columns in order, and the same columns (in any order) with the same SQL type and the same NOT NULL (null policy `error` or not). Slot names, CLIPS types and `nil` versus `default` are not part of the table and may differ.

Any error makes the daemon exit 1, like other package errors. Nothing is read from or written to the database at load.

## Messages

| Situation | Message |
|---|---|
| Mapping file cannot be opened or read | `clpdaemon: <file>: cannot open: <system error>` (or `cannot read`) |
| Syntax, NUL byte | same messages as the daemon configuration ([`configuration.md`](configuration.md#messages)) |
| Key of no known form | `clpdaemon: <file>:<line>: unknown key '<key>'` |
| Repeated key | `clpdaemon: <file>:<line>: duplicate key '<key>' (first set on line <n>)` |
| Bad table or sequence name | `clpdaemon: <file>:<line>: <key>: expected an identifier matching [a-z_][a-z0-9_]* (at most 63 bytes), got '<value>'` |
| Bad column name | `clpdaemon: <file>:<line>: <key>: column: expected an identifier matching [a-z_][a-z0-9_]* (at most 63 bytes), got '<column>'` |
| Bad sequence alias | `clpdaemon: <file>:<line>: <key>: sequence alias: expected an identifier matching [a-z_][a-z0-9_]* (at most 63 bytes), got '<alias>'` |
| Bad `owned` | `clpdaemon: <file>:<line>: <key>: expected yes or no, got '<value>'` |
| Slot line without four fields | `clpdaemon: <file>:<line>: <key>: expected '<column> <sql-type> <clips-type> <null>', got '<value>'` |
| Unknown SQL type | `clpdaemon: <file>:<line>: <key>: unknown sql type '<type>' (expected bigint, double, text, bytea, boolean, timestamptz, jsonb, numeric)` |
| Unknown CLIPS type | `clpdaemon: <file>:<line>: <key>: unknown clips type '<type>' (expected integer, float, string, symbol)` |
| Unknown null policy | `clpdaemon: <file>:<line>: <key>: unknown null policy '<policy>' (expected nil, default, error)` |
| `wf-id` in `<tpl>.key` | `clpdaemon: <file>:<line>: <tpl>.key: 'wf-id' is always the first key slot and must not be listed` |
| Slot repeated in `<tpl>.key` | `clpdaemon: <file>:<line>: <tpl>.key: '<slot>' listed more than once` |
| Empty item in `<tpl>.key` | `clpdaemon: <file>:<line>: <tpl>.key: empty list item` |
| Column used by two slots | `clpdaemon: <file>:<line>: <key>: column '<column>' is already mapped by slot '<slot>' on line <n>` |
| Table used by two templates | `clpdaemon: <file>:<line>: <key>: table '<table>' is already mapped by template '<tpl>' on line <n>` |
| Sequence used by two aliases | `clpdaemon: <file>:<line>: <key>: sequence '<name>' is already mapped by alias '<alias>' on line <n>` |
| Required key absent | `clpdaemon: <file>: missing required key '<key>'` (`<tpl>.table`, `<tpl>.owned`, `seq.<alias>`, `seq.<alias>.owned`) |
| Key slot whose null policy is not `error` | `clpdaemon: <file>:<line>: <tpl>.slot.<slot>: a key slot needs null policy error, got '<policy>'` |
| No `database` in the configuration | `clpdaemon: <pkg>: orm: a mapping needs a database (the configuration sets no database)` |
| Template that is no deftemplate of the package (or an implied one) | `clpdaemon: <pkg>: orm: unknown template '<tpl>'` |
| Template without `wf-id` | `clpdaemon: <pkg>: orm: template '<tpl>' has no wf-id slot` |
| Slot line or key slot naming no slot of the template | `clpdaemon: <pkg>: orm: template '<tpl>' has no slot '<slot>'` |
| CLIPS type the slot does not allow | `clpdaemon: <pkg>: orm: template '<tpl>': slot '<slot>' is mapped as <clips-type>, which its type constraint does not allow` |
| Multislot | `clpdaemon: <pkg>: orm: template '<tpl>': slot '<slot>' is a multislot, which cannot be mapped` |
| Slot without a line | `clpdaemon: <pkg>: orm: template '<tpl>': slot '<slot>' is not mapped` |
| CLIPS type the SQL type cannot hold | `clpdaemon: <pkg>: orm: template '<tpl>': slot '<slot>': sql type <sql-type> cannot hold clips type <clips-type> (expected <types>)`, `<types>` one of `integer`, `float`, `string`, `string or symbol`, `float, string or symbol` |
| Null policy `nil` on a slot that does not allow SYMBOL | `clpdaemon: <pkg>: orm: template '<tpl>': slot '<slot>' has null policy nil, but its type constraint does not allow SYMBOL` |
| Table of another definition | `clpdaemon: <pkg>: orm: table '<table>' is already mapped by <other pkg>` |
| Sequence of another definition | `clpdaemon: <pkg>: orm: sequence '<name>' is already mapped by <other pkg>` |
| Sequence with the name of a mapped table | `clpdaemon: <pkg>: orm: sequence '<name>' has the name of a table mapped by <pkg or other pkg>` |
| Table with the name of a mapped sequence | `clpdaemon: <pkg>: orm: table '<name>' has the name of a sequence mapped by <other pkg>` |
| Table an earlier version maps differently | `clpdaemon: <pkg>: orm: table '<table>' is mapped differently by <other version> (<difference>)`, `<difference>` one of `owned`, `key`, `column '<column>'` (the first column of the earlier version that is missing or differs, then the first column only this version maps) |

`<file>` is the mapping path as the manifest resolves it (`<package dir>/<orm>`); `<pkg>` is `definition@version`. Order: the file's line errors in file order, then its file-level errors (per template in order of first appearance: missing `table`, missing `owned`, key slots with another null policy in line order; then per sequence: missing name, missing `owned`), then the database message, then the template checks of step 3 (per slot line: SQL type, unknown slot, type constraint, `nil`) and the checks of steps 4 to 6. When the file has errors, steps 3 to 6 are skipped.

## DDL functions

With a `database` in the configuration the core defines three CLIPS functions (FR-23, D9) before the plugins and packages load; without one they do not exist, so a package calling them fails to load (`Missing function declaration for 'db-create-table'`). Each call is one autocommit statement of its own through the database interface ([`database.md`](database.md)): the functions never send `BEGIN` or `COMMIT` and never `ALTER` a table. The SQL holds only the table, column and type names of the mapping, always quoted (`"shop_order"`); the catalog lookups pass the table name as a parameter (`= $1`).

| Function | Returns | Effect |
|---|---|---|
| `(db-create-table <template>)` | `TRUE`, or `FALSE` on error | `CREATE TABLE IF NOT EXISTS` for an owned table (`owned = yes`): the mapped columns in file order with the backend type names, `NOT NULL` for null policy `error`, `PRIMARY KEY` (`wf_id` column, then the local key columns in order). An existing table is left as it is, rows included, whatever its columns: use `db-check-schema`. A table that is not owned is never created. |
| `(db-check-schema <template>)` | a multifield of difference strings, empty when the table matches; `FALSE` on error | Compares the mapping with the table in the database (owned or not). Columns the mapping does not name are ignored. |
| `(db-drop-table <template>)` | `TRUE`, or `FALSE` on error | `DROP TABLE IF EXISTS` (so `TRUE` also when the table is already gone), only when the configuration sets `orm_allow_drop = yes` ([`configuration.md`](configuration.md)) and only for an owned table. |

Generated SQL, for the example at the top on PostgreSQL:

```sql
CREATE TABLE IF NOT EXISTS "shop_order" ("wf_id" TEXT NOT NULL, "line_no" BIGINT NOT NULL, "status" TEXT, "amount" NUMERIC, "placed_at" TIMESTAMPTZ, PRIMARY KEY ("wf_id", "line_no"))
```

### Template names

`<template>` is a symbol or a string: `<package>::<template>` (`shop@1::order`) names the template of that package; an unqualified name is the template of the current module's package (the package of the rule that calls the function) when that package maps it, otherwise the template of the only loaded package that maps it. When several packages (versions of one definition) map it and none is the current module's, the call is an error that asks to qualify the name. Only packages whose mapping passed every check count. A wrong argument type or count is reported by CLIPS itself.

### db-check-schema

The table is looked up in the current schema: on PostgreSQL `information_schema.tables` and `information_schema.columns` with `table_schema = current_schema() AND table_name = $1`; on SQLite `sqlite_master` (`name = $1`, tables and views) and `pragma_table_info($1)`. Column names compare exactly on PostgreSQL and without ASCII case on SQLite. The strings, per mapped column in file order:

| Difference | String |
|---|---|
| No such table (then nothing else) | `table '<table>' does not exist` |
| Mapped column missing | `column '<column>' does not exist` |
| Column type not [compatible](#sql-types) | `column '<column>' has type '<catalog type>', expected <sql-type> (<backend type>)`, `<catalog type>` being the PostgreSQL `udt_name` (`text`, `int4`, ...) or the SQLite declared type as written |
| Column `NOT NULL` although the null policy is `nil` or `default` (the daemon may write NULL) | `column '<column>' is NOT NULL, expected nullable (null policy <null>)` |

A nullable column mapped with null policy `error` is not a difference (a row with NULL there is rejected when it is loaded, T10). Primary keys, defaults, constraints and column order are not compared. On SQLite a declared type matches when its affinity is the one of the generated type, so `TIMESTAMPTZ` (NUMERIC affinity) is not compatible with `timestamptz` there.

### Errors

An error writes one line to the daemon log stream (stderr, or the original stderr while the trace captures fds 1 and 2), raises a CLIPS evaluation error (the rule's actions stop, `[PRCCODE4] Execution halted during the actions of defrule ...`) and returns `FALSE`. Nothing is created, changed or dropped.

| Situation | Message |
|---|---|
| No loaded package maps the template | `clpdaemon: <udf>: unknown template '<template>' (no loaded package maps it)` |
| Unqualified name mapped by several packages | `clpdaemon: <udf>: template '<template>' is mapped by <pkg>, <pkg>[, ...]: qualify it as <package>::<template>` |
| `db-create-table` on a table that is not owned | `clpdaemon: db-create-table: template '<template>': table '<table>' is not owned (owned = no), so it is never created` |
| `db-drop-table` with `orm_allow_drop = no` | `clpdaemon: db-drop-table: dropping tables is disabled (orm_allow_drop = no)` |
| `db-drop-table` on a table that is not owned | `clpdaemon: db-drop-table: template '<template>': table '<table>' is not owned (owned = no), so it is never dropped` |
| The database refuses a statement | `clpdaemon: <udf>: template '<template>': table '<table>': <database message>` ([`database.md`](database.md#status), one line) |

`<udf>` is the function name, `<template>` the argument as given (control characters shown as `?`, at most 200 bytes). The checks run in the order of the table: the name first, then for `db-drop-table` the configuration key, then ownership.

## Interface

`src/orm.h` holds the CLIPS-free model and parser: `clpd_orm_parse` / `clpd_orm_free`, `clpd_orm_column_of`, `clpd_orm_resolve_key`, `clpd_orm_valid_ident`, the type and policy names, `clpd_orm_backend_type` (the table above), `clpd_orm_type_fits` / `clpd_orm_type_fits_list`, `clpd_orm_template_of` and `clpd_orm_table_difference`. `src/orm_ddl.h` holds `clpd_orm_ddl_register`, which `main.c` calls right after the database opens with a context (database, packages, `orm_allow_drop`) that lives as long as the CLIPS environment. After a clean load, `clpd_package.orm` holds the mapping, `clpd_package.orm_templates` the deftemplates in mapping order, and each template's `key_columns` the indexes of `wf-id` and the local key columns.
