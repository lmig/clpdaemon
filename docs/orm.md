# ORM mapping

A rule package can map its deftemplates to database tables and name the database sequences its rules use (S7, S11, FR-22). The mapping lives in a separate file named by the manifest key `orm` ([`packages.md`](packages.md#manifest)). Nothing in it is implicit: every slot of a mapped template gets its own line with the column, the SQL type, the CLIPS type and what a NULL becomes.

The mapped templates of one workflow definition form its aggregate (FR-22); their tables carry the persistent state of each workflow instance. This page describes the file and the checks made when the package loads (T9a). The DDL functions (`db-create-table`, `db-check-schema`, `db-drop-table`, T9b), `db-nextval` (T9c) and the per-cycle synchronization (T10) build on this model.

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

The SQL type is logical; each backend has its own column type (T9b). SQLite only stands in for PostgreSQL in tests, and its names never get NUMERIC affinity, so values come back as stored.

| `sql-type` | PostgreSQL | SQLite |
|---|---|---|
| `bigint` | `BIGINT` | `INTEGER` |
| `double` | `DOUBLE PRECISION` | `REAL` |
| `text` | `TEXT` | `TEXT` |
| `bytea` | `BYTEA` | `BLOB` |
| `boolean` | `BOOLEAN` | `INTEGER` |
| `timestamptz` | `TIMESTAMPTZ` | `TEXT` |
| `jsonb` | `JSONB` | `TEXT` |
| `numeric` | `NUMERIC` | `TEXT` |

### CLIPS types

`clips-type` is the type the slot value has in the fact: `integer`, `float`, `string` or `symbol`. The slot's own type constraint must allow it (`(slot n (type INTEGER))` cannot be mapped as `string`; a slot without a `type` attribute allows all four; `LEXEME` allows `string` and `symbol`, `NUMBER` allows `integer` and `float`).

### Null policies

What a NULL column becomes when a row is loaded into a fact (T10):

| `null` | Effect |
|---|---|
| `nil` | the slot gets the symbol `nil` |
| `default` | the slot gets its deftemplate default |
| `error` | the row is rejected; required for key slots |

## Loading

The mapping is read after the package's files, the `wf-id` lint and the `ingest` check ([`packages.md`](packages.md#loading)), at startup and under `--check`. A package with `orm` needs a database: without the `database` key in the configuration ([`database.md`](database.md)) it is an error. The checks are:

1. The file itself: syntax, unknown or repeated keys, values, required keys, null policy of key slots, a column, table or sequence used twice in the file.
2. A configured `database`.
3. Each mapped template, in file order: it is a deftemplate of the package; it has a `wf-id` slot; each slot line names a slot of the template whose type constraint allows the declared CLIPS type; each key slot is a slot of the template; every slot of the template is mapped and none is a multislot.
4. Tables, then sequences, already mapped by an earlier package of another definition. Versions of one definition (`shop@1` and `shop@2`) may map the same tables and sequences; two definitions may not. A package whose mapping file is valid claims its tables and sequences even if a later check failed. The mapping is not read when a file of the package fails to load (step 2 of [`packages.md`](packages.md#loading)).

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
| Table of another definition | `clpdaemon: <pkg>: orm: table '<table>' is already mapped by <other pkg>` |
| Sequence of another definition | `clpdaemon: <pkg>: orm: sequence '<name>' is already mapped by <other pkg>` |

`<file>` is the mapping path as the manifest resolves it (`<package dir>/<orm>`); `<pkg>` is `definition@version`. Order: the file's line errors in file order, then its file-level errors (per template in order of first appearance: missing `table`, missing `owned`, key slots with another null policy in line order; then per sequence: missing name, missing `owned`), then the database message, then the template checks of step 3 and the ownership checks of step 4. When the file has errors, steps 3 and 4 are skipped.

## Interface

`src/orm.h` holds the CLIPS-free model and parser: `clpd_orm_parse` / `clpd_orm_free`, `clpd_orm_column_of`, `clpd_orm_resolve_key`, `clpd_orm_valid_ident`, the type and policy names, and `clpd_orm_backend_type` (the table above). After a clean load, `clpd_package.orm` holds the mapping, `clpd_package.orm_templates` the deftemplates in mapping order, and each template's `key_columns` the indexes of `wf-id` and the local key columns.
