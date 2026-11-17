# clpdaemon — Exchange Contract for Connectors

| | |
|---|---|
| Status | Draft 0.1 (finalized as v1.0 in task T12) |
| Audience | Teams building connectors (gateways) to external systems, in any language |
| Database | PostgreSQL |
| Related | `docs/requirements.md` §4.7 |

Connectors are the only components that talk to external systems. They talk to the engine **only** through the table `mensaje_intercambio`. This document is the whole contract: if a connector follows it, its language, protocol and deployment do not matter.

## 1. Responsibilities

| Component | Does | Does not |
|---|---|---|
| Connector | Speaks the external protocol (queues, REST, SOAP, files); converts to and from canonical JSON; inserts `IN` rows; claims and sends its `OUT` rows; retries; authenticates. | Business logic; choosing engine nodes; writing aggregate tables. |
| Dispatcher | Correlates `IN` rows to `wf_id`; creates instances; assigns rows to nodes; notifies nodes. | Reads or transforms payloads. |
| Engine node | Consumes its assigned `IN` rows and writes `OUT` rows in the same transaction as the workflow state. | Calls external systems. |

One connector per **external system** (not per protocol). A system that uses two protocols is served by one connector.

## 2. Table

```sql
CREATE TABLE mensaje_intercambio (
  id                 BIGSERIAL    PRIMARY KEY,
  direccion          CHAR(3)      NOT NULL CHECK (direccion IN ('IN','OUT')),
  wf_id              TEXT,                      -- NULL until correlated (IN)
  correlacion_id     TEXT,                      -- links a request with its response
  endpoint_logico    TEXT         NOT NULL,     -- 'crm', 'fibre-provider', 'network'... never a URL or queue name
  tipo_mensaje       TEXT         NOT NULL,     -- e.g. 'line-rental-request', 'work-order-closed'
  ref_externa        TEXT,                      -- external system reference (order no., work order...)
  msg_id_externo     TEXT,                      -- external message id, for deduplication
  payload            JSONB        NOT NULL,     -- canonical envelope (section 4)
  payload_original   BYTEA,                     -- optional original message, for audit
  estado             TEXT         NOT NULL,
  asignado_a         TEXT,                      -- node (IN) or connector instance (OUT)
  lease_hasta        TIMESTAMPTZ,
  intentos           INTEGER      NOT NULL DEFAULT 0,
  siguiente_intento  TIMESTAMPTZ,
  error              TEXT,
  creado             TIMESTAMPTZ  NOT NULL DEFAULT now(),
  actualizado        TIMESTAMPTZ  NOT NULL DEFAULT now(),
  UNIQUE (endpoint_logico, direccion, msg_id_externo)
);
```

Partitioning by `direccion` and creation date, partial indexes on pending states and the retention policy are defined in T12/T23.

## 3. State machines

```
IN : RECIBIDO ──► ASIGNADO(node) ──► CONSUMIDO
        └──► SIN_CORRELACION / ERROR   (manual review or retry)

OUT: PENDIENTE ──► EN_ENVIO(connector, lease) ──► ENVIADO ──► CONFIRMADO
                         └──► REINTENTO(siguiente_intento) ──► ... ──► ERROR (dead letter)
```

| State | Written by |
|---|---|
| `RECIBIDO` | connector (insert) |
| `ASIGNADO`, `SIN_CORRELACION` | dispatcher |
| `CONSUMIDO`, `ERROR` (IN) | engine node |
| `PENDIENTE` | engine node (insert) |
| `EN_ENVIO`, `ENVIADO`, `CONFIRMADO`, `REINTENTO`, `ERROR` (OUT) | connector |

## 4. Canonical payload

```json
{
  "schema": "work-order-closed@1",
  "data": { "work-order": "OI-55", "closed-at": "2026-11-17T10:22:00Z", "result": "ok" }
}
```

- `schema` = `tipo_mensaje@schema-version`. Each message type has a JSON Schema published with this contract.
- `data` keys map to the slots of the deftemplate of the same name in the receiving workflow package. The engine rejects payloads that do not validate (row → `ERROR` with the reason).
- `wf-id` is never required in `data`; the engine takes it from the row.

## 5. Connector rules

### 5.1 Inbound (external → engine)
1. Convert the external message to the canonical payload.
2. `INSERT` one row: `direccion='IN'`, `estado='RECIBIDO'`, `endpoint_logico`, `tipo_mensaje`, `ref_externa`, `msg_id_externo`, `payload`, and `correlacion_id` when it is a response to an `OUT` message.
3. A unique-key violation means a duplicate: treat it as success (acknowledge to the external system).
4. Acknowledge the external system only after the `COMMIT`.

### 5.2 Outbound (engine → external)
1. Claim rows for your endpoints in small batches:
   ```sql
   UPDATE mensaje_intercambio m
      SET estado='EN_ENVIO', asignado_a=$instance, lease_hasta=now()+$lease, actualizado=now()
    WHERE m.id IN (
      SELECT id FROM mensaje_intercambio
       WHERE direccion='OUT' AND endpoint_logico = ANY($endpoints)
         AND (estado='PENDIENTE'
              OR (estado='REINTENTO' AND siguiente_intento <= now())
              OR (estado='EN_ENVIO' AND lease_hasta < now()))
       ORDER BY id
       FOR UPDATE SKIP LOCKED
       LIMIT $batch)
   RETURNING *;
   ```
2. Messages with the same `wf_id` and `endpoint_logico` are sent in `id` order: do not send a later one while an earlier one is `EN_ENVIO` or `REINTENTO`.
3. Send, then in one transaction set `ENVIADO` (asynchronous protocol) or `CONFIRMADO` (synchronous response received) and, if there is a response, insert it as an `IN` row with the same `correlacion_id`.
4. On a transient failure: `REINTENTO`, `intentos+1`, `siguiente_intento` with exponential backoff. After the configured maximum: `ERROR` (dead letter) and an `IN` row of type `delivery-failed` with the same `correlacion_id`, so the workflow can react.
5. Sending must be idempotent towards the external system where possible (send `correlacion_id` as the idempotency key).

### 5.3 Synchronous inbound interfaces
- Queries: answer from the database (read-only), never through the engine.
- Commands: insert the `IN` row and answer `202 Accepted` with the instance reference; deliver the outcome by callback or later query.

## 6. Notification
- Connectors do not notify engine nodes. The dispatcher polls `RECIBIDO` rows and may additionally receive an optional ZeroMQ wake-up from connectors (endpoint and message format defined in T12).
- The dispatcher notifies nodes over ZeroMQ after its `COMMIT`; nodes also poll (safety polling).

## 7. Correlation
- Responses: `correlacion_id` equals the one of the `OUT` request.
- Unsolicited events: the dispatcher resolves `(endpoint_logico, ref_externa)` through the `correlacion` table, which workflows fill when they learn an external reference.
- Unresolvable rows become `SIN_CORRELACION` for review.

## 8. Versioning of this contract
Changes to the table, states or payload envelope are versioned (`MAJOR.MINOR`). New optional columns and new message types are minor; anything else is major and requires coordination with all connector teams.
