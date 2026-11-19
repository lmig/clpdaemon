# ORM mapping

A rule package can map its deftemplates to database tables and name the database sequences its rules use (S7, S11, FR-22). The mapping lives in a separate file named by the manifest key `orm` ([`packages.md`](packages.md#manifest)). Nothing in it is implicit: every slot of a mapped template gets its own line with the column, the SQL type, the CLIPS type and what a NULL becomes.

The mapped templates of one workflow definition form its aggregate (FR-22); their tables carry the persistent state of each workflow instance. This page describes the file, the checks made when the package loads (T9a, T9b) the [DDL functions](#ddl-functions) `db-create-table`, `db-check-schema` and `db-drop-table` (T9b, T9d, FR-23) and the [sequence functions](#sequence-functions) `db-create-sequence` and `db-nextval` (T9c). The per-cycle synchronization (T10) builds on this model.

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
| `seq.<alias>` | name of the database sequence, an [identifier](#identifiers); `<alias>` is the name rules use (`(db-nextval order_no)`, [below](#sequence-functions)) and is an identifier too | required |
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

With a `database` in the configuration the core defines five CLIPS functions (FR-23, D9): the three table functions below and the two [sequence functions](#sequence-functions), before the plugins and packages load; without one they do not exist, so a package calling them fails to load (`Missing function declaration for 'db-create-table'`). Every statement they send runs in autocommit through the database interface, at most one `CREATE` or `DROP` per call ([`database.md`](database.md)): the functions never send `BEGIN` or `COMMIT` and never `ALTER` a table. The SQL holds only the table, column and type names of the mapping and, on PostgreSQL, the name of the current schema, always quoted (`"public"."shop_order"`); the catalog lookups pass the table name as a parameter (`= $1`).

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

A rule draws numbers from the database sequences its package maps (`seq.<alias>`, [above](#sequences)) by alias (T9c, L82). Both functions follow the rules of the [DDL functions](#ddl-functions): they exist only with a `database`, only a rule of a rule package may call them, only for the aliases of its own definition, and every statement runs in autocommit.

| Function | Returns | Effect |
|---|---|---|
| `(db-create-sequence <alias>)` | `TRUE`, or `FALSE` on error | Creates an owned sequence (`seq.<alias>.owned = yes`) when it does not exist; an existing sequence keeps its value (`TRUE`). A sequence that is not owned is never created. |
| `(db-nextval <alias>)` | the next value, an INTEGER (`1`, `2`, `3`, ... for a sequence the daemon created); `FALSE` on error | Advances the sequence in one statement and returns the new value. The sequence is shared: versions of one definition that map the same sequence, and other daemons or connections on the same database, never get the same value. A value drawn is used up even when the rule's later actions fail (the statement runs in autocommit, and PostgreSQL sequences are not transactional anyway). |

`<alias>` is a symbol or a string, resolved like a template name for `db-create-table` ([template names](#template-names)): `<package>::<alias>` names the alias of that package, which must be a version of the calling rule's definition; an unqualified alias is the calling rule's package's alias when that package maps it, otherwise the alias of the only version of the definition that maps it; several versions mapping it (even to the same sequence) ask to qualify it.

### Generated SQL

| | PostgreSQL | SQLite |
|---|---|---|
| `db-create-sequence` | `SELECT current_schema()`, then `CREATE SEQUENCE IF NOT EXISTS "<schema>"."<name>"` (PostgreSQL's defaults: `bigint`, start 1, increment 1) | `CREATE TABLE IF NOT EXISTS "clpd_sequence" ("name" TEXT PRIMARY KEY, "value" INTEGER NOT NULL)`, then `INSERT INTO "clpd_sequence" ("name", "value") VALUES ($1, 0) ON CONFLICT ("name") DO NOTHING` |
| `db-nextval` | `SELECT current_schema()`, then `SELECT nextval($1)` with `"<schema>"."<name>"` as the TEXT parameter (no cast in the SQL text) | `UPDATE "clpd_sequence" SET "value" = "value" + 1 WHERE "name" = $1 AND "value" < 9223372036854775807 RETURNING "value"` |

On PostgreSQL the sequence lives in `current_schema()`, like the tables: a same-named sequence of a later `search_path` schema is never created over or advanced. SQLite has no sequences, so the counter table `clpd_sequence` stands in for them (tests only, [`database.md`](database.md#sqlite-stand-ins)): one row per sequence name, `value` the last value returned (0 before the first), advanced by one `UPDATE ... RETURNING` statement (SQLite 3.35 or later). Do not map a table named `clpd_sequence` on SQLite. When a call fails or returns no value, the function looks the sequence up (PostgreSQL: a relation of that name in `current_schema()` in `pg_catalog.pg_class`; SQLite: the counter table and the row) to tell a missing sequence from another error. The largest value is 9223372036854775807 (INT64): on SQLite the guard leaves a row at the maximum unchanged and the call is an error; on PostgreSQL the sequence's own `MAXVALUE` applies and the call fails with PostgreSQL's message.

### Sequence errors

The same log line, CLIPS evaluation error and `FALSE` as the [DDL errors](#errors); nothing is created or advanced.

| Situation | Message |
|---|---|
| Not called from the actions of a rule of a rule package | the two `only a rule of a rule package may call it` messages of the [DDL errors](#errors) |
| No loaded package maps the alias | `clpdaemon: <udf>: unknown sequence alias '<alias>' (no loaded package maps it)` |
| An alias only other definitions map (for a qualified alias, the package named) | `clpdaemon: <udf>: sequence alias '<alias>' is not mapped by definition <definition> of the calling rule (mapped by <pkg>[, <pkg>...])` |
| Unqualified alias mapped by several versions of the calling rule's definition | `clpdaemon: <udf>: sequence alias '<alias>' is mapped by <pkg>, <pkg>[, ...]: qualify it as <package>::<alias>` |
| `db-create-sequence` on a sequence that is not owned | `clpdaemon: db-create-sequence: sequence alias '<alias>': sequence '<name>' is not owned (owned = no), so it is never created` |
| `db-nextval` on a sequence that does not exist (SQLite: no counter table or no row) | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>' does not exist` |
| SQLite: the counter is at 9223372036854775807 | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>' has reached its maximum value` |
| PostgreSQL: no schema of the `search_path` exists | `clpdaemon: <udf>: sequence alias '<alias>': sequence '<name>': no current schema (the search_path names no existing schema)` |
| The database refuses a statement (PostgreSQL's maximum included: `nextval: reached maximum value of sequence "?" (9223372036854775807) (SQLSTATE 2200H)`, the name hidden like every data exception, [`database.md`](database.md#status)) | `clpdaemon: <udf>: sequence alias '<alias>': sequence '<name>': <database message>` |
| SQLite: the counter row holds no INTEGER (written by someone else) | `clpdaemon: db-nextval: sequence alias '<alias>': sequence '<name>': the database returned no INTEGER` |

`<alias>` is the argument as given (control characters shown as `?`, at most 200 bytes). The checks run in the order of the table: the caller, the alias, ownership, then the statements.

### Decisions (T9c)

- `db-nextval` returns an INTEGER and takes an alias of the calling rule's own definition, any version, resolved like `db-create-table`; it is refused outside a rule (fail-closed).
- Owned sequences are created by the separate `db-create-sequence`, not by `db-create-table`, with the same caller scope; a sequence that is not owned is never created.
- PostgreSQL: `CREATE SEQUENCE IF NOT EXISTS` and `nextval($1)` on the name qualified with `current_schema()`, the name passed as a TEXT parameter; SQLite: the counter table `clpd_sequence(name TEXT PRIMARY KEY, value INTEGER NOT NULL)` with one row per name, starting so that the first value is 1, like PostgreSQL.
- Errors as the other DDL functions; a missing sequence and an INT64 overflow are errors (SQLite guards in the statement, so the row is not changed; PostgreSQL reports its own error); error messages never show a value from the database.

## Interface

`src/orm.h` holds the CLIPS-free model and parser: `clpd_orm_parse` / `clpd_orm_free`, `clpd_orm_column_of`, `clpd_orm_resolve_key`, `clpd_orm_valid_ident`, the type and policy names, `clpd_orm_backend_type` (the table above), `clpd_orm_type_fits` / `clpd_orm_type_fits_list` (the list written into a caller buffer of `CLPD_ORM_TYPE_LIST_SIZE` bytes, derived from the same table as `clpd_orm_type_fits`), `clpd_orm_template_of`, `clpd_orm_sequence_of` (T9c) and `clpd_orm_table_difference`. `src/orm_ddl.h` holds `clpd_orm_ddl_register`, (all five functions), which `main.c` calls right after the database opens with a context (database, packages, `orm_allow_drop`) that lives as long as the CLIPS environment. After a clean load, `clpd_package.orm` holds the mapping, `clpd_package.orm_templates` the deftemplates in mapping order, and each template's `key_columns` the indexes of `wf-id` and the local key columns.
