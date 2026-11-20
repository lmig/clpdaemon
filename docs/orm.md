# ORM mapping

A rule package can map its deftemplates to database tables and name the database sequences its rules use (S7, S11, FR-22). The mapping lives in a separate file named by the manifest key `orm` ([`packages.md`](packages.md#manifest)). Nothing in it is implicit: every slot of a mapped template gets its own line with the column, the SQL type, the CLIPS type and what a NULL becomes.

The mapped templates of one workflow definition form its aggregate (FR-22); their tables carry the persistent state of each workflow instance. This page describes the file, the checks made when the package loads (T9a, T9b, T9e), the [DDL functions](#ddl-functions) `db-create-table`, `db-check-schema` and `db-drop-table` (T9b, T9d, FR-23), and the [sequence functions](#sequence-functions) `db-create-sequence` and `db-nextval` (T9c, T9e). The [cycle transaction](database.md#cycle-transaction) (T10a) runs every statement of a cycle in one transaction, the [automatic synchronization](#automatic-synchronization) (T10b, FR-25) writes the changes of the mapped facts at the end of each cycle inside it, and the [rehydration](#rehydration) (T10c, FR-26) reloads from the database the instances a rolled-back cycle touched, through the [loader](#loading-rows).

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
| `<tpl>.table` | table name, an [identifier](#identifiers); not `clpd_sequence`, reserved on every backend for the daemon's sequence counters ([below](#generated-sql)), nor `workflow_instance`, reserved for the daemon's instance table ([`database.md`](database.md#workflow_instance)) | required |
| `<tpl>.owned` | `yes`: the table belongs to the daemon (`db-create-table` may create it, `db-drop-table` may drop it when `orm_allow_drop = yes`); `no`: an existing table, never created nor dropped | required |
| `<tpl>.key` | comma-separated list of zero or more slots, the local key in order; `wf-id` is always the first key column and is not listed | empty (the key is `wf-id` alone) |
| `<tpl>.sync` | `auto`: the cycle writes the template's facts to its table itself ([automatic synchronization](#automatic-synchronization), FR-25, D7); `manual`: the synchronization never writes them, only the DML functions do (T10d) | `auto` |
| `<tpl>.slot.<slot>` | `<column> <sql-type> <clips-type> <null>`, four fields separated by spaces or tabs | one line required for every slot |

The primary key of a table is `wf-id` followed by the local key (FR-22). Every slot of the deftemplate, `wf-id` included, needs its `<tpl>.slot.<slot>` line; key slots (`wf-id` and the slots in `<tpl>.key`) must use the null policy `error`. Two slots of one template cannot use the same column, and two templates of one file cannot use the same table. Multislots cannot be mapped.

### Sequences

| Key | Value | Default |
|---|---|---|
| `seq.<alias>` | name of the database sequence, an [identifier](#identifiers), not `workflow_instance` (PostgreSQL keeps tables and sequences in one namespace, so it is reserved for the daemon's instance table as a sequence name too); `<alias>` is the name rules use (`(db-nextval order_no)`, [below](#sequence-functions)) and is an identifier too | required |
| `seq.<alias>.owned` | `yes`: the sequence belongs to the daemon (`db-create-sequence` may create it); `no`: an existing sequence, never created | required |

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

What a NULL column becomes when a row is [loaded](#loading-rows) into a fact (T10c); for the other direction, what the synchronization writes for `nil`, see the [write rules](#write-rules):

| `null` | Effect |
|---|---|
| `nil` | the slot gets the symbol `nil`, so the slot's type constraint must allow SYMBOL |
| `default` | the slot gets its deftemplate default |
| `error` | the row is rejected (one log line, no fact); required for key slots |

## Loading

The mapping is read after the package's files, the `wf-id` lint and the `ingest` check ([`packages.md`](packages.md#loading)), at startup and under `--check`. A package with `orm` needs a database: without the `database` key in the configuration ([`database.md`](database.md)) it is an error. The checks are:

1. The file itself: syntax, unknown or repeated keys, values (the reserved table names `clpd_sequence` and `workflow_instance`, the reserved sequence name `workflow_instance` and the `sync` values included), required keys, null policy of key slots, a column, table or sequence used twice in the file.
2. A configured `database`.
3. Each mapped template, in file order: it is a deftemplate of the package; it has a `wf-id` slot; each slot line, in file order, has an SQL type that holds its CLIPS type, names a slot of the template, and that slot's type constraint allows the declared CLIPS type and, with null policy `nil`, SYMBOL; each key slot is a slot of the template; every slot of the template is mapped and none is a multislot.
4. Tables, then sequences, already mapped by an earlier package of another definition. Versions of one definition (`shop@1` and `shop@2`) may map the same tables and sequences; two definitions may not. A package whose mapping file is valid claims its tables and sequences even if a later check failed. The mapping is not read when a file of the package fails to load (step 2 of [`packages.md`](packages.md#loading)).
5. Names shared by a sequence and a table (PostgreSQL keeps both in one namespace): each sequence of the file against the tables of earlier packages (any definition) and of the file itself, then each table of the file against the sequences of earlier packages.
6. Each table of the file that an earlier version of the same definition maps: both must map it identically, compared with the first such version. Identical means the same `owned`, the same primary key columns in order, and the same columns (in any order) with the same SQL type and the same NOT NULL (null policy `error` or not). Slot names, CLIPS types, `nil` versus `default` and `sync` are not part of the table and may differ.

Any error makes the daemon exit 1, like other package errors. Nothing is read from or written to the database at load.

## Messages

| Situation | Message |
|---|---|
| Mapping file cannot be opened or read | `clpdaemon: <file>: cannot open: <system error>` (or `cannot read`) |
| Syntax, NUL byte | same messages as the daemon configuration ([`configuration.md`](configuration.md#messages)) |
| Key of no known form | `clpdaemon: <file>:<line>: unknown key '<key>'` |
| Repeated key | `clpdaemon: <file>:<line>: duplicate key '<key>' (first set on line <n>)` |
| Bad table or sequence name | `clpdaemon: <file>:<line>: <key>: expected an identifier matching [a-z_][a-z0-9_]* (at most 63 bytes), got '<value>'` |
| Table named `clpd_sequence` (on every backend) | `clpdaemon: <file>:<line>: <tpl>.table: table name 'clpd_sequence' is reserved (the daemon's sequence counter table)` |
| Table named `workflow_instance` (on every backend) | `clpdaemon: <file>:<line>: <tpl>.table: table name 'workflow_instance' is reserved (the daemon's workflow instance table)` |
| Sequence named `workflow_instance` (on every backend) | `clpdaemon: <file>:<line>: seq.<alias>: sequence name 'workflow_instance' is reserved (the daemon's workflow instance table)` |
| Bad `sync` | `clpdaemon: <file>:<line>: <tpl>.sync: expected auto or manual, got '<value>'` |
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

With a `database` in the configuration the core defines five CLIPS functions (FR-23, D9): the three table functions below and the two [sequence functions](#sequence-functions), with the five [DML functions](#dml-functions) (T10d), before the plugins and packages load; without one they do not exist, so a package calling them fails to load (`Missing function declaration for 'db-create-table'`). Every statement they send goes through the database interface, at most one `CREATE` or `DROP` per call ([`database.md`](database.md)): the functions never send `BEGIN` or `COMMIT` and never `ALTER` a table. Called from a rule they run inside the [cycle transaction](database.md#cycle-transaction) (T10a, L90): a cycle that is rolled back undoes the tables it created or dropped, on both backends; elsewhere (`db-check-schema` at load or at the top level) they run in autocommit. The SQL holds only the table, column and type names of the mapping and, on PostgreSQL, the name of the current schema, always quoted (`"public"."shop_order"`); the catalog lookups pass the table name as a parameter (`= $1`).

`db-create-table`, `db-drop-table`, `db-create-sequence` and `db-nextval` change the database, so only a rule may call them, and only for its own workflow definition (L84, T9c): the call must come from the actions of a rule of a loaded package, and the template or sequence alias must be mapped by a package of the same definition as that rule's package (any version: a rule of `shop@2` may create the table of `shop@1::order`). Called anywhere else (the top level, a deffacts evaluated at `Reset`, a plugin hook such as `after_load`, a rule of a module that is no package) they fail and touch nothing. `db-check-schema` only reads the catalog and may be called from anywhere, for the template of any loaded package.

| Function | Returns | Effect |
|---|---|---|
| `(db-create-table <template>)` | `TRUE`, or `FALSE` on error | From a rule, for a template of its own definition: `CREATE TABLE IF NOT EXISTS` for an owned table (`owned = yes`): the mapped columns in file order with the backend type names, `NOT NULL` for null policy `error`, `PRIMARY KEY` (`wf_id` column, then the local key columns in order). An existing table is left as it is, rows included, whatever its columns: use `db-check-schema`. A table that is not owned is never created. |
| `(db-check-schema <template>)` | a multifield of difference strings, empty when the table matches; `FALSE` on error | Compares the mapping with the table in the database (owned or not). Columns the mapping does not name are ignored. |
| `(db-drop-table <template>)` | `TRUE`, or `FALSE` on error | From a rule, for a template of its own definition: `DROP TABLE IF EXISTS` (so `TRUE` also when the table is already gone), only when the configuration sets `orm_allow_drop = yes` ([`configuration.md`](configuration.md)) and only for an owned table. |

Generated SQL, for the example at the top on PostgreSQL with the current schema `public`:

```sql
CREATE TABLE IF NOT EXISTS "public"."shop_order" ("wf_id" TEXT NOT NULL, "line_no" BIGINT NOT NULL, "status" TEXT, "amount" NUMERIC, "placed_at" TIMESTAMPTZ, PRIMARY KEY ("wf_id", "line_no"))
DROP TABLE IF EXISTS "public"."shop_order"
```

On PostgreSQL both statements name the schema `db-check-schema` inspects, `current_schema()` (the first schema of the `search_path` that exists), read with `SELECT current_schema()` just before; so a table of the same name in a later `search_path` schema is never created over, dropped or reported. When no schema of the `search_path` exists the call is an error. On SQLite the table name is unqualified (the `main` database).

### Template names

`<template>` is a symbol or a string: `<package>::<template>` (`shop@1::order`) names the template of that package; an unqualified name is the template of the caller's package when that package maps it, otherwise the template of the only loaded package that maps it. When several packages map it and none is the caller's, the call is an error that asks to qualify the name. Only packages whose mapping passed every check count. A wrong argument type or count is reported by CLIPS itself.

- `db-check-schema`: the caller's package is the package of the current module (the package of the rule that calls it, or of the module set at the top level); every loaded package counts.
- `db-create-table` and `db-drop-table`: the caller's package is the package of the executing rule, and only the packages of its definition count. A qualified name of another definition's package, or an unqualified name only other definitions map, is an error of its own; several versions of the own definition mapping an unqualified name ask to qualify it, listing only those versions.

### db-check-schema

The table is looked up in the current schema: on PostgreSQL `information_schema.tables` and `information_schema.columns` with `table_schema = current_schema() AND table_name = $1`; on SQLite `sqlite_master` (`name = $1 COLLATE NOCASE`, tables and views) and `pragma_table_info($1)`. Table and column names compare exactly on PostgreSQL (the generated names are quoted, so `"SHOP_ORDER"` is another table) and without ASCII case on SQLite, as SQLite itself resolves names: a table created as `SHOP_ORDER` is the one `db-check-schema` inspects and the one `CREATE TABLE IF NOT EXISTS` leaves in place. The strings, per mapped column in file order:

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
| `db-create-table`, `db-drop-table`, `db-create-sequence` or `db-nextval` outside the actions of a rule (also a deffacts evaluated at `Reset`, or an `Eval` after a rule has run) | `clpdaemon: <udf>: only a rule of a rule package may call it (no rule is executing)` |
| The same functions from a rule of a module that is no package | `clpdaemon: <udf>: only a rule of a rule package may call it (rule '<rule>' is in module '<module>')` |
| No loaded package maps the template | `clpdaemon: <udf>: unknown template '<template>' (no loaded package maps it)` |
| `db-create-table` or `db-drop-table` on a template only other definitions map | `clpdaemon: <udf>: template '<template>' is not mapped by definition <definition> of the calling rule (mapped by <pkg>[, <pkg>...])`, the packages of other definitions that map it (for a qualified name, the package named) |
| Unqualified name mapped by several packages | `clpdaemon: <udf>: template '<template>' is mapped by <pkg>, <pkg>[, ...]: qualify it as <package>::<template>` (for `db-create-table` and `db-drop-table`, only versions of the calling rule's definition) |
| `db-create-table` on a table that is not owned | `clpdaemon: db-create-table: template '<template>': table '<table>' is not owned (owned = no), so it is never created` |
| `db-drop-table` with `orm_allow_drop = no` | `clpdaemon: db-drop-table: dropping tables is disabled (orm_allow_drop = no)` |
| `db-drop-table` on a table that is not owned | `clpdaemon: db-drop-table: template '<template>': table '<table>' is not owned (owned = no), so it is never dropped` |
| PostgreSQL: no schema of the `search_path` exists (`db-create-table`, `db-drop-table`) | `clpdaemon: <udf>: template '<template>': table '<table>': no current schema (the search_path names no existing schema)` |
| The database refuses a statement (also a catalog lookup or the `current_schema()` query) | `clpdaemon: <udf>: template '<template>': table '<table>': <database message>` ([`database.md`](database.md#status), one line) |

`<udf>` is the function name, `<template>` the argument as given, `<rule>` and `<module>` the calling rule and its module (all three with control characters shown as `?`, at most 200 bytes). The checks run in the order of the table: for `db-create-table` and `db-drop-table` the caller first, then the name, then for `db-drop-table` the configuration key, then ownership, then the current schema and the statement.

## Sequence functions

A rule draws numbers from the database sequences its package maps (`seq.<alias>`, [above](#sequences)) by alias (T9c, L82). Both functions follow the rules of the [DDL functions](#ddl-functions): they exist only with a `database`, only a rule of a rule package may call them, only for the aliases of its own definition, and their statements run inside the cycle transaction.

| Function | Returns | Effect |
|---|---|---|
| `(db-create-sequence <alias>)` | `TRUE`, or `FALSE` on error | Creates an owned sequence (`seq.<alias>.owned = yes`) when it does not exist; an existing sequence keeps its value (`TRUE`). A sequence that is not owned is never created. |
| `(db-nextval <alias>)` | the next value, an INTEGER (`1`, `2`, `3`, ... for a sequence the daemon created); `FALSE` on error | Advances the sequence in one statement and returns the new value. The sequence is shared: versions of one definition that map the same sequence, and other daemons or connections on the same database, never get the same value. On PostgreSQL a value drawn is used up even when the rule's later actions fail or the cycle is rolled back (sequences are not transactional). On SQLite the counter row is rolled back with the cycle, so a rolled-back cycle's values come again (SQLite is for tests). |

`<alias>` is a symbol or a string, resolved like a template name for `db-create-table` ([template names](#template-names)): `<package>::<alias>` names the alias of that package, which must be a version of the calling rule's definition; an unqualified alias is the calling rule's package's alias when that package maps it, otherwise the alias of the only version of the definition that maps it; several versions mapping it (even to the same sequence) ask to qualify it.

### Generated SQL

| | PostgreSQL | SQLite |
|---|---|---|
| `db-create-sequence` | `SELECT current_schema()`, then `CREATE SEQUENCE IF NOT EXISTS "<schema>"."<name>"` (PostgreSQL's defaults: `bigint`, start 1, increment 1) | `CREATE TABLE IF NOT EXISTS "clpd_sequence" ("name" TEXT PRIMARY KEY, "value" INTEGER NOT NULL)`, then `INSERT INTO "clpd_sequence" ("name", "value") VALUES ($1, 0) ON CONFLICT ("name") DO NOTHING` |
| `db-nextval` | `SELECT current_schema()`, then `SELECT nextval($1)` with `"<schema>"."<name>"` as the TEXT parameter (no cast in the SQL text) | `UPDATE "clpd_sequence" SET "value" = "value" + 1 WHERE "name" = $1 AND typeof("value") = 'integer' AND "value" < 9223372036854775807 RETURNING "value"` |

On PostgreSQL the sequence lives in `current_schema()`, like the tables: a same-named sequence of a later `search_path` schema is never created over or advanced. SQLite has no sequences, so the counter table `clpd_sequence` stands in for them (tests only, [`database.md`](database.md#sqlite-stand-ins)): one row per sequence name, `value` the last value returned (0 before the first), advanced by one `UPDATE ... RETURNING` statement (SQLite 3.35 or later). So that no mapped table can collide with it (and `db-drop-table` never resets the counters), the table name `clpd_sequence` is reserved on every backend: a mapping that names it does not load ([messages](#messages)). The `UPDATE` only advances a row whose value is an INTEGER below the maximum: a value of another type, written by someone else (a REAL would be advanced and come back as a REAL; TEXT and BLOB compare above every number), leaves the row unchanged and the call is an error of its own. When a call fails or returns no value, the function looks the sequence up (PostgreSQL: a sequence, `relkind = 'S'`, of that name in `current_schema()` in `pg_catalog.pg_class`, so a table or view of that name is a missing sequence; SQLite: the counter table, the row and the type of its value) to tell a missing sequence from another error. The largest value is 9223372036854775807 (INT64): on SQLite the guard leaves a row at the maximum unchanged and the call is an error; on PostgreSQL the sequence's own `MAXVALUE` applies and the call fails with PostgreSQL's message.

On SQLite `db-create-sequence` sends two statements, the `CREATE TABLE IF NOT EXISTS` of the counter table and the `INSERT` of the row. When the `INSERT` fails the call returns `FALSE` with the database message; inside the cycle transaction that failure rolls the whole cycle back, the counter table included. Outside one (autocommit) the counter table the first statement created stays (empty of that row): `db-nextval` then reports the sequence as missing, and calling `db-create-sequence` again adds the row.

### Sequence errors

The same log line, CLIPS evaluation error and `FALSE` as the [DDL errors](#errors); nothing is advanced, and nothing is created except the SQLite counter table when only the row's `INSERT` fails ([above](#generated-sql)).

| Situation | Message |
|---|---|
| Not called from the actions of a rule of a rule package | the two `only a rule of a rule package may call it` messages of the [DDL errors](#errors) |
| No loaded package maps the alias | `clpdaemon: <udf>: unknown sequence alias '<alias>' (no loaded package maps it)` |
| An alias only other definitions map (for a qualified alias, the package named) | `clpdaemon: <udf>: sequence alias '<alias>' is not mapped by definition <definition> of the calling rule (mapped by <pkg>[, <pkg>...])` |
| Unqualified alias mapped by several versions of the calling rule's definition | `clpdaemon: <udf>: sequence alias '<alias>' is mapped by <pkg>, <pkg>[, ...]: qualify it as <package>::<alias>` |
| `db-create-sequence` on a sequence that is not owned | `clpdaemon: db-create-sequence: sequence alias '<alias>': sequence '<name>' is not owned (owned = no), so it is never created` |
| `db-nextval` on a sequence that does not exist (PostgreSQL: no sequence of that name in `current_schema()`, also when a table or view has the name; SQLite: no counter table or no row) | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>' does not exist`; inside the cycle transaction, when the statement itself failed (PostgreSQL: no sequence; SQLite: no counter table), the lookup cannot run and the line ends with the database message instead (`... sequence '<name>': relation "<schema>.<name>" does not exist (SQLSTATE 42P01)`, SQLite `... sequence '<name>': no such table: clpd_sequence`), and the cycle is rolled back |
| SQLite: the counter row holds a value that is no INTEGER (REAL, TEXT, BLOB; written by someone else), left unchanged | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>': the counter value is not an INTEGER` |
| SQLite: the counter is at 9223372036854775807 | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>' has reached its maximum value` |
| PostgreSQL: no schema of the `search_path` exists | `clpdaemon: <udf>: sequence alias '<alias>': sequence '<name>': no current schema (the search_path names no existing schema)` |
| The database refuses a statement (PostgreSQL's maximum included: `nextval: reached maximum value of sequence "?" (9223372036854775807) (SQLSTATE 2200H)`, the name hidden like every data exception, [`database.md`](database.md#status)), or `db-nextval` got no value and the lookup that follows fails | `clpdaemon: <udf>: sequence alias '<alias>': sequence '<name>': <database message>` |
| `db-nextval`: the statement returned a value that is no INTEGER (neither backend's statement should: a safety net) | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>': the database returned no INTEGER` |

`<alias>` is the argument as given (control characters shown as `?`, at most 200 bytes). The checks run in the order of the table: the caller, the alias, ownership, then the statements.

### Decisions (T9c)

- `db-nextval` returns an INTEGER and takes an alias of the calling rule's own definition, any version, resolved like `db-create-table`; it is refused outside a rule (fail-closed).
- Owned sequences are created by the separate `db-create-sequence`, not by `db-create-table`, with the same caller scope; a sequence that is not owned is never created.
- PostgreSQL: `CREATE SEQUENCE IF NOT EXISTS` and `nextval($1)` on the name qualified with `current_schema()`, the name passed as a TEXT parameter; SQLite: the counter table `clpd_sequence(name TEXT PRIMARY KEY, value INTEGER NOT NULL)` with one row per name, starting so that the first value is 1, like PostgreSQL.
- Errors as the other DDL functions; a missing sequence and an INT64 overflow are errors (SQLite guards in the statement, so the row is not changed; PostgreSQL reports its own error); error messages never show a value from the database.

### Decisions (T9e)

- The table name `clpd_sequence` is reserved on every backend (a mapping naming it fails to load), so a mapped table can never be the SQLite counter table.
- PostgreSQL: only a sequence (`relkind = 'S'`) counts as existing in the lookup after a failed `nextval`. SQLite: the `UPDATE` also requires `typeof("value") = 'integer'`, so a counter of another type is never advanced and gets its own message instead of the maximum-value one.

## DML functions

A rule reads and writes the rows of its own definition's mapped tables explicitly with five functions (T10d, FR-24, L90). They exist with the [DDL functions](#ddl-functions) (only with a `database`) and share their caller scope: only the actions of a rule of a loaded package may call them, only for a template mapped by a package of that rule's definition (any version), resolved like a template name for `db-create-table` ([template names](#template-names)). Their statements run inside the [cycle transaction](database.md#cycle-transaction).

The writes (`db-insert`, `db-update`, `db-delete`) take only templates mapped with `sync = manual`, so they never race the [automatic synchronization](#automatic-synchronization), which writes the `sync = auto` ones; `db-load` and `db-count` take any mapped template.

| Function | Returns | Effect |
|---|---|---|
| `(db-insert <template> <slot> <value> ...)` | `TRUE`, or `FALSE` on error | `INSERT` of one row with the given slots; every key slot (`wf-id` and the local key) needs a value; an omitted slot is left out of the `INSERT`, so the column gets its default (NULL without one; a NOT NULL column without a default is the database's error). |
| `(db-update <template> (create$ <slot> <value> ...) <slot> <value> ...)` | the number of rows changed (INTEGER, 0 when none matches), or `FALSE` on error | `UPDATE` of the given non-key slots of the rows the filters select; the filters name key slots only (`wf-id` and the local key slots, any of them: `(create$ wf-id w1)` updates every row of instance `w1`), at least one; a key slot is never changed (delete and insert instead). |
| `(db-delete <template> (create$ <slot> <value> ...))` | the number of rows deleted (INTEGER), or `FALSE` on error | `DELETE` of the rows the filters select; the filters may name any mapped slot, at least one of them `wf-id` or a local key slot. |
| `(db-load <template> (create$ <slot> <value> ...))` | the number of facts asserted (INTEGER), or `FALSE` on error | The rows the filters select, asserted as facts of the template through the [loader](#loading-rows), without echo: never written back, also for a `sync = auto` template. The filters may name any mapped slot, at least one of them `wf-id` or a local key slot (no filter, or only non-key ones, would load whole tables). A row the loader rejects logs its `load:` line, is not asserted and not counted; the call still succeeds. A row whose fact is already in working memory is counted but asserted only once (CLIPS keeps one copy of a fact). |
| `(db-count <template> (create$ <slot> <value> ...))` | the number of rows (INTEGER), or `FALSE` on error | `SELECT count(*)` of the rows the filters select; any mapped slot, and an empty `(create$)` counts the whole table. |

The `<slot> <value>` pairs are the arguments after the template (or after the filters for `db-update`) and the items of the `(create$ ...)` filter list; a slot name is a symbol or a string, each slot at most once per list. A value is typed per its column's mapping by the [write rules](#write-rules) of the synchronization: the declared CLIPS type (a string and a symbol for either lexeme type), `0` or `1` for a `boolean` column, the bytes of a string for `bytea`, the symbol `nil` in a column of null policy `nil` is NULL (in a filter `IS NULL`); in a column of another null policy `nil` is an ordinary symbol (text `nil`, a type error in an `integer` or `float` column). The filters are equalities ANDed, in the order given.

### DML statements

One statement per call, parameterized, the names from the mapping only, quoted, and on PostgreSQL qualified with `current_schema()` like the [DDL functions](#ddl-functions) (one `SELECT current_schema()` first). For the example `item` of `shop@1` (`item.key = n`) on PostgreSQL with the current schema `public`:

```sql
INSERT INTO "public"."shop_item" ("wf_id", "n", "qty") VALUES ($1, $2, $3)
UPDATE "public"."shop_item" SET "qty" = $1, "label" = $2 WHERE "wf_id" = $3 AND "n" = $4 RETURNING "wf_id"
DELETE FROM "public"."shop_item" WHERE "wf_id" = $1 AND "label" IS NULL RETURNING "wf_id"
SELECT count(*) FROM "public"."shop_item" WHERE "wf_id" = $1
```

`db-load` sends the loader's `SELECT` ([loading rows](#loading-rows)) with its filters. The `INSERT` columns and the `SET` list are in mapping order, whatever the order of the arguments. Every parameter is a value or the right side of a comparison with its column, so PostgreSQL types it from the column (`timestamptz`, `jsonb`, `numeric`, `boolean` from the text; `int8`, `float8` and `bytea` parameters are typed by the interface, [`database.md`](database.md#statements-and-parameters)); no parameter is ever in a context without a type. `RETURNING` gives the instance of each row changed.

### DML and the cycle

Called from a rule, the statements are part of the cycle: they commit with it. A statement the database refuses (a duplicate primary key of `db-insert`, text PostgreSQL cannot read as the column's type, a timeout) logs the function's line and rolls the whole cycle back, the rows the rules wrote before it included, like any [failed statement](database.md#cycle-transaction): the cycle transaction line follows, and the cycle is retried after the backoff. The instances the DML functions wrote in the cycle (the `wf-id` of an inserted row, the `wf_id` of every row `db-update` or `db-delete` changed) count as touched by the cycle, so a rollback [evicts and reloads](#rehydration) them, like the instances whose facts changed: the facts a `db-load` asserted from rows that the rollback removes are gone. `db-load` and `db-count` read; a rejected row is no error. A function that refuses before sending its statement (every message below but the database's) sends nothing and does not fail the cycle (a CLIPS evaluation error, which commits, [`database.md`](database.md#cycle-transaction)). The DML functions never write `workflow_instance`: only the automatic synchronization does.

### DML errors

The same log line, CLIPS evaluation error and `FALSE` as the [DDL errors](#errors); nothing is written and nothing asserted. `<udf>` is the function, `<template>` the argument as given, `<slot>` the slot argument as given (both with control characters shown as `?`, at most 200 bytes).

| Situation | Message |
|---|---|
| Not called from the actions of a rule of a rule package; a template that is unknown, of another definition, or ambiguous | the messages of the [DDL errors](#errors) (`only a rule of a rule package may call it (...)`, `unknown template ...`, `... is not mapped by definition <definition> of the calling rule (mapped by ...)`, `... qualify it as <package>::<template>`) |
| `db-insert`, `db-update` or `db-delete` on a `sync = auto` template | `clpdaemon: <udf>: template '<template>': table '<table>' has sync = auto: only the automatic synchronization writes it` |
| An odd number of arguments after the template (`db-insert`) or after the filters (`db-update`) | `clpdaemon: <udf>: template '<template>': table '<table>': the slot values must come in <slot> <value> pairs` |
| A slot argument that is no symbol or string | `clpdaemon: <udf>: template '<template>': table '<table>': expected a slot name, got <TYPE>` |
| A slot the template's mapping does not name | `clpdaemon: <udf>: template '<template>': table '<table>': slot '<slot>' is not mapped` |
| A slot given twice | `clpdaemon: <udf>: template '<template>': table '<table>': slot '<slot>' is given twice` |
| A value of another type than the column's, or a boolean other than 0 or 1 | `clpdaemon: <udf>: template '<template>': table '<table>': slot '<slot>' (column '<column>'): expected <clips-type>, got <TYPE>`, `... slot '<slot>' (column '<column>'): a boolean column takes 0 or 1` (`<TYPE>` as in the [synchronization failures](#failures)) |
| `db-insert` without a value for a key slot | `clpdaemon: db-insert: template '<template>': table '<table>': key slot '<slot>' needs a value` |
| `db-update` setting a key slot | `clpdaemon: db-update: template '<template>': table '<table>': slot '<slot>' is a key slot, which db-update never changes` |
| `db-update` without a slot to set | `clpdaemon: db-update: template '<template>': table '<table>': no slot to set` |
| An odd number of items in the filter list | `clpdaemon: <udf>: template '<template>': table '<table>': the filters must come in <slot> <value> pairs` |
| A filter item that should be a slot name and is not, a filter slot not mapped, given twice, or with a value of another type | the four messages above with `filter` before them: `...: filter: expected a slot name, got <TYPE>`, `...: filter slot '<slot>' is not mapped`, `...: filter slot '<slot>' is given twice`, `...: filter slot '<slot>' (column '<column>'): expected <clips-type>, got <TYPE>` (or `... a boolean column takes 0 or 1`) |
| `db-update`: a filter on a slot that is no key slot | `clpdaemon: db-update: template '<template>': table '<table>': filter slot '<slot>' is not a key slot` |
| `db-update`, `db-delete`, `db-load`: no filter on `wf-id` or a local key slot (an empty list included) | `clpdaemon: <udf>: template '<template>': table '<table>': the filters must name wf-id or a key slot` |
| PostgreSQL: no schema of the `search_path` exists | `clpdaemon: <udf>: template '<template>': table '<table>': no current schema (the search_path names no existing schema)` |
| The database refuses the statement (also the `current_schema()` query) | `clpdaemon: <udf>: template '<template>': table '<table>': <database message>` ([`database.md`](database.md#status), one line; for example `UNIQUE constraint failed: shop_item.wf_id, shop_item.n` on SQLite, `duplicate key value violates unique constraint "shop_item_pkey" (SQLSTATE 23505)` on PostgreSQL) |
| `db-count`: the database returned no INTEGER (a safety net) | `clpdaemon: db-count: template '<template>': table '<table>': the database returned no INTEGER` |

The checks run in the order of the table: the caller, the template, `sync`, then the lists (for `db-update` the filters, then the slot values), each pair in order (slot name, mapping, key rules, repetition, value), then the missing key slots of `db-insert`, the filter rule and `no slot to set`, then the current schema and the statement. No message shows a value.

### Decisions (T10d)

- Return values: `db-insert` `TRUE`; `db-update` and `db-delete` the number of rows changed (from `RETURNING`); `db-load` the number of facts asserted; `db-count` the count; `FALSE` on every error.
- `db-insert` needs every key slot; omitted non-key slots are left out of the `INSERT` (the column default applies, D21 existing tables), not written as NULL.
- `db-update` filters on key slots only and never sets one; `db-delete` and `db-load` filter on any mapped slot but need at least one filter on `wf-id` or a local key slot; `db-count` accepts any filters, none included (it reads only a number).
- The instances the DML functions write count as touched by the cycle (the open question of T10c): a rollback evicts and reloads them. A `db-load` alone touches nothing (its facts mirror rows as read; a rollback that removes rows also evicts the instance that wrote them).
- The DML functions do not write `workflow_instance` rows (only the automatic synchronization does).

## Automatic synchronization

With a `database`, every fact of a template mapped with `sync = auto` by a loaded package is written to its table by the cycle itself (T10b, FR-25, D7, L90): the daemon records the net change of each such fact since the last committed cycle and, at the end of the cycle (after `Run`, before the commit of the [cycle transaction](database.md#cycle-transaction)), sends the statements that bring the tables to the facts. Nothing has to be called from the rules. The hooks are always on: they do not depend on the trace, on `watch` or on any configuration key.

What is never written: facts of a template with `sync = manual` (only the DML functions write those, T10d), of a template no mapping names (also a template of a mapped package that its mapping leaves out), ordered facts, and facts asserted or retracted while the daemon is loading from the database ([echo suppression](#echo-suppression)). Every slot of a mapped template is mapped ([loading](#loading), step 3), so every change of a fact of a synchronized template is a change of its row.

### Net changes

The daemon compares, per fact, the row the fact had at the last commit with the row it has at the end of the cycle (a modify keeps the fact's index, so several modifies of one fact are one change):

| In the cycle (since the last commit) | Written |
|---|---|
| asserted | `INSERT` of the final values (asserted and then modified: still one `INSERT`) |
| asserted, then retracted | nothing |
| modified, the primary key unchanged | one `UPDATE` of the columns whose value differs, by the primary key; nothing when the values came back to the stored ones |
| modified, a key slot changed (`wf-id` or a local key slot) | `DELETE` of the old key, then `INSERT` of the new row |
| retracted | `DELETE` by the primary key |

The primary key is the `wf_id` column followed by the local key columns (FR-22). An `UPDATE` that finds no row with the key (another writer deleted it, or the fact was loaded without being stored) inserts the row instead. A `DELETE` that finds no row is not an error.

The facts that exist before the first cycle (the deffacts asserted by `Reset` at startup, facts asserted by a plugin's `init` or `after_load`) are changes since the last commit like any other: the first cycle inserts them. So is a fact asserted between cycles (a plugin's `cycle_end`): the next cycle writes it.

Two facts of one template with the same primary key in working memory (they differ in another slot, so CLIPS keeps both) cannot both be stored: the second `INSERT` is a unique violation (`CLPD_DB_UNIQUE`) and the whole cycle is rolled back. The same holds for a fact whose key matches a row stored by another writer or by an earlier run of the daemon: a deffacts fact of a mapped template is inserted again at every start, so a restart against a database that already holds its row rolls the first cycle back and [isolates](#failures) that instance, which is then evicted without a reload ([rehydration](#rehydration)); the other instances commit on the retry, unless the first cycle touched them too (rehydration at startup comes with T11/T22).

### Statements and order

The statements are parameterized; the names come from the mapping, quoted, and on PostgreSQL qualified with `current_schema()` like the [DDL functions](#ddl-functions) (one `SELECT current_schema()` per cycle that writes):

```sql
INSERT INTO "public"."shop_order" ("wf_id", "line_no", "status", "amount", "placed_at") VALUES ($1, $2, $3, $4, $5)
UPDATE "public"."shop_order" SET "status" = $1 WHERE "wf_id" = $2 AND "line_no" = $3
DELETE FROM "public"."shop_order" WHERE "wf_id" = $1 AND "line_no" = $2
INSERT INTO "public"."workflow_instance" ("wf_id", "definition", "version", "status", "created_at", "updated_at") VALUES ($1, $2, $3, 'active', now(), now()) ON CONFLICT ("wf_id") DO UPDATE SET "updated_at" = excluded."updated_at"
```

The `INSERT` lists every mapped column in file order; the `UPDATE` sets the changed non-key columns in file order. The changes are grouped per aggregate (workflow definition), the aggregates in the order their first package loaded; within an aggregate the `DELETE`s come first, then the `UPDATE`s, then the `INSERT`s (each in the order the facts first changed), so a key that moves from one fact to another never collides; then the `workflow_instance` row of each instance the aggregate wrote, in the order of the changes.

### workflow_instance rows

Each instance (`wf-id` value) a cycle writes gets its [`workflow_instance`](database.md#workflow_instance) row in the same transaction: the first write inserts it with `status = active` and the definition and version of the package of its first changed fact (parent, owner node and lease stay NULL until T21/T24); every write sets `updated_at`. An existing row keeps its definition, version, status and `created_at`. The `wf_id` is the text of the `wf-id` slot (a symbol or string as it is, an integer in decimal). A modify of `wf-id` touches both instances. An instance whose changes all cancel out is not written.

The timestamps come from the database clock, so every node and every other writer share one clock: PostgreSQL `now()` (the start of the cycle transaction: every row of a cycle has the same time), stored as `TIMESTAMPTZ`; SQLite `strftime('%Y-%m-%dT%H:%M:%fZ', 'now')`, UTC with milliseconds, stored as TEXT (`2026-11-19T17:51:10.960Z`, the format of the trace timestamps).

### Write rules

A slot value is written per the column's declared CLIPS type and null policy:

| Value | Column | Written |
|---|---|---|
| the symbol `nil` | null policy `nil` | NULL |
| the symbol `nil` | null policy `default` or `error` | as any other symbol (text `nil` in a text column; in an `integer` or `float` column it is a type error, below) |
| INTEGER | `integer` | an integer (`int8` on PostgreSQL); in a `boolean` column `0` is false and `1` is true, any other integer is an error |
| FLOAT | `float` | a float (`float8`; in a `numeric` column PostgreSQL converts it) |
| STRING or SYMBOL | `string` or `symbol` | text (in a `timestamptz`, `jsonb` or `numeric` column PostgreSQL parses it); in a `bytea` column the bytes of the string |
| any other type (an INTEGER in a `float` column, a FLOAT or INTEGER in a `string` column, an instance name, a fact address, ...) | | an error |

PostgreSQL may still refuse a written value: an integer above 2147483647 for an `int4` column of an existing table, text that is no valid timestamp or JSON, a string that is no valid UTF-8. That is a database error like any other (below). SQLite stores what it is given (its `INTEGER` holds 64 bits).

### Echo suppression

Facts asserted while the daemon loads rows from the database (the [rehydration](#rehydration) of T10c, `db-load` of T10d, both through the [loader](#loading-rows)) mirror stored rows and must not be written back. Those loaders call `clpd_orm_sync_suspend` before and `clpd_orm_sync_resume` after (the calls nest): while suspended, an assert is not recorded, and a retract drops whatever was recorded for that fact (a rehydration discards it for the stored state). A later modify of a loaded fact is an `UPDATE` against the values it was loaded with.

### Failures

A write the database refuses, or a value the [write rules](#write-rules) cannot write, rolls the whole cycle back (the rules' DDL and the rows written before it included) through the [cycle transaction](database.md#cycle-transaction): one line names the write, the cycle transaction line follows, and the cycle is retried after the backoff. The instance (`wf-id`) of that write or value is then isolated: its recorded changes are dropped, later changes of its facts are not recorded, and `clpdaemon: sync: instance '<wf-id>' is no longer synchronized: <reason>` (the same reason) follows the cycle transaction line. Then the [rehydration](#rehydration) evicts every instance the cycle touched, and the isolated one: their facts are retracted and their changes dropped; the isolated instance is not reloaded. The instances the cycle did not touch keep their changes, so the retry writes them. The isolation lasts until the daemon restarts (T11 may reload such an instance on demand). A lost connection (`CLPD_DB_LOST`), a failed `begin` or commit, a missing current schema and a failed statement of a rule (its DDL or sequence functions) isolate nothing: the retry writes everything again. When a statement of the cycle already failed (for example a rule's `db-nextval`), the synchronization sends nothing and the commit reports that failure.

| Situation | Messages |
|---|---|
| The database refuses a write of a mapped table | `clpdaemon: sync: <pkg>::<template>: table '<table>': <database message>`, then `clpdaemon: cycle transaction failed: <database message>; retrying in <n> ms` |
| The database refuses the instance row | `clpdaemon: sync: table 'workflow_instance': <database message>`, then the same cycle transaction line |
| PostgreSQL has no current schema | `clpdaemon: sync: no current schema (the search_path names no existing schema)`, then `clpdaemon: cycle transaction failed: no current schema (the search_path names no existing schema); retrying in <n> ms` |
| A value of another type than the declared one | `clpdaemon: sync: <pkg>::<template>: table '<table>': slot '<slot>' (column '<column>'): expected <clips-type>, got <TYPE>`, then `clpdaemon: cycle transaction failed: slot '<slot>' (column '<column>'): expected <clips-type>, got <TYPE>; retrying in <n> ms`; `<TYPE>` is `INTEGER`, `FLOAT`, `SYMBOL`, `STRING`, `INSTANCE-NAME`, `FACT-ADDRESS`, `INSTANCE-ADDRESS`, `EXTERNAL-ADDRESS` or `MULTIFIELD` |
| An integer other than 0 or 1 in a `boolean` column | `clpdaemon: sync: <pkg>::<template>: table '<table>': slot '<slot>' (column '<column>'): a boolean column takes 0 or 1`, then the cycle transaction line with that reason |

For example, two facts with one key on SQLite: `clpdaemon: sync: shop@1::order: table 'shop_order': UNIQUE constraint failed: shop_order.wf_id, shop_order.line_no`; on PostgreSQL `... duplicate key value violates unique constraint "shop_order_pkey" (SQLSTATE 23505)`. No message shows a slot value.

### Decisions (T10b)

- The facts before the first cycle (deffacts at `Reset`, startup) are written by the first cycle, like any change since the last commit (tested).
- A fact asserted and modified in one cycle is one `INSERT` with the final values; asserted and retracted, nothing; several modifies, one `UPDATE` of the changed columns; a key change, `DELETE` + `INSERT`.
- Two facts with one primary key: the second `INSERT` fails with a unique violation, the cycle is rolled back and the instance isolated (documented above, tested); the same for a restart that inserts its deffacts again.
- An `UPDATE` that finds no row inserts it; a `DELETE` that finds none is not an error.
- A value of another type than the declared CLIPS type is an error (a string and a symbol are interchangeable); a `boolean` column takes only 0 and 1.
- The timestamps of `workflow_instance` come from the database clock (above).
- A modify is recorded through the assert and retract hooks (CLIPS 6.4.2 keeps the fact index), so no modify hook is needed; there is no unmapped slot to ignore, since every slot of a mapped template is mapped.
- After a rolled-back cycle the recorded changes were kept for the retry, except those of an instance whose write failed for good, which is isolated (user decision "Aislar la instancia", native review R3-001/R4-001): one fact that cannot be written would otherwise roll back every retry forever. Since T10c the changes of the instances the cycle touched are dropped with their facts and the instances reloaded ([rehydration](#rehydration)); only the changes of instances it did not touch (for example facts asserted before the first cycle) are kept for the retry.

## Loading rows

The loader (`src/orm_load.h`, T10c) turns stored rows into facts. It is one routine for every caller: the [rehydration](#rehydration) after a rolled-back cycle, and later the on-demand rehydration of idle instances (T11), the failover (T22) and `db-load` (T10d). For one mapped template and a set of equality filters (ANDed; a NULL filter value is `IS NULL`) it sends one parameterized statement, every mapped column in mapping order, in primary key order:

```sql
SELECT "wf_id", "line_no", "status", "amount", "placed_at" FROM "public"."shop_order" WHERE "wf_id" = $1 ORDER BY "wf_id", "line_no"
```

The names come from the mapping, quoted and on PostgreSQL qualified with `current_schema()` like the other statements. The rows are read whole first, so a statement that fails asserts nothing. Then each row is asserted as a fact of its template (through a `FactBuilder`, never `Eval`) with the synchronization suspended ([echo suppression](#echo-suppression)): the facts are never written back, and a later change of one is written against the loaded values. Every column goes to its slot by the column's declared CLIPS type and [null policy](#null-policies):

| Stored value | `clips-type` | Slot value |
|---|---|---|
| NULL | any, null policy `nil` | the symbol `nil` |
| NULL | any, null policy `default` | the slot's deftemplate default (a slot declared `(default ?NONE)` has none: the row is rejected) |
| NULL | any, null policy `error` | the row is rejected |
| INTEGER (also a PostgreSQL `boolean`, as 0 or 1) | `integer` | INTEGER |
| FLOAT or INTEGER | `float` | FLOAT |
| TEXT that is a whole finite number (a PostgreSQL `numeric`, an SQLite `numeric` column) | `float` | FLOAT |
| TEXT (`text`, `timestamptz`, `jsonb`, `numeric`: as PostgreSQL prints it) | `string` / `symbol` | STRING / SYMBOL |
| BLOB (`bytea`) without a NUL byte | `string` / `symbol` | STRING / SYMBOL of its bytes |
| anything else (a TEXT in an `integer` column, possible on SQLite; a BLOB with a NUL byte, which no CLIPS string can hold) | | the row is rejected |

The slot's own constraints apply too (type, range, allowed values): a value they refuse rejects the row. A rejected row is not asserted, the other rows are, and the daemon log stream gets one line per rejected row:

`clpdaemon: load: <pkg>::<template>: table '<table>': instance '<wf-id>': row rejected: <reason>`

`<wf-id>` is the row's `wf_id` value (control characters shown as `?`, at most 200 bytes); when that column is NULL the `instance '<wf-id>': ` part is left out. `<reason>` is one of:

| Reason | When |
|---|---|
| `column '<column>' (slot '<slot>') is NULL and its null policy is error` | NULL in a column of null policy `error` |
| `column '<column>' (slot '<slot>') is NULL and the slot has no default` | NULL with null policy `default` for a slot declared `(default ?NONE)` |
| `column '<column>' (slot '<slot>'): expected <clips-type>, got <TYPE>` | a value of another type (`<TYPE>` `INTEGER`, `FLOAT`, `TEXT` or `BLOB`) |
| `column '<column>' (slot '<slot>'): the text is not a number` | TEXT in a `float` column that is no whole finite number |
| `column '<column>' (slot '<slot>'): the value holds a NUL byte` | a BLOB with a NUL byte for a `string` or `symbol` slot |
| `column '<column>' (slot '<slot>'): the slot's type constraint does not allow the value` | CLIPS refuses the value for the slot's `type` |
| `column '<column>' (slot '<slot>'): the value is outside the slot's range` | the slot's `range` |
| `column '<column>' (slot '<slot>'): the slot's allowed values do not include the value` | the slot's `allowed-...` attribute |
| `column '<column>' (slot '<slot>'): the slot's cardinality does not allow the value`, `... the slot's allowed classes do not include the value`, `... CLIPS refused the value` | the other refusals CLIPS reports for a slot value (not expected for a mapped single-field slot) |
| `CLIPS cannot build a fact of the template` | CLIPS cannot create a fact builder for the template (not expected after a clean load) |
| `CLIPS could not assert the fact` | CLIPS refuses the assert itself |

The columns are checked in mapping order and the first failure is the reason. No reason shows a stored value. For example, with `item.slot.req = req text string error` and a NULL `req`: `clpdaemon: load: shop@1::item: table 'shop_item': instance 'n1': row rejected: column 'req' (slot 'req') is NULL and its null policy is error`.

An instance is loaded per package: every mapped template of the package (`sync = auto` and `manual`, in mapping order), each with the filter `wf_id = <wf-id>` typed per the `wf-id` column's CLIPS type (an `integer` column takes the instance as a decimal integer; an instance that is no such number has no rows there). When versions of one definition share a table and both are reloaded for one instance, the table is read once, for the first of them.

## Rehydration

When the [cycle transaction](database.md#cycle-transaction) is rolled back, the working memory of the instances the cycle touched no longer matches the database: their stored rows are as the last committed cycle left them. The daemon then discards and reloads those instances (T10c, FR-26, D8, L90 "Todos los hechos de las instancias tocadas"):

1. **Touched instances.** While a cycle runs, the daemon notes the instance (the text of the `wf-id` slot, as [`workflow_instance`](#workflow_instance-rows) keys it) of every fact asserted, modified or retracted in it, of any template, mapped or not: the facts drained from the ingest queue at its start, the rules' actions, plugin functions. Since T10d also the instance of every row the [DML functions](#dml-functions) insert, update or delete. A fact of a template without a `wf-id` slot belongs to no instance. Rules that fired without changing a fact (for example a rule that only calls `db-nextval`) touch nothing.
2. **Eviction, at the rollback.** Every fact in working memory carrying the `wf-id` of a touched instance is retracted, whatever its template, mapped or not, and the instance's recorded [changes](#net-changes) are dropped. The instance isolated by a [failed write](#failures) is evicted too, even if the cycle did not touch it. The rules' activations of those facts are gone with them.
3. **Reload, when the retry starts.** Before the next cycle's `begin` (after the reconnect when the connection was lost), each evicted instance that is still synchronized is reloaded through the [loader](#loading-rows): every mapped template of each package whose templates carried its `wf-id` (the facts retracted at the eviction and the facts the cycle touched), one `SELECT ... WHERE "wf_id" = $1` each, outside the cycle transaction, asserted without echo. Then the cycle runs as usual: the rules see the stored state and fire again.

What this means for each kind of fact of a touched instance:

| Fact | After the rollback |
|---|---|
| A mapped fact (`sync = auto` or `manual`) | as stored by the last committed cycle (or by another writer, D22); a modify of the failed cycle is undone, a retract comes back, an assert is gone |
| An unmapped fact (a template the mapping leaves out, also one asserted before the cycle) | lost: unmapped facts of an instance are **transient**, they live until the instance's next rollback |
| A fact drained from the ingest queue (admin `inject`, a plugin's `post_fact`) | **lost**, and not queued again: its instance is touched, so it is retracted with it. Durable inputs come through the exchange table (T13), whose rows stay `ASIGNADO` after a rollback |
| A fact never committed (a deffacts fact of a touched instance before the first commit) | lost, like any unmapped one |

The instances the cycle did not touch keep their facts and their recorded changes, which the retry writes (for example a deffacts fact whose first `INSERT` was part of the failed cycle).

An instance [isolated](#failures) by a write that failed for good is evicted and **not reloaded** (fail-closed: its stored rows cannot take the changes its rules make, so no rule runs on it again until the daemon restarts), and its eviction logs `clpdaemon: rehydrate: instance '<wf-id>' is not reloaded: it is no longer synchronized`. This holds also when a later rolled-back cycle touches an instance isolated earlier (its new facts, from the ingest queue for example, are never written): it is evicted again.

### Reload failures and the retry

The reload belongs to the retry of the [cycle transaction](database.md#cycle-transaction): no cycle runs before every evicted instance is reloaded or isolated.

| Situation | Messages | Then |
|---|---|---|
| A `SELECT` the database refuses (a missing table, a permission, a timeout) | `clpdaemon: rehydrate: instance '<wf-id>': <pkg>::<template>: table '<table>': <database message>`, then `clpdaemon: sync: instance '<wf-id>' is no longer synchronized: <database message>` | what that instance loaded is retracted, it is isolated (never reloaded nor written); the other instances are reloaded and the cycle runs |
| A `SELECT` that lost the connection (`CLPD_DB_LOST`) | the same `rehydrate:` line, then `clpdaemon: cycle transaction failed: <database message>; retrying in <n> ms` | what that instance loaded is retracted, the cycle does not run (no drain, no hooks, no count, like a failed `begin`); after the backoff the connection is reopened and the reload starts again with the instances not reloaded yet |
| PostgreSQL: `SELECT current_schema()` fails, or no schema of the `search_path` exists | `clpdaemon: rehydrate: <message>`, then the cycle transaction line | the same as a lost connection (nothing reloaded yet; reconnect first only when the connection was lost) |
| A row the loader rejects | its `load:` line ([above](#loading-rows)) | the other rows are asserted, the instance stays synchronized |

`<wf-id>` in the `rehydrate:` lines is shown with control characters as `?`, at most 200 bytes. For example, a rule that created its table and then failed rolls the creation back too, so the reload of the instance it touched fails with `clpdaemon: rehydrate: instance 'w2': tx@1::order: table 'tx_order': no such table: tx_order` and that instance is isolated.

### Decisions (T10c)

- Touched instances are those of the facts asserted, modified or retracted in the cycle (the ingest drain included), of any template; the instance of a failed write is added. Rules that fired without changing a fact touch nothing (their activations are spent; an open question for the DML functions of T10d and the exchange of T13).
- The facts are retracted at the rollback, and reloaded when the retry starts, outside the cycle transaction, so the reload shares the retry's backoff and reconnect and no rule runs on an instance that is neither reloaded nor isolated.
- Conservative interaction with the isolation of T10b: an isolated instance is evicted and never reloaded (fail-closed), rather than reloaded and run without being stored; a reload refused by the database isolates its instance too (it would fail on every retry), while a lost connection only delays the cycle.
- Changes of the instances the cycle did not touch are kept for the retry (T10b), not reloaded.
- A NULL in a column of null policy `error`, a value of another type, a value the slot refuses and a BLOB with a NUL byte reject the row (one line each), never the whole load.

## Interface

`src/orm.h` holds the CLIPS-free model and parser: `CLPD_ORM_SEQUENCE_TABLE` (the reserved `clpd_sequence`), `CLPD_ORM_INSTANCE_TABLE` (the reserved `workflow_instance`), `clpd_orm_sync` and `clpd_orm_sync_name` (each template's `sync`, `CLPD_ORM_SYNC_AUTO` by default), `clpd_orm_parse` / `clpd_orm_free`, `clpd_orm_column_of`, `clpd_orm_resolve_key`, `clpd_orm_valid_ident`, the type and policy names, `clpd_orm_backend_type` (the table above), `clpd_orm_type_fits` / `clpd_orm_type_fits_list` (the list written into a caller buffer of `CLPD_ORM_TYPE_LIST_SIZE` bytes, derived from the same table as `clpd_orm_type_fits`), `clpd_orm_template_of`, `clpd_orm_sequence_of` (T9c) and `clpd_orm_table_difference`. `src/orm_ddl.h` holds `clpd_orm_ddl_register` (all five functions, and the five DML functions of `src/orm_dml.h`), which `main.c` calls right after the database opens with a context (database, packages, `orm_allow_drop`, and the synchronization, which `main.c` sets after the attach) that lives as long as the CLIPS environment, and `clpd_orm_ddl_resolve_template` / `clpd_orm_ddl_fail` (the caller scope and the message format the DML functions share). `src/orm_dml.h` holds `clpd_orm_dml_register` (T10d). `src/orm_sync.h` also has `clpd_orm_sync_param` (the write rules for one value), `clpd_orm_sync_type_name` and `clpd_orm_sync_touch` (an instance the DML functions wrote). `src/orm_tx.h` holds the cycle transaction (`clpd_orm_tx_begin` / `clpd_orm_tx_end`, `clpd_orm_tx_abort` for a rollback that is no failed statement, the backoff `clpd_orm_tx_next_delay`), `clpd_orm_tx_current_schema` and `clpd_orm_tx_create_instance_table` ([`database.md`](database.md#cycle-transaction)). `src/orm_sync.h` holds the [automatic synchronization](#automatic-synchronization) and the [rehydration](#rehydration): `clpd_orm_sync_attach` / `clpd_orm_sync_detach` (the hooks; `main.c` attaches them after the packages load and before `after_load` and `Reset`), `clpd_orm_sync_suspend` / `clpd_orm_sync_resume` / `clpd_orm_sync_suspended` (echo suppression), `clpd_orm_sync_clear`, `clpd_orm_sync_begin_cycle` (the reload of the evicted instances and the begin, which replaces `clpd_orm_tx_begin` at the start of a cycle) and `clpd_orm_sync_end_cycle` (the flush and the commit, which replaces `clpd_orm_tx_end` at the end of a cycle, and the eviction after a rollback). `src/orm_load.h` holds the [loader](#loading-rows): `clpd_orm_load` (one template, equality filters) and `clpd_orm_load_instance` (every mapped template of a package for one instance). `src/orm_tx.h` also has `clpd_orm_tx_connect` (the reconnect alone), `clpd_orm_tx_fail` (a failed attempt outside the transaction) and `clpd_orm_tx_schema_status` (`clpd_orm_tx_current_schema` with the query's status). After a clean load, `clpd_package.orm` holds the mapping, `clpd_package.orm_templates` the deftemplates in mapping order, and each template's `key_columns` the indexes of `wf-id` and the local key columns.

### Decisions (T10a)

- `<tpl>.sync = auto|manual`, default `auto` (D7); parsed and checked at load, exposed as `clpd_orm_template.sync`; the automatic synchronization itself is T10b. It is not part of the table, so versions sharing a table may differ in it.
- `workflow_instance` is a reserved table name on every backend, like `clpd_sequence`; since T10b it is a reserved sequence name too (L91).
- The DDL and sequence functions run inside the cycle transaction when a rule calls them: a rolled-back cycle undoes a create or a drop on both backends (tested), a PostgreSQL `nextval` stays used, a SQLite counter row is rolled back.
