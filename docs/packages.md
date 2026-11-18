# Rule packages

A rule package holds the CLIPS constructs of one workflow definition (FR-6). It is a directory listed in the daemon configuration key `packages`, with a manifest named `package.conf` and the `.clp` files it names.

## Manifest

`package.conf` uses the same `key = value` format as the daemon configuration ([`configuration.md`](configuration.md#file-format)): one pair per line, `#` comments, LF or CRLF, comma-separated lists, each key at most once.

```ini
# /etc/clpdaemon/packages/fibre-installation@v3/package.conf
definition  = fibre-installation
version     = v3
files       = templates.clp, rules.clp, functions.clp
lint_exempt = template:catalog, rule:load-catalog
```

| Key | Type | Default |
|---|---|---|
| `definition` | 1-64 characters from `[A-Za-z0-9._-]` | required |
| `version` | 1-64 characters from `[A-Za-z0-9._-]` | required |
| `files` | non-empty list of paths relative to the package directory, loaded in order | required |
| `lint_exempt` | list of `template:<name>` and `rule:<name>` excluded from the `wf-id` lint | empty |

The package is identified as `definition@version` (for example `fibre-installation@v3`).

## Loading

At startup (FR-2), after the configuration and the CLIPS environment, the daemon loads the packages in the order of `packages`:

1. It reads the manifest and creates the module `(defmodule definition@version)`; package files must not declare a `defmodule` of their own.
2. It loads `files` in order into that module. Loading a package stops at its first failed file.
3. It runs the `wf-id` lint on the package (FR-7, see below).

Every package is checked even when an earlier one failed, so one run reports all errors; any error makes the daemon exit 1. With `--check`, the daemon stops after loading and linting. Otherwise it calls `Reset` once and enters the event loop.

Before each cycle the daemon replaces the focus stack with MAIN under every package module, the first package on top, so one `Run` fires the rules of all packages (a rule of a non-MAIN module only fires with focus).

## Messages

Manifest errors go to stderr; every error in the file is reported, one per line, in file order, followed by the missing keys.

| Situation | Message |
|---|---|
| Manifest cannot be opened or read | `clpdaemon: <dir>/package.conf: cannot open: <system error>` (or `cannot read`) |
| Syntax, NUL byte, unknown or repeated key | same messages as the daemon configuration |
| Bad `definition` or `version` | `clpdaemon: <file>:<line>: <key>: expected 1-64 characters from [A-Za-z0-9._-], got '<value>'` |
| Empty `files` | `clpdaemon: <file>:<line>: files: expected at least one file` |
| Absolute path in `files` | `clpdaemon: <file>:<line>: files: '<path>' must be relative to the package directory` |
| Empty list item | `clpdaemon: <file>:<line>: <key>: empty list item` |
| Bad exemption | `clpdaemon: <file>:<line>: lint_exempt: expected template:<name> or rule:<name>, got '<item>'` |
| Required key absent | `clpdaemon: <file>: missing required key '<key>'` |
| Package file cannot be opened | `clpdaemon: <file>: cannot open: <system error>` |
| CLIPS error or warning while loading | `clpdaemon: <definition@version>: clips: <CLIPS message line>`, one line per non-blank line CLIPS writes (CLIPS names the file and line) |
| Package file failed to load | `clpdaemon: <file>: load failed`, after the CLIPS lines |
| `defmodule` in a package file | `clpdaemon: <file>: defmodule is not allowed in package files` |
| Same `definition@version` twice | `clpdaemon: <dir>: package <definition@version> is already loaded from <first dir>` |
