# clpdaemon — Exchange Contract for Connectors

| | |
|---|---|
| Contract version | **1.0** (2026-11-22), change log in [section 12](#12-contract-versioning) |
| Audience | Teams building connectors (gateways) to external systems, in any language |
| Database | PostgreSQL 13 or later (checked on 17) |
| Schemas | [`docs/schemas/`](schemas/) (JSON Schema draft 2020-12) |
| Related | [`docs/requirements.md`](requirements.md) §4.7 (FR-28 to FR-33), decisions D10-D12, D19, D22, D25, D30 |

clpdaemon runs workflows as CLIPS rule packages. It never talks to an external system. A **connector** is a separate program, in any language, that talks to one external system (CRM, fibre provider, installer, ...) and to the engine **only** through the database table `mensaje_intercambio`. This document is the whole contract: a connector that follows it works, whatever its language, protocol or deployment.

Table, column and state names are Spanish (`mensaje_intercambio`, `estado`, `RECIBIDO`, ...); they are part of the contract and are written exactly as shown. Statements marked *informative* show what the engine or the dispatcher does; connectors never run them. Engine behaviour marked "implemented in T13" (or another task) is part of this contract; the task named is the one that builds it.

## Contents

1. [Overview](#1-overview)
2. [A message round trip](#2-a-message-round-trip)
3. [Tables](#3-tables)
4. [Canonical payload](#4-canonical-payload)
5. [Message type schemas](#5-message-type-schemas)
6. [Inbound messages (`IN`)](#6-inbound-messages-in)
7. [Outbound messages (`OUT`)](#7-outbound-messages-out)
8. [Correlation and deduplication](#8-correlation-and-deduplication)
9. [Operations: review and manual reprocess](#9-operations-review-and-manual-reprocess)
10. [Notification](#10-notification)
11. [Settings: defaults and bounds](#11-settings-defaults-and-bounds)
12. [Contract versioning](#12-contract-versioning)

## 1. Overview

```
 external system ◄──► connector ──INSERT IN──►  mensaje_intercambio  ──► intake (dispatcher) ──► engine node
                          ▲                      (IN and OUT rows)        correlate, assign       consume IN,
                          └──claim, send OUT──────────────┘ ◄─────────────────────────────────── write OUT
```

| Component | Does | Does not |
|---|---|---|
| Connector | Speaks the external protocol (queues, REST, SOAP, files); converts external messages to and from the [canonical payload](#4-canonical-payload) and validates it; inserts `IN` rows; claims, sends and settles its `OUT` rows; retries; writes the `delivery-failed` dead letter; authenticates to the external system. | Business logic; choosing engine nodes; setting `wf_id`; writing `correlacion` or aggregate tables. |
| Intake (dispatcher) | Correlates `IN` rows to a workflow instance (`wf_id`), creates instances for start messages, assigns rows to engine nodes, notifies nodes. In v1 the single node runs it itself (T15); from v2 it is `clpd-dispatch` (T19). | Reads or transforms payloads. |
| Engine node | Consumes its assigned `IN` rows and writes `OUT` rows in the same transaction as the workflow state; records external references in `correlacion`. | Calls external systems. |

One connector per **external system**, not per protocol: a system reached through REST and a queue is served by one connector. A connector may run as several instances (processes); each instance has its own name (`asignado_a`, for example `installer-gw-1`). The value of `endpoint_logico` names the external system as the workflows see it (`installer`, `crm`, `fibre-provider`), never a URL or queue name: the connector maps it to addresses in its own configuration.

Delivery guarantees:

| Direction | Guarantee | How |
|---|---|---|
| External → engine | Each external message is consumed by the workflow once. | The connector acknowledges the external system only after its `COMMIT`; duplicates are dropped by the [deduplication key](#8-correlation-and-deduplication); the engine marks a row `CONSUMIDO` in the transaction that applies it. |
| Engine → external | At least once, in order per instance and endpoint. | `OUT` rows are written in the engine's transaction; connectors claim them with a lease and retry; the external system drops duplicates by the idempotency key `correlacion_id`. |

## 2. A message round trip

A workflow instance `fi-000123` asks the installer to create a work order, and later learns that it was closed. Every step is one database transaction.

| Step | Who | Row written or changed |
|---|---|---|
| 1 | engine | `INSERT` row 1042: `OUT`, `wf_id='fi-000123'`, `correlacion_id='fi-000123:7'`, `endpoint_logico='installer'`, `tipo_mensaje='work-order-request'`, `estado='PENDIENTE'`, payload `{"schema":"work-order-request@1","data":{"address":"Calle Mayor 1, Madrid","slot-date":"2026-11-20"}}` |
| 2 | connector `installer-gw-1` | [claims](#72-claim) row 1042: `EN_ENVIO`, `asignado_a='installer-gw-1'`, `lease_hasta=now()+2 min`, `intentos=1` |
| 3 | connector | calls the installer API with `Idempotency-Key: fi-000123:7`; the API answers `201 {"order":"OI-55"}` |
| 4 | connector | [settles](#74-send-result) row 1042 as `CONFIRMADO` and inserts row 1043: `IN`, `RECIBIDO`, `tipo_mensaje='work-order-created'`, `correlacion_id='fi-000123:7'`, `ref_externa='OI-55'`, `msg_id_externo='response:fi-000123:7'`, then commits |
| 5 | intake | finds `wf_id` through `correlacion_id` (row 1042), sets row 1043 `ASIGNADO` to node `n1`, commits, [notifies](#10-notification) `n1` |
| 6 | engine `n1` | asserts the fact `work-order-created`, runs the rules, marks row 1043 `CONSUMIDO`; a rule records the reference `('installer', 'OI-55') → 'fi-000123'` in `correlacion` |
| 7 | connector | days later the installer's webhook reports event `evt-9f3c`: the connector inserts row 2210: `IN`, `RECIBIDO`, `tipo_mensaje='work-order-closed'`, `ref_externa='OI-55'`, `msg_id_externo='evt-9f3c'`, payload as in [section 4](#41-example); answers the webhook `200` after the commit |
| 8 | intake | finds `wf_id` through `correlacion` (`installer`, `OI-55`), assigns row 2210 to `n1` |
| 9 | engine `n1` | consumes row 2210 |

If the installer redelivers the webhook of step 7, the insert finds the same `(endpoint_logico, direccion, msg_id_externo)` and does nothing; the connector answers `200` again. If the installer API had failed in step 3, row 1042 would go to `REINTENTO` and be claimed again later; after the last attempt the connector would write the [dead letter](#76-dead-letter) `delivery-failed` for the workflow.

## 3. Tables

### 3.1 `mensaje_intercambio`

The one table for both directions (S13). It lives in the engine's database schema (normally `public`). The DDL is part of the contract: connectors may rely on every column, type and constraint below.

```sql
CREATE TABLE mensaje_intercambio (
  id                 BIGSERIAL    PRIMARY KEY,
  direccion          TEXT         NOT NULL CHECK (direccion IN ('IN', 'OUT')),
  wf_id              TEXT,
  correlacion_id     TEXT,
  endpoint_logico    TEXT         NOT NULL CHECK (endpoint_logico ~ '^[a-z][a-z0-9-]{0,62}$'),
  tipo_mensaje       TEXT         NOT NULL CHECK (tipo_mensaje ~ '^[a-z][a-z0-9-]{0,62}$'),
  ref_externa        TEXT,
  msg_id_externo     TEXT,
  payload            JSONB        NOT NULL,
  payload_original   BYTEA,
  estado             TEXT         NOT NULL,
  asignado_a         TEXT,
  lease_hasta        TIMESTAMPTZ,
  intentos           INTEGER      NOT NULL DEFAULT 0 CHECK (intentos >= 0),
  siguiente_intento  TIMESTAMPTZ,
  error              TEXT,
  creado             TIMESTAMPTZ  NOT NULL DEFAULT now(),
  actualizado        TIMESTAMPTZ  NOT NULL DEFAULT now(),
  CONSTRAINT mensaje_intercambio_dedup
    UNIQUE (endpoint_logico, direccion, msg_id_externo),
  CONSTRAINT mensaje_intercambio_estado CHECK (
       (direccion = 'IN'  AND estado IN ('RECIBIDO', 'ASIGNADO', 'CONSUMIDO', 'SIN_CORRELACION', 'ERROR'))
    OR (direccion = 'OUT' AND estado IN ('PENDIENTE', 'EN_ENVIO', 'REINTENTO', 'ENVIADO', 'CONFIRMADO', 'ERROR'))),
  CONSTRAINT mensaje_intercambio_in_msg_id CHECK (
    direccion <> 'IN' OR msg_id_externo IS NOT NULL),
  CONSTRAINT mensaje_intercambio_out_ids CHECK (
    direccion <> 'OUT' OR (wf_id IS NOT NULL AND correlacion_id IS NOT NULL)),
  CONSTRAINT mensaje_intercambio_campos CHECK (
        (estado <> 'ASIGNADO'  OR (wf_id IS NOT NULL AND asignado_a IS NOT NULL))
    AND (estado <> 'EN_ENVIO'  OR (asignado_a IS NOT NULL AND lease_hasta IS NOT NULL))
    AND (estado <> 'REINTENTO' OR siguiente_intento IS NOT NULL)),
  CONSTRAINT mensaje_intercambio_payload CHECK (
        jsonb_typeof(payload) = 'object'
    AND octet_length(payload::text) <= 65536
    AND coalesce(jsonb_typeof(payload -> 'data'), '') = 'object'
    AND coalesce(split_part(payload ->> 'schema', '@', 1), '') = tipo_mensaje)
);
```

### 3.2 Columns

| Column | `IN` row | `OUT` row |
|---|---|---|
| `id` | Assigned by the database. Order of insertion within one session; may have gaps (an insert that finds a duplicate still uses a sequence value). | Same. |
| `direccion` | `'IN'` | `'OUT'` |
| `wf_id` | `NULL` on insert. Set by the intake when it correlates the row (§8). Connectors never set it. | The instance that wrote the row. Never `NULL`. |
| `correlacion_id` | For a response or a `delivery-failed`: the `correlacion_id` of the `OUT` row it answers. Otherwise `NULL`. | Opaque text, unique among `OUT` rows, generated by the engine (T13). Send it to the external system as the idempotency key and, when the protocol allows, as the correlation key it echoes in its answer. |
| `endpoint_logico` | The external system the message came from, `[a-z][a-z0-9-]{0,62}`. | The external system to send to. Each connector instance claims only its endpoints. |
| `tipo_mensaje` | Message type, `[a-z][a-z0-9-]{0,62}`; names the deftemplate the engine asserts and the [schema](#5-message-type-schemas) file. | Message type of the outgoing message. |
| `ref_externa` | The business reference in the external system (order number, work order) when the message has one; used for [correlation](#8-correlation-and-deduplication). | The reference, when the workflow knows it. |
| `msg_id_externo` | Required. The external message id, or a deterministic id the connector derives ([§6.2](#62-insert)). Deduplication key with `endpoint_logico` and `direccion`. | `NULL` (v1.0). |
| `payload` | The [canonical payload](#4-canonical-payload), at most 65536 bytes as PostgreSQL prints it. | Same, written by the engine. |
| `payload_original` | Optional copy of the external message as received, for audit. Never read by the engine. | `NULL`. |
| `estado` | [IN states](#61-states). | [OUT states](#71-states). |
| `asignado_a` | The engine node (`node_id`) the row is assigned to. | The connector instance that claimed the row last. |
| `lease_hasta` | `NULL` (reserved for v3). | End of the claim's lease while `EN_ENVIO`, else `NULL`. |
| `intentos` | Correlation attempts of the intake; reset to `0` by a manual reprocess. | Number of claims (send attempts started). |
| `siguiente_intento` | While `SIN_CORRELACION`: next correlation attempt; `NULL` when the intake has given up. | While `REINTENTO`: earliest time of the next attempt. |
| `error` | Why the row is `SIN_CORRELACION` or `ERROR`, one line. | Last send error, one line, at most 1000 characters. |
| `creado` | Insert time (database clock). | Same. |
| `actualizado` | Time of the last change. Every statement in this document sets it; set it in yours. | Same. |

All times are the database clock (`now()`); connectors never write times computed from their own clock.

### 3.3 Indexes

The partial indexes serve the pending states, so their size follows the backlog, not the history.

```sql
-- intake: new inbound rows
CREATE INDEX mensaje_intercambio_in_recibido
  ON mensaje_intercambio (id)
  WHERE direccion = 'IN' AND estado = 'RECIBIDO';
-- intake: correlation retries that are due
CREATE INDEX mensaje_intercambio_in_sin_correlacion
  ON mensaje_intercambio (siguiente_intento)
  WHERE direccion = 'IN' AND estado = 'SIN_CORRELACION';
-- engine node: its assigned rows, in id order
CREATE INDEX mensaje_intercambio_in_asignado
  ON mensaje_intercambio (asignado_a, id)
  WHERE direccion = 'IN' AND estado = 'ASIGNADO';
-- connectors: claim by endpoint, in id order
CREATE INDEX mensaje_intercambio_out_pendiente
  ON mensaje_intercambio (endpoint_logico, id)
  WHERE direccion = 'OUT' AND estado IN ('PENDIENTE', 'EN_ENVIO', 'REINTENTO');
-- connectors: per-instance send order
CREATE INDEX mensaje_intercambio_out_orden
  ON mensaje_intercambio (wf_id, endpoint_logico, id)
  WHERE direccion = 'OUT' AND estado IN ('PENDIENTE', 'EN_ENVIO', 'REINTENTO');
-- operators: rows waiting for review
CREATE INDEX mensaje_intercambio_revision
  ON mensaje_intercambio (id)
  WHERE estado IN ('SIN_CORRELACION', 'ERROR');
-- responses: the OUT row of a correlacion_id
CREATE UNIQUE INDEX mensaje_intercambio_out_correlacion
  ON mensaje_intercambio (correlacion_id)
  WHERE direccion = 'OUT';
-- audit: the messages of one instance
CREATE INDEX mensaje_intercambio_wf_id
  ON mensaje_intercambio (wf_id, id);
```

Partitioning and retention are decided in T23 with load data. Whatever T23 decides keeps, for every 1.x version: the columns of §3.1, their meaning, the states, and the deduplication behaviour of §6.2, including `INSERT ... ON CONFLICT (endpoint_logico, direccion, msg_id_externo) DO NOTHING`. Retention keeps an `IN` row at least as long as its external system may redeliver the message.

### 3.4 `correlacion`

Maps an external reference to the workflow instance that owns it, for messages that are not responses (§8).

```sql
CREATE TABLE correlacion (
  endpoint_logico  TEXT         NOT NULL CHECK (endpoint_logico ~ '^[a-z][a-z0-9-]{0,62}$'),
  ref_externa      TEXT         NOT NULL,
  wf_id            TEXT         NOT NULL,
  creado           TIMESTAMPTZ  NOT NULL DEFAULT now(),
  PRIMARY KEY (endpoint_logico, ref_externa)
);
CREATE INDEX correlacion_wf_id ON correlacion (wf_id);
```

| Column | Meaning |
|---|---|
| `endpoint_logico` | The external system the reference belongs to. |
| `ref_externa` | The reference as it appears in `mensaje_intercambio.ref_externa`. |
| `wf_id` | The instance that owns the reference. A reference belongs to one instance for ever: it is never reassigned, and it is not released when the instance ends (a late event of an ended instance must never reach a new one). A connector whose external system recycles references makes them unique, for example with a date or prefix in `ref_externa`. |
| `creado` | When the reference was recorded. |

Written by the engine (a workflow records a reference when it learns it, implemented in T13) and by the intake (the reference of a start message, for the instance it creates, T15). Connectors only read it.

### 3.5 Database access

Each connector logs in with its own role. Minimum privileges:

```sql
CREATE ROLE connector_installer LOGIN;
GRANT SELECT, INSERT ON mensaje_intercambio TO connector_installer;
GRANT UPDATE (estado, asignado_a, lease_hasta, intentos, siguiente_intento, error, actualizado)
  ON mensaje_intercambio TO connector_installer;
GRANT USAGE ON SEQUENCE mensaje_intercambio_id_seq TO connector_installer;
GRANT SELECT ON correlacion TO connector_installer;
```

A connector never updates `payload`, `wf_id` or the identifying columns of a row. Operators who [reprocess](#9-operations-review-and-manual-reprocess) rows need `UPDATE` on `payload` too. Set the password through the password file or secret store of your platform; never in logs.

## 4. Canonical payload

### 4.1 Example

```json
{
  "schema": "work-order-closed@1",
  "data": { "work-order": "OI-55", "closed-at": "2026-11-17T10:22:00Z", "result": "ok" }
}
```

This payload is valid against [`schemas/envelope.schema.json`](schemas/envelope.schema.json) and against the schema of its type, [`schemas/work-order-closed@1.json`](schemas/work-order-closed@1.json). Its row has `tipo_mensaje = 'work-order-closed'`. The receiving package defines

```clips
(deftemplate work-order-closed
  (slot wf-id) (slot work-order (type STRING)) (slot closed-at (type STRING))
  (slot result (type SYMBOL) (allowed-symbols ok failed cancelled))
  (slot notes (type STRING) (default "")))
```

and the engine asserts `(work-order-closed (wf-id "fi-000123") (work-order "OI-55") (closed-at "2026-11-17T10:22:00Z") (result ok) (notes ""))`.

### 4.2 Envelope rules

Every `payload`, in both directions, is a JSON object with exactly two members ([`schemas/envelope.schema.json`](schemas/envelope.schema.json)):

| Member | Rule |
|---|---|
| `schema` | `<tipo_mensaje>@<version>`: the row's `tipo_mensaje`, `@`, a positive integer without leading zeros. The table rejects a payload that is not an object, has no `data` object, or whose `schema` does not start with the row's `tipo_mensaje` (constraint `mensaje_intercambio_payload`); the rest is the connector's validation. |
| `data` | An object, slot name → value. Names match `[a-z][a-z0-9-]{0,62}`, at most 1024 members. `wf-id` and `correlacion-id` are never in `data`: the engine takes them from the row. |

Limits: the whole payload at most 65536 bytes as PostgreSQL prints it (`octet_length(payload::text)`), each string at most 8192 bytes of UTF-8, each array at most 1024 elements, at most 4096 values in the payload. JSON must be strict RFC 8259 UTF-8 without duplicate member names (PostgreSQL `jsonb` silently keeps the last of duplicates, so the table cannot detect them).

A connector validates every payload it inserts against the envelope schema and the schema of its `schema` value **before** the `INSERT`, and rejects (or dead-letters, for its own `OUT` handling) anything that fails. It validates an `OUT` payload before sending and treats an unknown `schema` version as a permanent failure (§7.5).

### 4.3 Values

The engine maps `data` to the deftemplate named by `tipo_mensaje` in the instance's package, using the slot's declared CLIPS type (implemented in T13; no JSON Schema validator runs in the engine):

| JSON value in `data` | CLIPS value |
|---|---|
| string | STRING; SYMBOL when the slot's type allows SYMBOL and not STRING |
| integer (no fraction, in the int64 range) | INTEGER; FLOAT when the slot's type allows FLOAT and not INTEGER |
| number with a fraction (`2.5`, `2.0`) | FLOAT |
| array of the values above | multifield, only for a multislot (`[]` is the empty multifield) |
| `true`, `false`, `null`, an object, a nested array | not allowed |

- A slot missing from `data` takes its deftemplate default; a slot declared `(default ?NONE)` must be present. To leave an optional value out, omit the member; there is no `null`.
- The engine reads the payload as PostgreSQL stores it: `jsonb` keeps `2.0` as `2.0` but prints `1e2` as `100`, so write FLOAT values with a decimal point, not an exponent.
- Every value must satisfy the slot's constraints (`type`, `allowed-...`, `range`, `cardinality`).
- The engine sets the slot `wf-id` from the row's `wf_id`, and the slot `correlacion-id`, when the deftemplate declares it, from the row's `correlacion_id` (STRING; the slot default when the column is `NULL`).
- Times are strings in RFC 3339 UTC (`2026-11-17T10:22:00Z`); booleans are symbols (`yes`/`no`) or an enumeration.

A row whose `data` does not map (an unknown slot, a wrong type, a missing required slot, a violated constraint, a `tipo_mensaje` the package has no deftemplate for) is moved to `ERROR` with the reason in `error`, in the engine's cycle transaction, and is reported by a log line and a metric (implemented in T13). The engine never retries it; see [§9](#9-operations-review-and-manual-reprocess).

`OUT` payloads follow the same table in reverse: STRING and SYMBOL become JSON strings, INTEGER an integer, FLOAT a number with a fraction (`3.0`), a multifield an array.

## 5. Message type schemas

### 5.1 Schema file rules

Each message type and version has one JSON Schema file, `docs/schemas/<tipo_mensaje>@<version>.json`, that validates the **whole payload** (envelope included), without references to other files, so any validator in any language can use it alone. [`schemas/work-order-closed@1.json`](schemas/work-order-closed@1.json) is the model. A file:

1. declares `"$schema": "https://json-schema.org/draft/2020-12/schema"` and `"$id": "urn:clpdaemon:schema:<tipo_mensaje>@<version>"`;
2. has `title` and `description`; the description names the direction (`IN` or `OUT`), the `endpoint_logico` values that use it, what `ref_externa` holds, and the receiving deftemplate;
3. at the root: `"type": "object"`, `"required": ["schema", "data"]`, `"additionalProperties": false`, `"properties"` with `"schema": {"const": "<tipo_mensaje>@<version>"}`;
4. for `data`: `"type": "object"`, `"additionalProperties": false`, one property per slot (never `wf-id` or `correlacion-id`), `required` listing at least the slots declared `(default ?NONE)`;
5. types each property with `string`, `integer`, `number` or `array` of those, and may add `description`, `enum`, `const`, `pattern`, `format`, `minLength`, `maxLength`, `minimum`, `maximum`, `items`, `minItems`, `maxItems`; it never allows `boolean`, `null` or `object` values. `format` is an annotation in most validators: add a `pattern` when the format matters;
6. carries `examples`: at least one complete payload, valid against the file and the envelope schema.

### 5.2 Publishing

A message type is published by a pull request to this repository that adds the schema file and its row in [§5.4](#54-published-schemas), reviewed by the team of the workflow package and the team of the connector. CI checks that every file under `docs/schemas/` is valid JSON and a valid draft 2020-12 schema, that it follows §5.1, and that every entry of its `examples` validates against it and against the envelope schema. Once merged, the file is **immutable**.

Before the first message of a new `IN` type or version is inserted, a package version that defines its deftemplate (with every slot of the schema) must be loaded; before the engine writes a new `OUT` type or version, the connector of its endpoint must support it.

### 5.3 Versioning

- Any change to a published schema is a new file with the next version: `work-order-closed@2.json`. Versions are integers, starting at `1`.
- Producers send the version they were built against; consumers accept every version still listed as `active`. The engine checks the slots, not the version (§4.3), so a package accepts two versions of an `IN` type when its deftemplate has the slots of both: add new slots with a default, and stop requiring a slot only after no producer sends the older version.
- A version nobody produces any more is marked `retired` in §5.4; its file stays, so stored rows can still be read.
- Message type schemas are versioned independently of this contract: publishing or retiring one is not a contract change.

### 5.4 Published schemas

| Schema | File | Direction | Producer | Consumer | Status |
|---|---|---|---|---|---|
| Envelope | [`envelope.schema.json`](schemas/envelope.schema.json) | both | all | all | active |
| `delivery-failed@1` | [`delivery-failed@1.json`](schemas/delivery-failed@1.json) | `IN` | every connector (§7.6) | every workflow that writes `OUT` rows | active |
| `work-order-closed@1` | [`work-order-closed@1.json`](schemas/work-order-closed@1.json) | `IN` | installer connector | fibre-installation workflow | example |

## 6. Inbound messages (`IN`)

### 6.1 States

```
                  intake                      engine node (cycle transaction)
 RECIBIDO ─────────────────► ASIGNADO(node) ─────────────────► CONSUMIDO
    │  ▲                         ▲     │
    │  │ operator (§9)           │     └──────────────────────► ERROR
    │  └──────────┐              │ intake (correlated)            │
    └──► SIN_CORRELACION ────────┘                                │
           │  ▲  retry every interval until the window ends       │
           └──┘                                                   │
 operator (§9): SIN_CORRELACION or ERROR ──► RECIBIDO ◄───────────┘
```

| State | Meaning | Entered by | Next |
|---|---|---|---|
| `RECIBIDO` | Inserted, not yet correlated. | connector (`INSERT`, §6.2); operator (§9) | `ASIGNADO`, `SIN_CORRELACION` |
| `ASIGNADO` | Correlated to `wf_id` and assigned to node `asignado_a`. | intake | `CONSUMIDO`, `ERROR` |
| `CONSUMIDO` | Applied by the workflow, in the same transaction as its state, or dropped because its instance has ended (reason in `error`, §6.5). Final. | engine node | — |
| `SIN_CORRELACION` | No instance found. While `siguiente_intento` is set the intake retries; once it is `NULL` the row waits for an operator. | intake | `ASIGNADO`, `SIN_CORRELACION`, `RECIBIDO` (operator) |
| `ERROR` | The engine could not apply the row (§4.3); reason in `error`. Waits for an operator. | engine node | `RECIBIDO` (operator) |

| Transition | Who | Statement |
|---|---|---|
| — → `RECIBIDO` | connector | [§6.2](#62-insert) |
| `RECIBIDO`/`SIN_CORRELACION` → `ASIGNADO` | intake | [§6.5](#65-intake-and-engine-transitions-informative), informative |
| `RECIBIDO`/`SIN_CORRELACION` → `SIN_CORRELACION` | intake | [§6.5](#65-intake-and-engine-transitions-informative), informative |
| `ASIGNADO` → `CONSUMIDO` / `ERROR` | engine node | [§6.5](#65-intake-and-engine-transitions-informative), informative |
| `SIN_CORRELACION`/`ERROR` → `RECIBIDO` | operator | [§9](#9-operations-review-and-manual-reprocess) |

### 6.2 Insert

```sql
INSERT INTO mensaje_intercambio
  (direccion, endpoint_logico, tipo_mensaje, ref_externa, msg_id_externo,
   correlacion_id, payload, payload_original, estado)
VALUES ('IN', $1::text, $2::text, $3::text, $4::text, $5::text, $6::jsonb, $7::bytea, 'RECIBIDO')
ON CONFLICT (endpoint_logico, direccion, msg_id_externo) DO NOTHING
RETURNING id;
```

Rules:

1. Convert the external message to the canonical payload and validate it (§4.2).
2. `msg_id_externo` is required. Use the external system's message id. When it has none, derive a deterministic one from what identifies the message, so a redelivery gets the same value: for a synchronous response `response:<correlacion_id>`, for a polled resource `<ref_externa>:<version or timestamp of the resource>`. Never use a random value.
3. No row returned means a duplicate: treat it as success and acknowledge it to the external system. (Without `ON CONFLICT`, SQLSTATE `23505` on `mensaje_intercambio_dedup` means the same.)
4. Acknowledge the external system only after the `COMMIT`. If the commit fails, do not acknowledge: the external system redelivers.
5. `wf_id` is not set. `correlacion_id` is set only for a response to an `OUT` row (§6.3).
6. After the `COMMIT`, optionally send the [wake-up hint](#101-connector-wake-up).

### 6.3 Responses

A response to an `OUT` row is an `IN` row with the same `correlacion_id` and its own `tipo_mensaje` (for example `work-order-created` answering `work-order-request`). For a synchronous protocol, insert it in the transaction that [settles](#74-send-result) the `OUT` row. For an asynchronous protocol, insert it when the answer arrives and, in the same transaction, confirm the `OUT` row (§7.7). If the external system does not echo `correlacion_id`, leave it `NULL` and set `ref_externa`: the intake then correlates through `correlacion`.

### 6.4 Order

`IN` rows are assigned and consumed in `id` order. Ids grow in insertion order within one database session, but two sessions can commit out of order. When two messages about the same reference must be applied in order, insert them from one session, in that order (or in one transaction).

### 6.5 Intake and engine transitions (informative)

The intake correlates a row (§8.2) and assigns it to a node (T15; from v2 T19), after which it [notifies](#102-intake-to-node-notification) the node:

```sql
UPDATE mensaje_intercambio
   SET estado = 'ASIGNADO', wf_id = $2::text, asignado_a = $3::text,
       siguiente_intento = NULL, error = NULL, actualizado = now()
 WHERE id = $1::bigint AND direccion = 'IN' AND estado IN ('RECIBIDO', 'SIN_CORRELACION');
```

When no instance is found, it retries every `correlation_retry_interval` for `correlation_retry_window` (§11), that is at most `$4 = ceil(window / interval)` attempts counted in `intentos`; after the last one `siguiente_intento` is `NULL`, and the intake writes a log line and counts a metric for the row (T15):

```sql
UPDATE mensaje_intercambio
   SET estado = 'SIN_CORRELACION', intentos = intentos + 1,
       siguiente_intento = CASE WHEN intentos + 1 < $4::integer
                                THEN now() + $3::bigint * interval '1 millisecond' END,
       error = $2::text, actualizado = now()
 WHERE id = $1::bigint AND direccion = 'IN' AND estado IN ('RECIBIDO', 'SIN_CORRELACION');
```

The engine node reads its rows in `id` order and, in its cycle transaction, marks each one `CONSUMIDO`, or `ERROR` with the reason (implemented in T13; FR-29):

```sql
UPDATE mensaje_intercambio
   SET estado = 'CONSUMIDO', actualizado = now()
 WHERE id = $1::bigint AND direccion = 'IN' AND estado = 'ASIGNADO' AND asignado_a = $2::text;

UPDATE mensaje_intercambio
   SET estado = 'ERROR', error = $3::text, actualizado = now()
 WHERE id = $1::bigint AND direccion = 'IN' AND estado = 'ASIGNADO' AND asignado_a = $2::text;
```

A row for an instance that has already ended (`completed` or `failed`) is not an error: the engine marks it `CONSUMIDO` without asserting it, stores the reason `instance '<wf_id>' is <status>; message dropped` in `error`, and logs one line (T13, as for the ingest drops of T11d). A late answer from an external system therefore never needs an operator.

**Synchronous commands** (a REST or SOAP call that asks the workflow to do something): the connector inserts the `IN` row and answers `202 Accepted` with the row `id`, after its `COMMIT`. The `wf_id` is assigned later by the intake (§8); the caller learns it, and the outcome, by a later query on that `id` (read-only, from the database, never through the engine) or by a callback. Connectors never generate a `wf_id`. Queries are answered from the database only.

## 7. Outbound messages (`OUT`)

### 7.1 States

```
                 connector claim                  send ok (async)
 PENDIENTE ───────────────────► EN_ENVIO(lease) ─────────────────► ENVIADO ──► CONFIRMADO
     ▲ engine INSERT              │   │   │  send ok (sync, answer received)         ▲
                                  │   │   └──────────────────────────────────────────┘
               transient failure  │   │ last attempt / permanent failure
       REINTENTO ◄────────────────┘   └──────────────► ERROR (dead letter + IN delivery-failed)
           │ claim when siguiente_intento <= now()
           └──────────► EN_ENVIO          (EN_ENVIO with an expired lease is claimed again)
```

| State | Meaning | Entered by | Next |
|---|---|---|---|
| `PENDIENTE` | Written by the workflow, not yet claimed. | engine node (`INSERT`, T13) | `EN_ENVIO` |
| `EN_ENVIO` | Claimed by connector instance `asignado_a` until `lease_hasta`. | connector | `ENVIADO`, `CONFIRMADO`, `REINTENTO`, `ERROR`; `EN_ENVIO` again when the lease expires |
| `REINTENTO` | A transient failure; waits until `siguiente_intento`. | connector | `EN_ENVIO` |
| `ENVIADO` | Handed to the external system; no confirmation yet. Final when the protocol never confirms. | connector | `CONFIRMADO` |
| `CONFIRMADO` | The external system confirmed or answered. Final. | connector | — |
| `ERROR` | Dead letter: the connector gave up; reason in `error`. Final: the workflow got a `delivery-failed` message and decides what to do. | connector | — |

| Transition | Who | Statement |
|---|---|---|
| — → `PENDIENTE` | engine node | T13 |
| `PENDIENTE`/`REINTENTO`/expired `EN_ENVIO` → `EN_ENVIO` | connector | [§7.2](#72-claim) |
| `EN_ENVIO` → `EN_ENVIO` (longer lease) | connector | [§7.3](#73-lease-extension) |
| `EN_ENVIO` → `ENVIADO` / `CONFIRMADO` | connector | [§7.4](#74-send-result) |
| `EN_ENVIO` → `REINTENTO` | connector | [§7.5](#75-retry) |
| `EN_ENVIO` → `ERROR` | connector | [§7.6](#76-dead-letter) |
| `ENVIADO` → `CONFIRMADO` | connector | [§7.7](#77-asynchronous-confirmation) |

### 7.2 Claim

```sql
UPDATE mensaje_intercambio m
   SET estado = 'EN_ENVIO', asignado_a = $1::text,
       lease_hasta = now() + $2::bigint * interval '1 millisecond',
       intentos = m.intentos + 1, siguiente_intento = NULL, actualizado = now()
 WHERE m.id IN (
   SELECT c.id
     FROM mensaje_intercambio c
    WHERE c.direccion = 'OUT'
      AND c.endpoint_logico = ANY ($3::text[])
      AND c.estado IN ('PENDIENTE', 'EN_ENVIO', 'REINTENTO')
      AND (c.estado = 'PENDIENTE'
           OR (c.estado = 'REINTENTO' AND c.siguiente_intento <= now())
           OR (c.estado = 'EN_ENVIO' AND c.lease_hasta < now()))
      AND NOT EXISTS (
            SELECT 1
              FROM mensaje_intercambio e
             WHERE e.direccion = 'OUT'
               AND e.wf_id = c.wf_id
               AND e.endpoint_logico = c.endpoint_logico
               AND e.id < c.id
               AND e.estado IN ('PENDIENTE', 'EN_ENVIO', 'REINTENTO'))
    ORDER BY c.id
    LIMIT $4::integer
    FOR UPDATE SKIP LOCKED)
RETURNING m.id, m.wf_id, m.correlacion_id, m.endpoint_logico, m.tipo_mensaje,
          m.ref_externa, m.payload, m.intentos;
```

| Parameter | Value |
|---|---|
| `$1` | This connector instance's name, unique among the running instances of all connectors (for example `installer-gw-1`). |
| `$2` | Lease in milliseconds (§11). |
| `$3` | The `endpoint_logico` values this instance serves. |
| `$4` | Batch size (§11). |

Rules:

1. Run the claim in its own short transaction and commit it before sending. Claim again every `out_poll_interval` while rows are returned; when a claim returns nothing, wait one interval.
2. Several instances may claim at once: `SKIP LOCKED` gives each row to one of them.
3. Order: a row is claimed only when no earlier `OUT` row of the same `wf_id` and `endpoint_logico` is still `PENDIENTE`, `EN_ENVIO` or `REINTENTO`. A claim therefore returns at most one row per instance and endpoint; send the returned rows in `id` order. Rows in `ERROR`, `ENVIADO` or `CONFIRMADO` do not hold the later ones back.
4. Each claim increments `intentos`. Remember the returned `intentos`: it is the **claim token**, and every later statement on the row checks it, so a connector whose lease expired cannot overwrite the result of the instance that claimed the row after it.
5. A returned row with `intentos` greater than `max_attempts` was claimed after a previous holder's lease expired on its last attempt: do not send it; write the [dead letter](#76-dead-letter) with the reason `lease expired on the last attempt`.
6. Validate the payload (§4.2) before sending; a payload that fails, or a `schema` version the connector does not support, is a permanent failure (§7.6).

### 7.3 Lease extension

The lease must cover the whole send (§11). For a send that may take longer, extend the lease before it expires; 0 rows updated means the claim is lost: stop and do not settle the row.

```sql
UPDATE mensaje_intercambio
   SET lease_hasta = now() + $4::bigint * interval '1 millisecond', actualizado = now()
 WHERE id = $1::bigint AND direccion = 'OUT' AND estado = 'EN_ENVIO'
   AND asignado_a = $2::text AND intentos = $3::integer;
```

`$1` the row id, `$2` the instance name, `$3` the claim token, `$4` the new lease in milliseconds.

### 7.4 Send result

Send with `correlacion_id` as the idempotency key (an `Idempotency-Key` header, a message id or a business key of the external system), then settle the row in one transaction:

```sql
UPDATE mensaje_intercambio
   SET estado = $4::text, lease_hasta = NULL, error = NULL, actualizado = now()
 WHERE id = $1::bigint AND direccion = 'OUT' AND estado = 'EN_ENVIO'
   AND asignado_a = $2::text AND intentos = $3::integer
   AND $4::text IN ('ENVIADO', 'CONFIRMADO');
```

- `$4 = 'CONFIRMADO'` when the external system confirmed or answered synchronously; insert the answer as an `IN` row (§6.2, with the `OUT` row's `correlacion_id` and `msg_id_externo = 'response:<correlacion_id>'` unless the answer has its own id) in the **same** transaction.
- `$4 = 'ENVIADO'` when the protocol is asynchronous (a queue accepted the message, a `202 Accepted`).
- 1 row updated: commit. 0 rows: the claim was lost (another instance holds the row now); roll back, including the answer, and log it. The new holder sends again, and the idempotency key makes the external system drop the duplicate.

### 7.5 Retry

A **transient** failure is one that may succeed later: a timeout, a refused or reset connection, HTTP 408, 429 or 5xx, a broker that is unavailable, an unknown outcome after the request left. A **permanent** failure will not: HTTP 4xx other than 408 and 429, a payload or `schema` version the connector or the external system rejects. Each connector classifies the errors of its protocol.

On a transient failure with `intentos < max_attempts`:

```sql
UPDATE mensaje_intercambio
   SET estado = 'REINTENTO', lease_hasta = NULL,
       siguiente_intento = now() + $4::bigint * interval '1 millisecond',
       error = left($5::text, 1000), actualizado = now()
 WHERE id = $1::bigint AND direccion = 'OUT' AND estado = 'EN_ENVIO'
   AND asignado_a = $2::text AND intentos = $3::integer;
```

`$4`, the delay in milliseconds after attempt `n` (`n` = the claim token `intentos`), is exponential with jitter:

```
d     = min(backoff_max, backoff_initial × 2^(n−1))
delay = a uniformly random value in [d/2, d]
```

With the defaults (`max_attempts` 8, `backoff_initial` 1 s, `backoff_max` 1 h) the delays after attempts 1 to 7 are at most 1, 2, 4, 8, 16, 32 and 64 s; the 8th failure is the dead letter. Each connector sets these per `endpoint_logico` within the bounds of [§11](#11-settings-defaults-and-bounds). 0 rows updated: the claim was lost; roll back.

A permanent failure, or a transient failure with `intentos >= max_attempts`, goes to the dead letter.

### 7.6 Dead letter

One statement moves the `OUT` row to `ERROR` and inserts its `delivery-failed` `IN` message ([`schemas/delivery-failed@1.json`](schemas/delivery-failed@1.json)), so the workflow can react:

```sql
WITH dl AS (
  UPDATE mensaje_intercambio
     SET estado = 'ERROR', lease_hasta = NULL, siguiente_intento = NULL,
         error = left($4::text, 1000), actualizado = now()
   WHERE id = $1::bigint AND direccion = 'OUT' AND estado = 'EN_ENVIO'
     AND asignado_a = $2::text AND intentos = $3::integer
  RETURNING id, correlacion_id, endpoint_logico, tipo_mensaje, ref_externa, intentos, error)
INSERT INTO mensaje_intercambio
  (direccion, correlacion_id, endpoint_logico, tipo_mensaje, ref_externa,
   msg_id_externo, payload, estado)
SELECT 'IN', correlacion_id, endpoint_logico, 'delivery-failed', ref_externa,
       'delivery-failed:' || id,
       jsonb_build_object('schema', 'delivery-failed@1',
                          'data', jsonb_build_object('out-id', id, 'out-type', tipo_mensaje,
                                                     'attempts', intentos, 'reason', error)),
       'RECIBIDO'
  FROM dl
RETURNING id;
```

`$4` is the reason, one line. 1 row returned: commit. 0 rows: the claim was lost; roll back. The `delivery-failed` row correlates to the instance through `correlacion_id` like any response. The engine defines the deftemplate `delivery-failed` for every package, with the slots `wf-id`, `correlacion-id`, `out-id` (INTEGER), `out-type` (STRING), `attempts` (INTEGER) and `reason` (STRING) (implemented in T13); a package does not define its own. A dead-lettered `OUT` row is never sent again: to try again, the workflow writes a new `OUT` row.

### 7.7 Asynchronous confirmation

When the answer to an `ENVIADO` row arrives, insert it as an `IN` row (§6.3) and, in the same transaction:

```sql
UPDATE mensaje_intercambio
   SET estado = 'CONFIRMADO', actualizado = now()
 WHERE direccion = 'OUT' AND correlacion_id = $1::text AND estado = 'ENVIADO';
```

0 rows is normal (the row is still `EN_ENVIO` because the answer overtook the settle, or it is already `CONFIRMADO`): commit anyway. The `IN` row is what the workflow sees; `CONFIRMADO` is for monitoring.

## 8. Correlation and deduplication

### 8.1 Keys

| Key | Purpose | Set by |
|---|---|---|
| `(endpoint_logico, direccion, msg_id_externo)` | Deduplication of `IN` rows: the same external message is stored once. | connector |
| `correlacion_id` | Links an `OUT` request with its `IN` responses and its `delivery-failed`; idempotency key towards the external system. | engine (on `OUT`), copied by the connector |
| `(endpoint_logico, ref_externa)` in `correlacion` | Routes unsolicited events (no `correlacion_id`) to the instance that owns the business reference. | engine (workflow rules, T13), intake (start messages, T15) |

### 8.2 How the intake finds the instance

For each `RECIBIDO` row, in `id` order (T15):

1. `wf_id` already set (a row put back by an operator): that instance.
2. `correlacion_id` set: the `wf_id` of the `OUT` row with that `correlacion_id`.
3. `ref_externa` set: the `wf_id` of `correlacion` for `(endpoint_logico, ref_externa)`.
4. `tipo_mensaje` starts a workflow definition: a new instance; when `ref_externa` is set, the intake records it in `correlacion` for the new instance.
5. Otherwise: `SIN_CORRELACION` (§6.5). The intake retries, because a reference may be recorded just after the event arrives.

## 9. Operations: review and manual reprocess

`SIN_CORRELACION` (once `siguiente_intento` is `NULL`) and `IN` `ERROR` rows are terminal until an operator acts; each is reported by a log line and a metric when it becomes terminal (T13, T15). Rows to review:

```sql
SELECT id, direccion, estado, endpoint_logico, tipo_mensaje, ref_externa, wf_id, intentos, error, creado
  FROM mensaje_intercambio
 WHERE estado IN ('SIN_CORRELACION', 'ERROR')
   AND (estado = 'ERROR' OR siguiente_intento IS NULL)
 ORDER BY id;
```

After fixing the cause (a missing correlation, a package not loaded, a wrong payload), put the row back to `RECIBIDO`. The intake picks it up at its next poll, with a new correlation window:

```sql
UPDATE mensaje_intercambio
   SET estado = 'RECIBIDO', asignado_a = NULL, lease_hasta = NULL,
       siguiente_intento = NULL, intentos = 0, error = NULL, actualizado = now()
 WHERE id = $1::bigint AND direccion = 'IN' AND estado IN ('SIN_CORRELACION', 'ERROR');
```

To correct the payload at the same time, add `payload = $2::jsonb` to the `SET` list; the new payload must validate (§4.2). Never change `direccion`, `endpoint_logico`, `msg_id_externo` or `tipo_mensaje`. A row keeps its `wf_id`; set `wf_id = NULL` too when the correlation itself was wrong. In `psql`:

```
UPDATE mensaje_intercambio SET estado = 'RECIBIDO', asignado_a = NULL, lease_hasta = NULL,
       siguiente_intento = NULL, intentos = 0, error = NULL, actualizado = now()
 WHERE id = 2210 AND direccion = 'IN' AND estado IN ('SIN_CORRELACION', 'ERROR');
```

`OUT` rows in `ERROR` are not reprocessed (§7.6).

## 10. Notification

Notifications only shorten the wait. They are ZeroMQ messages that carry **hints, never data**: every component also polls the table (safety polling), so a lost, delayed or forged notification never loses or changes a message (FR-31, S14). Notifications are sent only **after** the `COMMIT` of the rows they hint at.

### 10.1 Connector wake-up

Optional. After committing `IN` rows, a connector may wake the intake so it correlates them before its next poll.

| Item | Contract |
|---|---|
| Sockets | The intake binds a `SUB` socket and subscribes to `clpd/1/wake`. The connector uses a `PUB` socket and connects it to every configured wake endpoint (both dispatchers of an active/passive pair; the passive one ignores the hints). |
| Endpoint | Connector setting `wake_endpoints`: a list of ZeroMQ endpoints, for example `tcp://intake-1.internal:7701`; empty (the default) disables the wake-up. The intake's bind address is its setting `wake_bind`, default `tcp://*:7701` (T14 in v1, where the node runs the intake; T19 in v2). |
| Message | Two frames. Frame 1: exactly the 11 ASCII bytes `clpd/1/wake`. Frame 2: the `endpoint_logico` of the committed rows (1-63 bytes, `[a-z][a-z0-9-]*`), or empty. |
| Send | Non-blocking (`ZMQ_DONTWAIT`); when the message cannot be queued, drop it. At most one per committed transaction; batching is fine. Set `ZMQ_LINGER` to 0. |
| Receive | The intake discards a message that is not exactly two frames, whose frame 1 is not exactly `clpd/1/wake`, or whose frame 2 is not empty or a valid `endpoint_logico`. A valid one makes it poll `RECIBIDO` rows soon, at most once per `cycle_min_interval_ms`; frame 2 is only logged. |

### 10.2 Intake to node notification

Informative for connector teams (T14, T19).

| Item | Contract |
|---|---|
| Sockets | Each dispatcher binds a `PUB` socket at `notify_bind` (default `tcp://*:7702`). Each node connects a `SUB` socket to the `notify_bind` endpoint of every dispatcher (node setting `notify_endpoints`) and subscribes to `clpd/1/node/<node_id>`. |
| Message | Two frames. Frame 1: exactly `clpd/1/node/<node_id>`. Frame 2: the ASCII bytes `assigned`. |
| Send | After the `COMMIT` that set rows `ASIGNADO` to that node; non-blocking, dropped when it cannot be queued. |
| Receive | A node discards any message whose frame 1 is not exactly its own topic (a subscription to `clpd/1/node/n1` also matches `clpd/1/node/n10`). A valid message schedules a cycle (FR-3: no sooner than `cycle_min_interval_ms` after the previous one); any frame 2 value is treated as `assigned`. |

In v1 the single node runs the intake itself (T15) and wakes its own cycle in-process; no `notify` socket is used.

### 10.3 Safety polling

| Who | Polls | Interval |
|---|---|---|
| Intake | `RECIBIDO` rows, and `SIN_CORRELACION` rows whose `siguiente_intento <= now()` | `safety_poll_interval_ms` (default 5000) |
| Engine node | Its `ASIGNADO` rows | Every `safety_poll_interval_ms`, and in every cycle a notification triggers (T13, T14) |
| Connector | Its `OUT` rows ([claim](#72-claim)) | `out_poll_interval` (§11). The engine does not notify connectors in v1.0. |

The ZeroMQ sockets are plain TCP without authentication: bind them to an internal network. A forged message only triggers an extra poll.

## 11. Settings: defaults and bounds

Connector settings are configured per `endpoint_logico` (L117). A connector refuses to start with a value outside the bounds.

| Setting | Default | Bounds | Meaning |
|---|---|---|---|
| `max_attempts` | 8 | 1-100 | Claims of one `OUT` row before the dead letter. |
| `backoff_initial` | 1 s | 100 ms - 60 s | Delay bound after the first failed attempt (§7.5). |
| `backoff_max` | 1 h | `backoff_initial` - 24 h | Upper bound of the delay. |
| jitter | `[d/2, d]` | fixed | Not configurable; never 0. |
| `lease` | 120 s | 10 s - 1 h | Claim lease; at least `send_timeout` + 5 s. |
| `send_timeout` | 30 s | 1 s - `lease` − 5 s | Longest single send before it counts as a transient failure. |
| `batch` | 20 | 1-500 | Rows per claim. |
| `out_poll_interval` | 1 s | 100 ms - 60 s | Wait after an empty claim. |
| `wake_endpoints` | empty | — | §10.1. |

Intake settings (T15; v2 T19):

| Setting | Default | Bounds | Meaning |
|---|---|---|---|
| `correlation_retry_window` | 10 min | 0 - 24 h | How long a `SIN_CORRELACION` row is retried; 0 = no retry. |
| `correlation_retry_interval` | 30 s | 1 s - 1 h | Wait between correlation attempts. |
| `safety_poll_interval_ms` | 5000 | positive | [`configuration.md`](configuration.md). |
| `wake_bind` | `tcp://*:7701` | ZeroMQ endpoint | §10.1. |

## 12. Contract versioning

The contract has a version `MAJOR.MINOR`, in the header of this document.

| Change | Version |
|---|---|
| New nullable column with a default that connectors may ignore; new index; new message type or schema version (§5.3, not a contract change at all); new optional setting; clarification | MINOR |
| Removed or renamed column, table or state; changed type, meaning or constraint; new state; a change to the envelope (§4.2) or to the value mapping (§4.3); a change to a statement connectors run that makes the 1.x statement wrong; a change to the notification frames | MAJOR, agreed with every connector team before it ships |

Connectors state the contract version they were built against. Every 1.x engine accepts the behaviour this document describes.

### Change log

| Version | Date | Changes |
|---|---|---|
| 1.0 | 2026-11-22 | First contract (T12, L117). From draft 0.1: `direccion` is `TEXT` (`CHAR(3)` padded `'IN'` to `'IN '`); constraints for states per direction, required `msg_id_externo` on `IN`, required `wf_id` and `correlacion_id` on `OUT`, fields required per state, payload shape and size; partial indexes for the pending states; `correlacion` DDL; the claim increments `intentos` (claim token) and keeps per-instance order; statements for lease extension, send result, retry, dead letter, confirmation and manual reprocess; envelope schema and the per-type schema process (`docs/schemas/`); value mapping by slot type; retry defaults and bounds; `SIN_CORRELACION` retry window; ZeroMQ wake-up and notification frames. Late messages for an ended instance are `CONSUMIDO` with the reason in `error`; synchronous commands are answered `202 Accepted` with the row `id`; a `correlacion` reference is never reassigned or released (L118). |
| 0.1 | — | Draft. |
