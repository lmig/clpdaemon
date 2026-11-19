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
ingest      = order, payment
orm         = orm.conf
```

| Key | Type | Default |
|---|---|---|
| `definition` | 1-64 characters from `[A-Za-z0-9._-]` | required |
| `version` | 1-64 characters from `[A-Za-z0-9._-]` | required |
| `files` | non-empty list of paths relative to the package directory, loaded in order | required |
| `lint_exempt` | list of `template:<name>` and `rule:<name>` excluded from the `wf-id` lint | empty |
| `ingest` | list of deftemplate names of the package that facts from outside may assert ([`ingest.md`](ingest.md)) | empty |
| `orm` | path of the ORM mapping file, relative to the package directory (one path: a comma is part of it); needs `database` in the daemon configuration ([`orm.md`](orm.md)) | absent (no mapping) |

The package is identified as `definition@version` (for example `fibre-installation@v3`).

## Loading

At startup (FR-2), after the configuration and the CLIPS environment, the daemon loads the packages in the order of `packages`:

1. It reads the manifest and creates the module `(defmodule definition@version)`; package files must not declare a `defmodule` of their own.
2. It loads `files` in order into that module. Every construct a file defines must belong to the package's module (see [Package module](#package-module)). Loading a package stops at its first failed file.
3. It runs the `wf-id` lint on the package (FR-7, see below).
4. It checks the `ingest` list: every name must be a deftemplate of the package (not an implied one), and not listed by an earlier package (also one that failed to load).
5. When `orm` is set, it reads and checks the mapping file against the package's deftemplates and the mappings of earlier packages ([`orm.md`](orm.md#loading)).

Every package is checked even when an earlier one failed, so one run reports all errors; any error makes the daemon exit 1. With `--check`, the daemon stops after loading, linting and checking the mappings. Otherwise it calls `Reset` once and enters the event loop.

Before each cycle the daemon replaces the focus stack with MAIN under every package module, the first package on top, so one `Run` fires the rules of all packages (a rule of a non-MAIN module only fires with focus).

## Package module

A package file defines its constructs in the package's module only (T9e, L87): a construct in another package's module (`(defrule shop@1::x ...)` in a file of `crm@1`) or in MAIN (`(deffunction MAIN::f ...)`) is a load error, also under `--check`, so one package can never add or replace rules, facts, functions or globals of another. Naming the package's own module (`(defrule crm@1::x ...)`) is allowed.

The check runs for every construct as CLIPS defines it, not only on the current module at the end of the file: a module-qualified name makes that module current in CLIPS, so a file can switch away and back. Each of these is reported, one line per construct, in definition order:

- a new `deftemplate`, `defrule`, `deffacts`, `deffunction`, `defgeneric`, `defmethod` (named by its defgeneric), `defclass` or `definstances` in another module, and the redefinition of an existing one there (also with the same text);
- every global a `defglobal` defines or redefines in another module, also when an initial value switches the current module (`(defglobal ?*a* = (set-current-module MAIN))`), and the constructs a `build` inside an initial value defines;
- every `defmessage-handler` added to or redefined on a class of another module, also on a system class (`(defmessage-handler USER m () ...)`: the system classes belong to MAIN);
- the unqualified constructs that follow a qualified one in another module (CLIPS puts them in the module that became current).

Constructs in a module the file creates with `defmodule` are not reported: the `defmodule` message covers them.

## wf-id lint

Every deftemplate of a package must have a `wf-id` slot, and every rule must bind the `wf-id` slot of all its patterns to one shared variable, so a rule only joins facts of the same workflow instance (FR-7):

```clips
(defrule start ?o <- (order (wf-id ?w) (status new)) (crm-ack (wf-id ?w)) => ...)
```

- Patterns are numbered from 1 in textual order, including those inside `not`, `exists`, `and`, `or`, `forall` and `logical`; `test` and `declare` are not patterns.
- A slot value counts as a variable when it starts with a single-field `?name`; `&` constraints may follow (`?w&:(neq ?w x)`, `?w&~closed`). A constant, `$?w`, `?`, a missing slot, or any or-constraint (`?w|z`: it also matches the other alternative, which may be another instance) does not. Only the first `wf-id` slot of a pattern is checked.
- A rule without its pretty-print form (for example with `conserve-mem` on) cannot be checked and is an error.
- The first checked pattern of a rule sets the expected variable.
- `lint_exempt` names templates (for example reference data shared by all instances) and rules to skip; patterns on an exempt template are not checked. Naming a construct the package does not define is an error.

Lint errors make the package fail like load errors.

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
| Empty `orm` | `clpdaemon: <file>:<line>: orm: expected a file` |
| Absolute path in `orm` | `clpdaemon: <file>:<line>: orm: '<path>' must be relative to the package directory` |
| Name repeated in `ingest` | `clpdaemon: <file>:<line>: ingest: '<name>' listed more than once` |
| Bad exemption | `clpdaemon: <file>:<line>: lint_exempt: expected template:<name> or rule:<name>, got '<item>'` |
| Required key absent | `clpdaemon: <file>: missing required key '<key>'` |
| Package file cannot be opened, or is a directory | `clpdaemon: <file>: cannot open: <system error>` (`Is a directory` for a directory) |
| CLIPS error or warning while loading | `clpdaemon: <definition@version>: clips: <CLIPS message line>`, one line per non-blank line CLIPS writes (CLIPS names the file and line) |
| Package file failed to load | `clpdaemon: <file>: load failed`, after the CLIPS lines |
| `defmodule` in a package file | `clpdaemon: <file>: defmodule is not allowed in package files` |
| A construct outside the package module ([above](#package-module)) | `clpdaemon: <file>: <construct> '<module>::<name>' is outside the package module <definition@version>`, `<construct>` one of `deftemplate`, `defrule`, `deffacts`, `defglobal`, `deffunction`, `defgeneric`, `defmethod`, `defclass`, `definstances`, `defmessage-handler`; `<name>` is `?*<global>*` for a defglobal, the defgeneric for a defmethod, and `<class> <handler> <type>` for a message-handler (`'MAIN::USER hello primary'`) |
| The constructs of a file cannot be checked (a construct type the loader cannot inspect, for example one a plugin added) | `clpdaemon: <file>: cannot check the modules of its constructs` |
| Same `definition@version` twice | `clpdaemon: <dir>: package <definition@version> is already loaded from <first dir>` |
| Exemption names an unknown construct | `clpdaemon: <pkg>: lint: lint_exempt names unknown template '<t>'` (or `unknown rule '<r>'`) |
| Template without `wf-id` | `clpdaemon: <pkg>: lint: template '<t>' has no wf-id slot` |
| Pattern without a `wf-id` variable | `clpdaemon: <pkg>: lint: rule '<r>': pattern <n> (<template>) does not bind wf-id to a variable` |
| Rule without pretty-print form | `clpdaemon: <pkg>: lint: rule '<r>': cannot be checked without its pretty-print form` |
| Different `wf-id` variables | `clpdaemon: <pkg>: lint: rule '<r>': pattern <n> (<template>) binds wf-id to ?<x>, expected ?<w> from pattern <m>` |
| `ingest` names no deftemplate of the package | `clpdaemon: <pkg>: ingest names unknown template '<t>'` |
| `ingest` names a template an earlier package lists | `clpdaemon: <pkg>: ingest names template '<t>', already listed by <first pkg>` |

A file's messages come in this order: the CLIPS lines, then `load failed` or the `defmodule` message, then the constructs outside the package module. Lint messages come after the load messages, in this order: unknown exemptions, templates, then rules in definition order. The `ingest` messages come next, in list order, then the mapping messages ([`orm.md`](orm.md#messages)).
