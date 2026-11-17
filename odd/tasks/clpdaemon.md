# clpdaemon — CLIPS workflow engine daemon

Autonomous CLIPS 6.4.2 daemon that executes workflows coded as rule packages, persisted in PostgreSQL, isolated from external systems through a single exchange table (inbox/outbox), and scalable across several nodes through a dispatcher.
Full requirements: `docs/requirements.md`. Exchange contract for connector teams: `docs/exchange-contract.md`. Decisions D1-D30: `docs/requirements.md` §8.

## Specs

- **S1** — Daemon executable: "Será un ejecutable en modo demonio".
- **S2** — Rule packages at startup: "sea capaz de cargar paquetes de reglas a la hora de lanzarse".
- **S3** — Periodic run: "sea capaz de cada x segundos configurables ejecutar su run". Decision D27: "Híbrido configurable" (cycle on notification with a configurable minimum interval, plus the periodic tick).
- **S4** — Socket ingestion: "sea capaz de ingerir hechos desde el exterior a través de la conexión por socket". Decision D1: "Unix, admin y pruebas".
- **S5** — Tracing: "sea capaz de trazar a un fichero los hechos que va generando y las reglas que va ejecutando y la salida estandar o de error". Decision D3: "Ambos" (text and JSON Lines).
- **S6** — C plugins: "sea capaz de cargar funciones de una librería dinámica escrita en C de acuerdo al contrato que CLIPS tiene para hacer interfaz con el lenguaje C". Source: "Utiliza para ello las fuentes y documentación que se encuentran en https://www.clipsrules.net/".
- **S7** — ORM: "un sistema ORM hechos CLIPS <-> tablas de BD con plugin para gestión básica de DDL y DML de BD". Decision D6: "PostgreSQL".
- **S8** — Pub/sub plugin: "un plugin para la inyección de hechos al motor modo publish/subscriber, ya sea con 0MQ u otras soluciones".
- **S9** — Scalability and dispatching: "la posibilidad de alta escalabilidad y dispatching de hechos de los canales externos sobre un demonio en concreto, sobre varios, sobre el demonio menos cargado, en round-robind, etc".
- **S10** — Workflows: "el sistema se va a construir para la ejecución de flujos de trabajo codificados en paquetes de reglas clips; cada flujo de trabajo llevará un identificador único".
- **S11** — Multi-table persistence: "la persistencia de un flujo de trabajo puede estar diseminada en varias tablas de la BD". Decision D21: "Mezcla" (existing and new tables). Decision D22: "También otros procesos" write to those tables.
- **S12** — Inbox/outbox isolation: "aislar el núcleo del motor (demonio) mediante patrones inbox y outbox que traten las diferentes interfaces (Colas -petición/respuesta asíncrona-, SOAP y REST -con petición/respuesta asíncrona/síncrona-, etc)".
- **S13** — Single exchange table and autonomous gateways: "solo una tabla en BD de intercambio de mensajería independientemente del gateway, protocolo, etc"; "separar gateways en componentes autónomos por protocolo y lenguaje"; dedicated readers for inbound and outbound messages.
- **S14** — Notification: "vamos a usar ZeroMQ para el aviso con el sondeo de seguridad".
- **S15** — Gateway scope: "Solo el contrato, dejando para una fase posterior del proyecto la construcción de los mismos". Decision D30: "Sí, documento aparte".
- **S16** — Workflow characteristics: D18 "Mezcla de ambos" (short and long-lived); D19 "Sí, con subflujos"; D20 "Sí, convivencia" of versions; D4 "Retirar + desalojar inactivas".
- **S18** — T2 configuration decisions (L17): "--config obligatorio"; unknown keys: "Error (Recomendado)"; required keys: "Solo node_id (Recomendado)"; error format: "clpdaemon: fichero:línea: mensaje". Exact messages in `docs/configuration.md`.
- **S17** — Delivery: D16 "Incrementales" (v1 single node, v2 dispatcher and nodes, v3 lease/recovery/rehydration, v4 subflows and advanced versioning). D14/D29: "No lo sé aún" (design to scale, size with load tests, configurable retention).

