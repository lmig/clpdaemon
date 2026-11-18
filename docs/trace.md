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
- Every line is flushed when it is written.
- `SIGHUP` reopens both files by path, for log rotation: move the file away, then send `SIGHUP`, and the daemon creates a new file. A file that cannot be reopened keeps its previous stream, and the daemon reports it and keeps running:
  `clpdaemon: <path>: cannot reopen: <system error>`

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
