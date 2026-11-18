# C plugins

A plugin is a shared library, written in C, that adds functions to the CLIPS engine through the CLIPS C interface (`AddUDF` and the `UserDefinedFunction` contract of the CLIPS 6.4 *Advanced Programming Guide*, https://www.clipsrules.net/) and can observe the engine lifecycle (FR-17, FR-18, FR-19). The contract is the public header [`include/clpd_plugin.h`](../include/clpd_plugin.h), version 1 (`CLPD_PLUGIN_ABI`). Anything else in the daemon (the `clpd_*` symbols) is internal and not part of the contract.

## Configuration

`plugins` in the configuration file ([`configuration.md`](configuration.md)) is an ordered list of paths. Each path is used as given (a relative path is relative to the daemon's working directory) and must contain a `/`, so that `dlopen` never searches the library paths: plugins load only from configured paths (NFR-2). Use `./name.so` for a file in the working directory. There is no per-plugin configuration in version 1.

## Contract

The library exports one symbol:

```c
const clpd_plugin *clpd_plugin_get(void);
```

It returns a descriptor that must stay valid while the library is loaded (normally a `static const` object):

| Field | Meaning |
|---|---|
| `abi_version` | Must be `CLPD_PLUGIN_ABI` (1). Any other value is rejected. |
| `struct_size` | `sizeof(clpd_plugin)` as compiled by the plugin; a smaller value is rejected. |
| `name` | 1-64 characters from `[A-Za-z0-9._-]`, unique among the loaded plugins; prefixes the plugin's log lines. |
| `init(host, env, &state)` | Required. Registers the UDFs and stores the plugin state in `*state` (`NULL` on entry). Returns 0 on success; anything else aborts startup. |
| `after_load(state)` | Optional. All rule packages are loaded; called before `Reset`. |
| `cycle_begin(state)` | Optional. Called before the `Run` of every cycle. |
| `cycle_end(state, ok)` | Optional. Called after the `Run` of every cycle; `ok` is 0 when that `Run` stopped on a CLIPS evaluation error (for example a UDF that rejected an argument or called `UDFThrowError`), 1 otherwise, including a `Run` halted by a stop signal or by the rule limit. |
| `on_ack(state, ack)` | Reserved for the exchange acknowledgements (T13/T14). The daemon never calls it in version 1; set it to `NULL`. |
| `shutdown(state)` | Optional. Frees the state. Called once for every plugin whose `init` succeeded. |

The daemon passes `init` a host structure, valid until `shutdown` returns:

| Field | Meaning |
|---|---|
| `struct_size` | `sizeof(clpd_host)` of the daemon. Later revisions only append fields. |
| `log(plugin, fmt, ...)` | Writes `clpdaemon: <name>: <message>` to the daemon log stream (stderr; the original stderr while fds 1 and 2 are redirected to the trace). `plugin` is the plugin's own descriptor; `fmt` follows `printf`. Engine thread only. |
| `post_fact(fact_text)` | Queues a fact for the engine thread, written as fact JSON ([`ingest.md`](ingest.md)), such as `{"template":"order","slots":{"wf-id":"w1","qty":3}}` (NUL-terminated). Callable from any thread, also from `init`. Returns 0 when queued; -1 with `errno` `EINVAL` when the text is `NULL` or not JSON within the limits, `EAGAIN` when the queue (1024 facts) is full, `ENOMEM`. The template and slots are checked when the next cycle asserts the fact; a rejected fact is logged as `clpdaemon: <name>: post_fact rejected: <message>`. |
| `node_id` | The `node_id` of the configuration. |

## Lifecycle

1. The daemon creates the CLIPS environment.
2. For each path, in configured order: `dlopen(path, RTLD_NOW | RTLD_LOCAL)`, `clpd_plugin_get`, descriptor checks, `init`. The first failure aborts startup (exit 1) after the plugins already initialized are shut down in reverse order.
3. The rule packages load ([`packages.md`](packages.md)); package files may call the functions the plugins registered. A package error shuts the plugins down and aborts startup.
4. `after_load` of every plugin, in configured order; then `Reset` and the event loop.
5. Every cycle: `cycle_begin` of every plugin in configured order, `Run`, then `cycle_end` in configured order.
6. On `SIGTERM` or `SIGINT`, after the last cycle: `shutdown` in reverse configured order, while the environment still exists; then the environment is destroyed, and only then are the libraries unloaded (`dlclose`), so no registered function ever points into an unloaded library.

With `--check`, steps 1-3 run and then `shutdown` (no `after_load`, no cycle). A plugin whose `init` failed is not shut down, but its library stays loaded until the environment is destroyed, because it may have registered functions before failing.

## Threads

Every hook runs on the engine thread, the only thread that may call CLIPS (NFR-1). A plugin that starts its own threads must not call CLIPS or `host->log` from them; `host->post_fact` is the only host function callable from any thread (FR-19). Stop those threads in `shutdown`.

The daemon blocks `SIGTERM`, `SIGINT` and `SIGHUP` before it loads the plugins, so a thread started from a hook inherits that mask and the signals reach only the daemon's event loop: once the loop runs, a stop signal ends in the graceful stop above, with `shutdown`. A plugin thread must not unblock them. A stop signal that arrives during `init` or package loading waits until the event loop starts, which then stops before the first cycle; if startup fails first, the daemon exits 1 with the startup error (after the plugins' `shutdown`) and the signal is not reported. Under `--check` the signals keep their default action.

## Output

With a trace configured, what a plugin writes to file descriptors 1 and 2 (for example with `printf`), and what it writes through CLIPS routers, goes to the trace ([`trace.md`](trace.md), FR-16) from `after_load` until `shutdown` of a normal stop. The capture starts after the plugins and packages loaded, so output from `init`, and from `shutdown` when startup fails, goes to the console, not the trace. `host->log` lines never go to the trace.

## Messages

Every message goes to stderr and is followed by exit 1. Only the first failing plugin is reported.

| Situation | Message |
|---|---|
| Path without `/` | `clpdaemon: <path>: plugin path must contain a '/'` |
| Same path listed again | `clpdaemon: <path>: plugin path listed more than once` |
| `dlopen` fails | `clpdaemon: <path>: cannot load: <dlerror text>` |
| No `clpd_plugin_get` symbol | `clpdaemon: <path>: missing clpd_plugin_get` |
| `clpd_plugin_get` returns `NULL` | `clpdaemon: <path>: clpd_plugin_get returned NULL` |
| ABI mismatch | `clpdaemon: <path>: ABI version <n>, expected 1` |
| Descriptor too small | `clpdaemon: <path>: descriptor size <n>, expected at least <size>` |
| Bad name | `clpdaemon: <path>: plugin name: expected 1-64 characters from [A-Za-z0-9._-], got '<name>'` |
| Name already loaded (also the same library under another path) | `clpdaemon: <path>: plugin <name> is already loaded from <first path>` |
| `init` is `NULL` | `clpdaemon: <path>: descriptor has no init` |
| `init` fails | `clpdaemon: <path>: init failed` |

For a missing file, glibc's `dlerror` text repeats the path, for example `clpdaemon: ./x.so: cannot load: ./x.so: cannot open shared object file: No such file or directory`. The library is opened with `RTLD_NOW`, so a reference no loaded object defines fails here too, for example `clpdaemon: ./x.so: cannot load: ./x.so: undefined symbol: clpd_log` for a plugin that calls a daemon internal.

## Building a plugin

A plugin includes `clpd_plugin.h` (which includes the CLIPS `clips.h`), compiles with the CLIPS headers and the same CLIPS configuration macro as the daemon (`-DLINUX`), and is linked as a shared library **without** CLIPS: the daemon links every CLIPS object and exports all of them (`-rdynamic`), and the plugin's CLIPS references resolve against the executable at `dlopen`. The daemon's own `clpd_*` symbols are not exported (the linker version script `src/clpd_exports.ver`), so a plugin cannot depend on them.

```sh
cc -std=c99 -fPIC -shared -DLINUX -Iinclude -isystem vendor/clips/src \
  -o my_plugin.so my_plugin.c
```

`-isystem` marks the CLIPS headers as system headers, so warning options such as `-Wpedantic` report the plugin's code and not the CLIPS headers.

Inside this repository, `clpd_plugin_module(<target> <sources>)` (in `src/CMakeLists.txt`) builds `<build>/plugins/<target>.so` that way. The example [`examples/plugins/clpd_example.c`](../examples/plugins/clpd_example.c) builds as `<build>/plugins/clpd_example.so` and registers:

| Function | Result |
|---|---|
| `(clpd-example-add <integer> <integer>)` | their sum |
| `(clpd-example-greet <string-or-symbol>)` | `"hello, <name>"` |
| `(clpd-example-cycles)` | engine cycles begun so far |

It logs `init`, `after_load`, `cycle_begin 1`, `cycle_end <n> ok=<0|1>` for the first cycle and whenever `ok` changes, and `shutdown`. Register functions only from `init`: CLIPS resolves function names when package files are parsed.