## Tasks

Route: `inline` unless noted. Risk items refer to the high-risk list (1 data, 2 security, 3 contracts, 4 concurrency, 5 delivery, 6 untestable). Commit: filled in when the task lands.

### v1 — Single node

Phase 1 — Foundations
- [x] **T1** (S1, S2, S6) Repository skeleton, CMake, vendored CLIPS 6.4.2 core (unmodified) built as static `-fPIC` library, embed smoke test. Route: inline. Risk: item 5. Commit: f8657ba (vendor import), 12d33dc (build + test)
- [x] **T2** (S1, S3, S18) Configuration file + CLI (`--config`, `--foreground`, `--check`), validation with exact error messages. Route: inline. Risk: none (new flags and file). Branch: `feat/t2-config` (stacked on `feat/t1-skeleton`). Commit: cf08663
- [ ] **T3** (S1, S3) Event loop (`poll` + `timerfd` + `signalfd`), hybrid cycle scheduler (minimum interval + tick), graceful shutdown with periodic halt check (`AddPeriodicFunction` + `SetHaltRules`). Route: inline. Risk: item 4. Commit: —
- [ ] **T4** (S2, S10) Rule package loader: package = `defmodule` named `definition@version`, manifest, fatal `LoadError` reporting, `wf-id` lint (templates carry `wf-id`, rules join on `?wf-id`). Route: inline. Commit: —

Phase 2 — Observability
- [ ] **T5** (S5) Trace: router for `STDOUT`/`STDERR`/`STDWRN`, `Watch` facts/rules, structured callbacks (assert/retract/modify, before/after rule fires), text and JSONL, `wf_id` and `node_id` on every line, `SIGHUP` reopen, plugin `stdout`/`stderr` redirection. Route: inline. Commit: —

Phase 3 — C plugins
- [ ] **T6** (S6) Plugin contract v1 (`clpd_plugin_get`, ABI version, host API, lifecycle hooks), `dlopen` loader, `-rdynamic` linking, example UDF plugin with tests. Route: inline. Risk: item 3. Commit: —

Phase 4 — Admin socket and ingest pipeline
- [ ] **T7** (S4) Common ingest pipeline (bounded queue + `eventfd`, JSON → `FactBuilder`, deftemplate whitelist, size limits) and Unix admin socket (inject test facts, status, reopen trace). Route: inline. Risk: item 2. Commit: —

Phase 5 — Persistence (ORM)
- [ ] **T8** (S7) DB backend interface + SQLite backend (tests) + PostgreSQL backend (`libpq`), parameterized statements only. Route: inline. Risk: item 2. Commit: —
- [ ] **T9** (S7, S11) ORM mapping: template ↔ table, aggregates per workflow definition, composite keys (`wf_id` + local key), existing tables with explicit column mapping, generated tables; DDL `db-create-table` / `db-check-schema`, `db-drop-table` disabled by default, no automatic `ALTER`. Route: inline. Risk: item 1. Commit: —
- [ ] **T10** (S7, S11) Cycle transaction: `workflow_instance`, automatic sync per aggregate, `ROLLBACK` + rehydration of affected instances, echo suppression; DML UDFs (`db-insert`, `db-update`, `db-delete`, `db-load`, `db-count`). Route: inline. Risk: item 1. Commit: —
- [ ] **T11** (S10, S16) Workflow lifecycle: `wf-complete` / `wf-fail`, clock fact for deadlines, idle eviction and on-demand rehydration. Route: inline. Risk: item 1. Commit: —

