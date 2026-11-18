# Trace

The daemon writes a trace of the facts it asserts, modifies and retracts, the rules it fires, and the output of the engine (FR-13 to FR-16, decision D3). Two formats are available, each in its own file, and both can be on at once:

| Key | Format |
|---|---|
| `trace_text` | Human-readable text, one event per line |
| `trace_jsonl` | JSON Lines, one JSON object per line |

An empty or missing key disables that format. With both disabled, the daemon writes no trace. Paths are used as given; a relative path is relative to the daemon's working directory.

## Files

- Files are opened in append mode at startup, before the CLIPS environment is created (FR-2). `--check` opens them too (creating missing files, writing nothing), so a path the daemon cannot write fails the check.
- A file that cannot be opened stops startup with exit code 1:
  `clpdaemon: <path>: cannot open: <system error>`
- Every line is flushed when it is written. A line that cannot be written is lost (it may be left partial), and the first failure on a file is reported until a reopen succeeds:
  `clpdaemon: <path>: cannot write: <system error>`
- `SIGHUP` reopens both files by path, for log rotation: move the file away, then send `SIGHUP`, and the daemon creates a new file. A file that cannot be reopened keeps its previous stream, and the daemon reports it and keeps running:
  `clpdaemon: <path>: cannot reopen: <system error>`

## Event sources

The daemon connects the trace to the CLIPS environment after the rule packages are loaded and before the single `Reset`, so the facts asserted by `Reset` (deffacts) are traced, while package load diagnostics still go to stderr. Without a trace file nothing is connected and engine output goes to the console.

- **Output**: a router takes the logical names `STDOUT` (also `t`), `STDERR` and `STDWRN`. Text is cut into lines at `\n`; each line is one `output` event with its stream name. A partial line (no `\n` yet) is written at the end of every engine cycle, when a rule starts or ends firing, and at shutdown. With a trace, engine output no longer reaches the console.
- **Facts** (`watch` includes `facts`): `assert`, `retract` and `modify` events. A `modify` is one event with the fact after the change (same index); the retract and assert CLIPS performs inside it are not traced. A modify whose result already exists as another fact only removes the old one, and is traced as its `retract`.
- **Rules** (`watch` includes `rules`): one `fire` event before the actions of the rule run, with the module, the rule and the indices of the facts it matched in pattern order. Patterns that match no fact (`not`, `exists`) and objects (COOL instances) are left out; a rule without patterns has no facts.
- CLIPS `watch` itself stays off: its `==>`, `<==` and `FIRE` text would repeat the events above.

`wf_id` attribution:

- A fact event carries the value of the fact's `wf-id` slot: the symbol or string contents, or the number as CLIPS prints it. Ordered facts, templates without a `wf-id` slot, the symbol `nil` (the slot default, meaning not set; the string `"nil"` is a value) and other values (for example a multislot) have none.
- A `fire` event carries the `wf_id` of the first matched fact that has one.
- Output written while a rule fires carries that rule's `wf_id` (also when rules are not watched); output written outside a firing has none.

If the hooks cannot be installed the daemon stops with exit code 1:
`clpdaemon: cannot attach the trace to the CLIPS environment`

## Line formats

Every line carries a UTC timestamp with milliseconds (`2026-11-18T10:37:25.641Z`), the `node_id`, and the `wf_id` of the workflow instance when it is known.

### Text

```
<timestamp> <node_id> <wf_id or -> <event>
```

| Event | Text |
|---|---|
| Fact asserted | `FACT+ f-<index> <fact>` |
| Fact retracted | `FACT- f-<index> <fact>` |
| Fact modified | `FACT~ f-<index> <fact after the change>` |
| Rule fired | `FIRE <module>::<rule>` followed by ` f-<i>,f-<j>...` when the rule matched facts |
| Output | `<stream> <text>` |

Control bytes in a value are written as `\xHH`, so one event is always one line.

Example:

```
2026-11-18T10:37:25.641Z n1 wf-42 FACT+ f-12 (order (wf-id wf-42) (qty 3))
2026-11-18T10:37:25.642Z n1 wf-42 FIRE ship@1::send f-12,f-13
2026-11-18T10:37:25.642Z n1 - STDOUT hello
```

### JSON Lines

Every object starts with `ts`, `node_id`, `wf_id` (left out when unknown) and `event`, followed by the fields of that event:

| `event` | Fields |
|---|---|
| `assert`, `retract`, `modify` | `fact` (index), `template`, `text` (the fact; for `modify`, after the change) |
| `fire` | `module`, `rule`, `facts` (indices of the matched facts, possibly empty) |
| `output` | `stream`, `text` |

Example:

```
{"ts":"2026-11-18T10:37:25.641Z","node_id":"n1","wf_id":"wf-42","event":"assert","fact":12,"template":"order","text":"(order (wf-id wf-42) (qty 3))"}
{"ts":"2026-11-18T10:37:25.642Z","node_id":"n1","wf_id":"wf-42","event":"fire","module":"ship@1","rule":"send","facts":[12,13]}
{"ts":"2026-11-18T10:37:25.642Z","node_id":"n1","event":"output","stream":"STDOUT","text":"hello"}
```

Strings use the JSON escapes (`\"`, `\\`, `\n`, `\t`, `\u00XX` for other control bytes). Bytes that are not valid UTF-8 are written unchanged.
