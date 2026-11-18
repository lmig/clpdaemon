# Fact ingestion

Facts enter the engine from outside through one pipeline (S4, FR-19): a plugin's `host->post_fact` ([`plugins.md`](plugins.md)) and, from T7c, the admin socket's `inject` command. Both take the same fact JSON, validated against a whitelist of deftemplates and asserted with the CLIPS `FactBuilder` API, never by evaluating or parsing CLIPS text.

## Whitelist

Only deftemplates named in a package's `ingest` list can be ingested ([`packages.md`](packages.md#manifest)):

```ini
ingest = order, payment
```

Each name must be a deftemplate the package defines, and no two packages may list the same name, so a template name in a fact is unqualified and resolves to exactly one package's deftemplate. Both are load errors, reported for every package (also under `--check`).

## Fact JSON

```json
{"template":"order","slots":{"wf-id":"w1","qty":3,"price":2.5,"kind":{"sym":"rush"},"tags":["a",{"sym":"b"}]}}
```

The document is an object with exactly the keys `template` (a string, the deftemplate name) and `slots` (an object, slot name to value), in any order. Values:

| JSON | CLIPS |
|---|---|
| string `"w1"` | STRING `"w1"` |
| `{"sym":"rush"}` | SYMBOL `rush` |
| integer (no fraction, no exponent, int64 range) `3` | INTEGER `3` |
| number with a fraction or an exponent `2.5`, `1e2` | FLOAT `2.5`, `100.0` |
| array of the values above `["a",{"sym":"b"}]` | multifield `("a" b)`, only for a multislot |

A single-field slot takes one value, a multislot takes an array (`[]` for an empty multifield). Booleans, `null`, nested arrays and objects other than `{"sym":"<symbol>"}` are rejected. A symbol must be non-empty, contain no space, control character or delimiter (`"`, `(`, `)`, `&`, `|`, `<`, `~`, `;`), and not start with `?` or `$?`. Slots not given take their deftemplate default; a slot declared `(default ?NONE)` must be given. Values must satisfy the slot's constraints (`type`, `range`, `allowed-...`, `cardinality`), checked field by field for a multislot.

Limits (the parser's, [`src/json.h`](../src/json.h)): 65536 bytes per document, nesting depth 4 (the fact, `slots`, a multislot array, a `{"sym":...}` in it), 8192 bytes per decoded string or key, 1024 elements per array or members per object, 4096 values per document. The JSON itself is strict RFC 8259 (UTF-8, no duplicate keys, no `NaN`).

## Queue

Posted facts wait in one bounded queue of 1024 entries, shared by every source and guarded by a mutex, so any thread may post. A post copies the text after checking that it is JSON within the limits; the template and slots are checked later, on the engine thread. Each post wakes the event loop (its eventfd). A post made before the loop exists, for example from a plugin's `init`, is queued too: when the loop starts, it is woken for the facts already queued.

At the start of every cycle, before the plugins' `cycle_begin`, the engine thread asserts the facts that were queued when the cycle started, in posting order. Facts posted during a cycle wait for the next one. A fact equal to an existing fact is accepted, and CLIPS keeps one. Facts still queued at shutdown, or under `--check`, are dropped.

## Rejection messages

Every rejection has one message; the checks run in this order and the first failure is reported. `<name>` is a name from the document as shown in a message: control characters become `?`, and a name longer than 64 bytes is cut there and followed by `...`.

| Check | Message |
|---|---|
| JSON syntax or limits | `invalid JSON: offset <n>: <parser message>` ([`src/json.h`](../src/json.h)), e.g. `invalid JSON: offset 1: unexpected end of input` |
| Not an object | `expected a JSON object` |
| A top-level key other than `template` and `slots` (first in document order) | `unknown key '<name>'` |
| No `template` | `missing key 'template'` |
| No `slots` | `missing key 'slots'` |
| `template` not a string | `'template' must be a string` |
| `slots` not an object | `'slots' must be an object` |
| Template in no `ingest` list | `template '<name>' is not ingestible` |
| Then each slot, in document order: | |
| Not a slot of the template | `template '<template>' has no slot '<name>'` |
| Array for a single-field slot | `slot '<name>': array for a single-field slot` |
| Not an array for a multislot | `slot '<name>': expected an array for a multislot` |
| `true` or `false` | `slot '<name>': booleans are not allowed` |
| `null` | `slot '<name>': null is not allowed` |
| Array inside a multislot array | `slot '<name>': nested arrays are not allowed` |
| Object other than `{"sym":"<string>"}` | `slot '<name>': expected {"sym":"<symbol>"} for an object` |
| Bad symbol | `slot '<name>': invalid symbol` |
| `type` constraint | `slot '<name>': value type not allowed by the slot` |
| `range` constraint | `slot '<name>': value outside the slot's range` |
| `allowed-...` constraint | `slot '<name>': value not among the slot's allowed values` |
| `cardinality` constraint | `slot '<name>': value count outside the slot's cardinality` |
| Another constraint | `slot '<name>': value rejected by the slot's constraints` |
| Then, in deftemplate order: a `(default ?NONE)` slot not given | `slot '<name>' requires a value` |
| CLIPS refuses the assert | `cannot assert the fact` |

`host->post_fact` returns -1 with `EINVAL` only for the JSON check (first row); a fact rejected later is logged when the cycle drains it as `clpdaemon: <plugin>: post_fact rejected: <message>` on the daemon log stream, for example `clpdaemon: poster: post_fact rejected: template 'order' has no slot 'bogus'`. `<plugin>` is the plugin whose library called `post_fact` (`?` when the call came from code outside every plugin library, for example a helper library the plugin loaded).

## Internal interface

`src/ingest.h`: `clpd_ingest_post` (any thread; 0, or -1 with `EINVAL`, `EAGAIN` when the queue is full, `ENOMEM`), `clpd_ingest_queue_length` (any thread), `clpd_ingest_check` (engine thread: validates without asserting and returns the exact message, for the admin socket's synchronous `inject`, which then posts), `clpd_ingest_assert` and `clpd_ingest_drain`.