Phase 6 — Exchange and notification
- [ ] **T12** (S12, S13, S15) Finalize `docs/exchange-contract.md` v1.0: DDL, state machines, envelope JSON Schema, per-message-type schema process, correlation, idempotency, claiming, retries, dead letter, notification. Route: inline. Risk: item 3. Commit: —
- [ ] **T13** (S11, S12, S13) Engine side of the exchange: consume `ASIGNADO` rows in the cycle transaction, write `OUT`, deduplication, correlation table; changes made by other writers enter as `IN` messages (D22). Route: inline. Risk: item 1. Commit: —
- [ ] **T14** (S8, S14) ZeroMQ plugin: wake-up subscriber + safety polling; pub/sub fact injection for non-durable sources through the ingest pipeline. Route: inline. Risk: item 4. Commit: —
- [ ] **T15** (S9, S10, S13) Single-node intake mode: correlation, instance creation and assignment of `RECIBIDO` rows, as a library the v2 dispatcher reuses. Route: inline. Risk: item 4. Commit: —
- [ ] **T16** (S10–S13) End-to-end sample: simulated connector + fibre-installation sample package. Route: inline. Commit: —
- [ ] **T17** (S1) systemd unit, operations guide v1. Route: inline. Risk: item 5. Commit: —

### v2 — Distribution
- [ ] **T18** (S9) Node identity, groups, heartbeat over ZeroMQ with load metrics and loaded packages. Route: inline. Commit: —
- [ ] **T19** (S9, S14) `clpd-dispatch`: correlation, instance creation, policies (direct, group, broadcast, round-robin, least-loaded pull-based, key affinity), notification after `COMMIT`. Route: to be decided at v2 start. Risk: item 4. Commit: —
- [ ] **T20** (S9) Dispatcher high availability: active/passive with a DB leader lock. Route: inline. Risk: item 4. Commit: —

### v3 — Resilience
- [ ] **T21** (S9, S11) Instance lease + fencing on every write. Route: inline. Risk: item 4. Commit: —
- [ ] **T22** (S9) Failover: dead-node detection, reassignment of instances and pending `IN` rows, rehydration. Route: inline. Risk: item 4. Commit: —
- [ ] **T23** (S17) Load tests, sizing, exchange table partitioning and retention configuration. Route: inline. Risk: item 1. Commit: —

### v4 — Advanced workflows
- [ ] **T24** (S16) Subflows through internal `IN`/`OUT` messages with the parent `wf_id`. Route: inline. Commit: —
- [ ] **T25** (S16) Version coexistence: several package versions per node, instances pinned to their version, new instances routed to the current version. Route: inline. Risk: item 3. Commit: —

### Later phase (out of current scope, S15)
- Construction of the external-system connectors (gateways), per system, in any language, against `docs/exchange-contract.md`.

## Log

