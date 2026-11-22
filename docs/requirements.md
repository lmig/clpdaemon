# clpdaemon — Requirements Specification

| | |
|---|---|
| Status | Draft 0.1 — decisions D1–D30 resolved, pending implementation |
| Engine | CLIPS 6.4.2 (https://www.clipsrules.net/), embedded, unmodified |
| Database | PostgreSQL (production), SQLite (tests only) |
| Language / build | C99, CMake |
| Task plan | `odd/tasks/clpdaemon.md` |
| Exchange contract | `docs/exchange-contract.md` |

## 1. Purpose and scope

clpdaemon is a daemon that executes **workflows coded as CLIPS rule packages**. Each workflow instance has a unique identifier, persists its state across several database tables, and interacts with external systems (CRM, fibre provider, installer, inventory, network, ...) only through a single database **exchange table** (inbox/outbox). Several daemons can run side by side; a **dispatcher** decides which node processes each inbound message.

**In scope:** the daemon (engine node), its plugins (ORM, ZeroMQ), the dispatcher, the exchange contract document, a simulated reference connector, operations documentation.

**Out of scope (later phase):** the real connectors (gateways) to external systems. This project only delivers their contract (D26).

## 2. Glossary

| Term | Meaning |
|---|---|
| Node | One running clpdaemon process, owning one CLIPS environment. |
| Cycle | One engine execution: read assigned inbound messages, assert facts, `Run`, write state and outbound messages, `COMMIT`. |
| Workflow definition | A rule package, identified as `definition@version` (e.g. `fibre-installation@v3`). |
| Workflow instance | One execution of a definition, identified by `wf_id`. |
| Aggregate | The set of tables holding the state of one workflow definition, all keyed by `wf_id`. |
| Exchange table | `mensaje_intercambio`: the single table for inbound (`IN`) and outbound (`OUT`) messages. |
| Connector | External autonomous component that translates one external system's protocols to and from the exchange table. |
| Dispatcher | Component that correlates inbound messages, creates instances and assigns messages to nodes. |
| Rehydration | Rebuilding an instance's facts in a node's working memory from the database. |

## 3. Architecture overview

```
 External systems     Connectors (later phase,          PostgreSQL                      Core
 (CRM, provider,      any language, one per system)  ┌────────────────────────┐
  installer, ...)                                    │ mensaje_intercambio    │◄── clpd-dispatch
       ◄──────────►   connector-x  ────────────────► │   IN / OUT             │    (correlate, create
                                                     │ workflow_instance      │     instance, assign,
                                                     │ correlacion            │     notify via ZeroMQ)
                                                     │ aggregate tables       │          │
                                                     └────────────────────────┘          ▼
                                                                ▲             clpdaemon node 1..N
                                                                └──────────── (consume IN, Run, write
                                                                               state + OUT, one COMMIT)
```

Key constraint (CLIPS APG 6.4.2 §1.2): *"Each environment can have at most one thread of execution."* Every node runs a single-threaded event loop that owns its CLIPS environment; other threads (e.g. plugin receivers) only hand data to it through a thread-safe queue.

## 4. Functional requirements

### 4.1 Daemon and engine core
- **FR-1** The daemon runs in the foreground under systemd (`Type=notify`); it does not double-fork. `--foreground` is the default; `--config <file>` selects the configuration; `--check` validates configuration and packages and exits. (S1)
- **FR-2** Startup order: read configuration → open trace → create environment → load plugins → load rule packages → `Reset` once → enter event loop; with a database, instances are rehydrated on demand when an input for one arrives (FR-9, T11d), not eagerly at startup. Any load error aborts startup with a non-zero exit code and a message naming the file and the error. (S1, S2)
- **FR-3** A cycle runs (a) when a notification arrives, no sooner than `cycle_min_interval_ms` after the previous cycle, and (b) every `tick_interval_ms`. Setting both to the same value yields a pure periodic tick. (S3, D27)
- **FR-4** Each cycle calls `Run` with a configurable `max_rules_per_cycle` limit (`-1` = unlimited). `Reset` is never called per cycle. (S3)
- **FR-5** `SIGTERM`/`SIGINT` stop the daemon gracefully: an engine periodic function sets `SetHaltRules`, the current cycle finishes or rolls back, plugins shut down, the environment is destroyed. `SIGHUP` reopens the trace files. (S1)

### 4.2 Rule packages and workflows
- **FR-6** A rule package is a directory with a manifest naming the definition, version and ordered `.clp` files. Each package is a `defmodule` named `definition@version`. (S2, S10)
- **FR-7** Every deftemplate of a workflow package has a `wf-id` slot. At load time a lint step reports templates without `wf-id` and rules whose patterns do not join on the same `?wf-id` (patterns on the core's clock fact `clpd-clock`, FR-10, are exempt); lint errors are fatal unless the package declares an explicit exemption. (S10)
- **FR-8** The core provides lifecycle functions `(wf-complete ?wf-id)` and `(wf-fail ?wf-id ?reason)`: they persist the final state and retract all facts of the instance. (S10, D4)
- **FR-9** Instances without activity for `idle_eviction_after` are evicted from working memory and rehydrated from the database when a message for them arrives. (D4, D18)
- **FR-10** The core maintains a clock fact updated every cycle so rules can express deadlines and timeouts. (S10)
- **FR-11** (v4) Several versions of the same definition may be loaded at once; an instance stays on the version it started with; new instances use the current version. (D20)
- **FR-12** (v4) An instance can start subflows and wait for them through internal `IN`/`OUT` messages carrying the parent `wf_id`. (D19)

### 4.3 Tracing
- **FR-13** An I/O router captures the logical names `STDOUT`, `STDERR` and `STDWRN` and writes them to the trace file with timestamp and logical name. (S5)
- **FR-14** Facts asserted, modified and retracted and rules fired are traced through `Watch(FACTS)`/`Watch(RULES)` output and through structured callbacks (`AddAssertFunction`, `AddRetractFunction`, `AddModifyFunction`, `AddBeforeRuleFiresFunction`). (S5)
- **FR-15** Output formats: human-readable text and JSON Lines, selectable by configuration, both possible at once. Every line carries `node_id` and, when known, `wf_id`. (S5, D3)
- **FR-16** Plugin writes to the process `stdout`/`stderr` file descriptors are redirected to the trace. (S5)

### 4.4 C plugins
- **FR-17** Plugins are shared libraries loaded with `dlopen` from the configured list before the rule packages. Each exports one symbol, `clpd_plugin_get`, returning a descriptor with an ABI version, `init` and optional hooks (`after_load`, `cycle_begin`, `cycle_end`, `on_ack`, `shutdown`). An ABI mismatch or a failing `init` aborts startup. (S6)
- **FR-18** Plugins register CLIPS functions with `AddUDF` following the CLIPS `UserDefinedFunction` contract. Plugins do not link CLIPS themselves; the daemon exports CLIPS symbols (`-rdynamic`). Plugins are never unloaded while the environment exists. (S6)
- **FR-19** The host API given to plugins offers `post_fact` (callable from any thread; enqueues for the engine thread) and `log` (engine thread only). (S6, S8)

### 4.5 Admin socket and ingest pipeline
- **FR-20** A Unix domain socket (permissions from configuration) accepts line-delimited commands: inject a test fact, report status, reopen trace. It is not exposed to the network. (S4, D1)
- **FR-21** All direct fact sources (admin socket, pub/sub plugin) use one ingest pipeline: bounded queue → engine thread → validation of the JSON payload against the deftemplate whitelist and size limits → `FactBuilder`. `Eval`/`Build` are never reachable from outside. (S4, S8, D2)

### 4.6 Persistence (ORM plugin)
- **FR-22** A mapping file declares, per deftemplate, the table, the key (composite `wf_id` + local key) and the column mapping; existing tables use explicit column mapping, new tables can be generated. Aggregates group the templates of one workflow definition. (S7, S11, D21)
- **FR-23** DDL functions: `(db-create-table <template>)` (`CREATE TABLE IF NOT EXISTS`, only for tables marked as owned), `(db-check-schema <template>)` (returns differences), `(db-drop-table <template>)` (disabled unless enabled in configuration). No automatic `ALTER`. (S7, D9)
- **FR-24** DML functions: `db-insert`, `db-update`, `db-delete`, `db-load` (equality filters, asserts results), `db-count`. Only parameterized statements; table and column names come only from the mapping. No raw-SQL function. (S7)
- **FR-25** Automatic synchronization per aggregate: changes to mapped facts during a cycle are written in the cycle transaction. Facts asserted while loading from the database are not written back. (S7, D7)
- **FR-26** If the cycle transaction fails, it is rolled back, the working memory of the affected instances is discarded and rehydrated, their inbound messages stay `ASIGNADO`, and the cycle is retried with exponential backoff. (D8)
- **FR-27** Changes made to aggregate tables by processes other than the engine must reach the engine as `IN` messages (written by those processes or by database triggers into the exchange table). (D22)

### 4.7 Exchange and notification
- **FR-28** All interaction with external systems goes through the single table `mensaje_intercambio` defined in `docs/exchange-contract.md` (contract v1.0, T12: DDL, states, envelope and per-type JSON Schemas under `docs/schemas/`). The engine never calls an external system. (S12, S13)
- **FR-29** In one cycle transaction a node reads its `ASIGNADO` rows ordered by `id`, asserts them as facts (the `data` of the envelope mapped to the slots of the deftemplate named by `tipo_mensaje`, with the slot's declared CLIPS types), runs, writes aggregate state and `OUT` rows, and marks the `IN` rows `CONSUMIDO`, or `ERROR` with the reason in `error` when `data` does not map; `ERROR` is terminal until an operator puts the row back to `RECIBIDO` (exchange contract §4.3, §9; L117). (S12, S13)
- **FR-30** Deduplication by `UNIQUE(endpoint_logico, direccion, msg_id_externo)` (`INSERT ... ON CONFLICT DO NOTHING`); responses correlate by `correlacion_id` with an `OUT` row of the same `endpoint_logico`, unsolicited events by `(endpoint_logico, ref_externa)` through `correlacion`; rows that cannot be correlated are retried for a configurable window (default 10 min) and then stay `SIN_CORRELACION` for an operator. `OUT` delivery is at least once, claimed with a lease and retried per `endpoint_logico` (defaults 8 attempts, backoff 1 s to 1 h with jitter); after the last attempt the connector writes the dead letter and a `delivery-failed` `IN` message (exchange contract §7, §8). (S13)
- **FR-31** The dispatcher notifies the node over ZeroMQ **after** its `COMMIT`. Notifications carry only hints; nodes also poll their assigned rows every `safety_poll_interval_ms`. A lost notification never loses data. Connectors may send an optional ZeroMQ wake-up hint to the intake after their `COMMIT`; socket types, endpoints and frames are fixed in the exchange contract §10. (S14)
- **FR-32** The ZeroMQ plugin also supports direct pub/sub fact injection for non-durable sources through the ingest pipeline (FR-21). Durable external messages always use the exchange table. (S8)
- **FR-33** Inbound synchronous interfaces are handled by connectors: queries read the database; commands are answered `202 Accepted` with the `id` of the inserted `IN` row (the `wf_id` is assigned later by the dispatcher and is readable by a later query on that row) and the outcome is delivered by callback or later query. (D25, L118)

### 4.8 Distribution (v2) and resilience (v3)
- **FR-34** Each node has a `node_id`, belongs to groups, and sends a heartbeat (ZeroMQ) with load metrics (queue length, last cycle duration, fact count, agenda size) and loaded `definition@version` list. (S9)
- **FR-35** The dispatcher correlates `ref_externa` → `wf_id`, creates instances for start-type messages, and assigns messages: existing instance → owner node (affinity); new instance → configured policy among nodes that have the definition loaded: direct, group, broadcast, round-robin, least-loaded (pull-based). (S9)
- **FR-36** The dispatcher runs active/passive with a database leader lock; it keeps no state outside the database. (S9)
- **FR-37** (v3) Each instance has an owner node and a lease; every write checks ownership (fencing). When a node's lease expires, the dispatcher reassigns its instances and pending messages; the new owner rehydrates them. (S9, D15)

## 5. Non-functional requirements
- **NFR-1 Concurrency:** one thread per CLIPS environment; no CLIPS call from any other thread.
- **NFR-2 Security:** admin socket local only; no remote code evaluation path; parameterized SQL only; plugins loaded only from configured paths; database credentials never logged; each connector logs in with its own role and, through column grants and row-level security, reads and changes only the exchange rows of its own `endpoint_logico` values and never sets `wf_id` (exchange contract §3.5).
- **NFR-3 Consistency:** state change, consumption of inbound messages and production of outbound messages are atomic per cycle (single PostgreSQL database, D23).
- **NFR-4 Observability:** per-instance audit trail from the trace (`wf_id`) and the exchange table; metrics exposed by nodes and dispatcher.
- **NFR-5 Capacity:** volumes unknown (D14/D29). The design must scale horizontally; capacity is established by load tests (T23); exchange table retention and partitioning are configurable and decided in T23, keeping the columns and deduplication behaviour of exchange contract v1.0 (§3.3).
- **NFR-6 Portability:** Linux (systemd). CLIPS sources vendored unmodified.
- **NFR-7 Testability:** every behavior covered by automated tests through public interfaces (CLI, socket, database, trace files); SQLite backend for fast tests, PostgreSQL for integration tests.

## 6. Data model (summary)

| Table | Owner | Purpose |
|---|---|---|
| `workflow_instance` | engine | `wf_id` PK, definition, version, status, parent `wf_id`, owner node, lease, timestamps |
| `mensaje_intercambio` | shared contract | inbound and outbound messages (see exchange contract) |
| `correlacion` | dispatcher/engine | `(endpoint_logico, ref_externa)` (primary key) → `wf_id`; DDL in the exchange contract §3.4 |
| `conector_endpoint` | operations (table owner) | `(rol, endpoint_logico)`: the endpoints each connector role may read and change, used by the row-level security policies; exchange contract §3.5 |
| aggregate tables | engine (new) or existing systems (mapped) | per-workflow business state, keyed by `wf_id` + local key |

## 7. Configuration (initial keys)

```ini
node_id                  = n1
tick_interval_ms         = 5000
cycle_min_interval_ms    = 200
max_rules_per_cycle      = 10000
safety_poll_interval_ms  = 5000
idle_eviction_after      = 30m
admin_socket             = /run/clpdaemon/admin.sock
trace_text               = /var/log/clpdaemon/trace.log
trace_jsonl              = /var/log/clpdaemon/trace.jsonl
watch                    = facts,rules
packages                 = /etc/clpdaemon/packages/fibre-installation@v3
plugins                  = /usr/lib/clpdaemon/clpd-orm.so, /usr/lib/clpdaemon/clpd-zmq.so
```

## 8. Decision record

| ID | Decision |
|---|---|
| D1 | Daemon socket: Unix domain socket for administration and tests only. |
| D2 / D28 | Payload: canonical JSON validated against deftemplates, asserted with `FactBuilder`; original payload optionally kept. |
| D3 | Trace: text and JSON Lines. |
| D4 | Facts retracted when the instance ends; idle instances evicted and rehydrated on demand. |
| D5 | C99 + CMake. |
| D6 | PostgreSQL in production; SQLite for tests. |
| D7 | Automatic synchronization per aggregate, one transaction per cycle. |
| D8 | DB failure in a cycle: rollback, discard and rehydrate affected instances, retry with backoff. |
| D9 | No `DROP` by default, no automatic `ALTER`. |
| D10–D12 | Delivery guaranteed by the exchange table; outputs only through `OUT`; deduplication by unique key. |
| D13 | Affinity by `wf_id`; new instances distributed by policy. |
| D14 / D29 | Volumes unknown: design to scale, size with load tests, configurable retention. |
| D15 | Instance lease + reassignment + rehydration. |
| D16 | Incremental deliveries v1–v4. |
| D17 | Two identifiers: `definition@version` and instance `wf_id`. |
| D18 | Short-lived and long-lived workflows coexist. |
| D19 | Subflows supported. |
| D20 | Several versions of a definition coexist. |
| D21 | Aggregate tables: mix of existing and new tables. |
| D22 | Other processes also write aggregate tables; their changes reach the engine as `IN` messages. |
| D23 | All tables in the same PostgreSQL database. |
| D25 | Inbound synchronous calls: `202` + callback or query; queries read the database. |
| D26 | Connectors: only the contract now; construction in a later phase. |
| D27 | Hybrid cycle: on notification with minimum interval, plus periodic tick. |
| D30 | Exchange contract as a separate document. |
| — | Notification: ZeroMQ, with safety polling. |

## 9. Open items and assumptions
- **A1** Mechanism for other writers (D22): triggers on aggregate tables versus explicit `IN` messages written by those processes. To be decided per table in T13.
- **A2** Exact CLIPS slot-introspection function names (APG §12.3.5) to be confirmed against headers in T9.
- **A3** Rule-package lint exemptions format (FR-7): resolved in T4 as a `lint_exempt` list in the package manifest (`docs/packages.md`).
- **A4** Resolved: `clpdaemon` is its own monorepo (hosted on the owner's personal GitHub); layout in `README.md`.

## 10. Delivery plan

| Delivery | Phases | Tasks |
|---|---|---|
| v1 single node | 1 foundations · 2 observability · 3 plugins · 4 admin socket · 5 persistence · 6 exchange | T1–T17 |
| v2 distribution | node identity, dispatcher, dispatcher HA | T18–T20 |
| v3 resilience | lease and fencing, failover, load tests | T21–T23 |
| v4 advanced workflows | subflows, version coexistence | T24–T25 |
| Later | real connectors | — |

Task details, status and commit evidence: `odd/tasks/clpdaemon.md`.