- **L1** (original request) "Hola, necesito que analicemos cómo hacer un motor clips básico autónomo. Será un ejecutable en modo demonio que sea capaz de cargar paquetes de reglas a la hora de lanzarse, sea capaz de cada x segundos configurables ejecutar su run, sea capaz de ingerir hechos desde el exterior a través de la conexión por socket, sea capaz de trazar a un fichero los hechos que va generando y las reglas que va ejecutando y la salida estandar o de error, sea capaz de cargar funciones de una librería dinámica escrita en C de acuerdo al contrato que CLIPS tiene para hacer interfaz con el lenguaje C. Utiliza para ello las fuentes y documentación que se encuentran en https://www.clipsrules.net/"
- **L2** "antes de nada, además, incluye en el plan la necesidad de un sistema ORM hechos CLIPS <-> tablas de BD con plugin para gestión básica de DDL y DML de BD"
- **L3** "una cosa, más, aparte del socket para la inyección de hechos al motor, necesito un plugin para la inyección de hechos al motor modo publish/subscriber, ya sea con 0MQ u otras soluciones"
- **L4** "antes de nada, además, quiero que recojas la posibilidad de alta escalabilidad y dispatching de hechos de los canales externos sobre un demonio en concreto, sobre varios, sobre el demonio menos cargado, en round-robind, etc"
- **L5** "una pregunta antes de responder a las decisiones pendientes: se puede compartir la memoria de trabajo de los demonios ?" — Answer given: not natively; partition by key, broadcast reference data, DB as source of truth, optional aggregator tier.
- **L6** "una nota, el sistema se va a construir para la ejecución de flujos de trabajo codificados en paquetes de reglas clips; cada flujo de trabajo llevará un identificador único"
- **L7** "una aclaración, la persistencia de un flujo de trabajo puede estar diseminada en varias tablas de la BD. Por ejemplo, para un flujo de instalación de una línea de fibra intervienen varios actores y sistemas satélites al nuestro, CRM para gestionar el pedido sobre una cierta tecnología, el proveedor de fibra para gestionar con nuestro sistema  el alquiler de la línea, el instalador para gestionar con nuestro sistema la instalación de la línea, un sistema inventario para gestionar los datos del cliente y técnicos, un sistema de red para gestionar las actvaciones y desactivaciones técnicas, etc. Nuestro sistema interactúa con todos ellos persistiendo las gestiones de los diferentes flujos de trabajo en diferentes tablas de la BD."
- **L8** "una duda, con el resto de sistemas vamos a aislar el núcleo del motor (demonio) mediante patrones inbox y outbox que traten las diferentes interfaces (Colas -petición/respuesta asíncrona-, SOAP y REST -con petición/respuesta asíncrona/síncrona-, etc). No se si es conveniente se realicen con algún mecanismo de gateway entre sistema <-> BD <-> motor (demonio)"
- **L9** "es factible tener solo una tabla en BD de intercambio de mensajería independientemente del gateway, protocolo, etc ? los gateway pueden estar escritos en diferentes lenguajes. Sería bueno separar gateways en componentes autónomos por protocolo y lenguaje? en el caso de mensajería entrante, sería conveniente tener demonios dedicados a la lectura de mensajería insertada en la tabla de intercambio desde los sistemas externos desde BD al motor en forma de hechos y en el caso de mensajería saliente, sería conveniente tener demonios dedicados para lectura de la tabla de intercambio que transforme los registros con mensajería a enviar al protocolo en concreto ?" — Answer given: one canonical table; connectors autonomous per external system; outbound read by each connector; inbound assigned by the dispatcher and consumed transactionally by the engine.
- **L10** "vamos a usar ZeroMQ para el aviso con el sondeo de seguridad y vamos a pasar a resolver las decisiones. Tras esto, antes de comenzar a implementar móntame una especificación de requisitos persistente junto su plan de tareas a abordar por fases"
- **L11** Decisions answered by the user: architecture-resolved set accepted except block "Núcleo", reviewed as D1 "Unix, admin y pruebas", D2/D28 "JSON canónico → FactBuilder", D4 "Retirar + desalojar inactivas", D27 "Híbrido configurable"; D3 "Ambos"; D5 "C99 + CMake"; D6 "PostgreSQL"; D14/D29 "No lo sé aún"; D16 "Incrementales"; D18 "Mezcla de ambos"; D19 "Sí, con subflujos"; D20 "Sí, convivencia"; D21 "Mezcla"; D22 "También otros procesos"; D23 "Misma BD"; D26 "Solo el contrato, dejando para una fase posterior del proyecto la construcción de los mismos"; D30 "Sí, documento aparte".
- **L12** Evidence: CLIPS 6.4.2 core sources (`clips_core_source_642.zip`, Jan 2025) checked for `AddRouter`, router callback typedefs, `AddUDF`/`UserDefinedFunction`, `Load`/`LoadError`, `AssertString`, `Run`, `Watch`, `AddPeriodicFunction`, `SetHaltRules`, `AddAssertFunction`/`AddRetractFunction`/`AddModifyFunction`, `AddBefore/AfterRuleFiresFunction`; watch output for facts (`==>`) and rules (`FIRE`) goes to `STDOUT`; makefile builds `libclips.a`. APG 6.4.2 §1.2: one thread of execution per environment.
- **L13** Note: `/home/lmibanez/code` is the Git top level (no commits yet), so `clpdaemon` is not yet its own repository; resolve before the first work-unit commit (T1).
- **L14** "1. voy a usar mi github particular para crear el repositorio del proyecto. Será en modo monorepo. Inicializalo. 2. mantén el idioma inglés por defecto" — `clpdaemon` initialized as its own Git repository (branch `main`), monorepo layout documented in `README.md`; artifacts stay in English.
- **L15** "dale" (start T1). Tools installed at user level with Homebrew (owned by the user, no sudo): cmake 4.4.5, zeromq 4.3.5, libpq 18.6, postgresql@17 17.11 (not started as a service), sqlite already present. Worker delegation failed twice (`Writer session Git authority changed before admission`, the session started under `/home/lmibanez/code`), so T1 was done inline. Evidence: RED = configure failed before `vendor/clips` existed; GREEN = fresh configure + build with no warnings, `ctest` 1/1 passed; negative check: a missing fixture exits 1 with failed checks. Vendored files verified identical with `diff -r`; archive SHA-256 in `vendor/clips/README.md`. Slice boundary: f8657ba is a mechanical third-party import (~173k lines) reviewed by archive hash and `diff -r`, not line by line; 12d33dc holds the authored T1 changes. Native review assess: medium, not due.
- **L16** "Retoma clpdaemon: lanza la verificación independiente de T1 (PR #1) y, si pasa, marca la casilla del plan de pruebas del PR y sigue con T2." — Independent verification (`gentle-ai-verify`, risk item 5): PASS. Fresh configure + build 0 warnings, `ctest` 1/1, negative probes (missing/unreadable fixture, wrong slot, two rules, syntax error → exit 1; no arguments → exit 2), upstream archive re-downloaded with matching SHA-256 and `diff -r` identical (341 files), clips compiled `-fPIC -std=c99 -DLINUX` without `main.c`; S1, S2, S6 met for T1 scope. Non-blocking note: `tests/smoke/clips_embed_test.c:45,51` read `result` after a failed `Eval`. PR #1 test-plan box checked.
- **L17** T2 decisions answered by the user: without `--config` → "--config obligatorio"; unknown keys → "Error (Recomendado)"; required keys → "Solo node_id (Recomendado)"; message format → "clpdaemon: fichero:línea: mensaje". Defaults for the other keys come from `docs/requirements.md` §7 (`trace_text`, `trace_jsonl`, `packages`, `plugins` default to empty). Until T3, a run without `--check` validates and exits 0.
- **L18** T2 evidence: RED = `clpdaemon_cli` 1/19 cases with a stub `main`; GREEN = fresh configure + build with 0 warnings (`-Wall -Wextra -Werror`), `ctest` 2/2, `tests/cli/cli_test.sh` 19/19 (exit code, exact stdout and stderr per case); ASan + UBSan build 19/19 with no reports. Two test-script defects fixed during GREEN (a fixture violating the interval rule; relative binary path). Native review assess pending at commit time.
- **L19** T2 native review (lineage `review-fa4379a4adfa0cb6`, base `210dd9a`, lenses readability + reliability): approved, authority burned. 13 non-blocking advisory findings kept as later work; most relevant: read errors treated as EOF (`src/config.c:296`, a directory as `--config` reports "missing required key"), repeated `--config` silently keeps the last one (`src/main.c:27-29`), parsed values not asserted by tests, test comparison blind to trailing newlines, `line_error` format padding.
- **L20** User chose "Corregir los dos hallazgos primero". Fixed R3-read-error-treated-as-eof (`ferror` after the read loop → `clpdaemon: <file>: cannot read: <system error>`, exit 1) and R3-repeated-config-last-wins (`clpdaemon: option --config given more than once` + usage, exit 2). RED 19/21, GREEN 21/21 CLI cases, `ctest` 2/2, 0 warnings. The other 11 advisory findings of L19 stay as later work.
- **L21** Native review of 0336df4 (lineage `review-581f064e9479203a`): approved; advisory R3-errno-captured-after-free fixed by saving `errno` right after the read loop, before `free(buf)` (no RED possible: glibc >= 2.33 preserves `errno` in `free`; 21/21 CLI cases, `ctest` 2/2 still pass). Advisory R3-mkdir-fixture-not-idempotent not applicable: `cli_test.sh` runs in a fresh `mktemp -d`.
