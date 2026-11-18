# Vendored CLIPS 6.4.2 core

| | |
|---|---|
| Upstream | CLIPS 6.4.2 (January 2025), https://www.clipsrules.net/ |
| Archive | `clips_core_source_642.zip` from https://sourceforge.net/projects/clipsrules/files/CLIPS/6.4.2/ |
| Archive SHA-256 | `55369287510f8826a66f9ce162ff0213bfdef42fc85736b82049ab7dca8315c3` |
| License | MIT No Attribution, see `LICENSE.txt` (upstream `readme.txt`) |

## Contents

`src/` holds every `core/*.c` and `core/*.h` file from the archive, **byte-for-byte unmodified**. The upstream `makefile` and `makefile.win` are not copied; `CMakeLists.txt` reproduces the upstream Linux `release` build.

## Build differences from upstream

| Item | Upstream makefile | Here | Why |
|---|---|---|---|
| `main.c` | linked into the `clips` command-line executable | excluded from the library | clpdaemon provides its own `main`; the upstream library (`libclips.a`) also excludes it |
| Position-independent code | not set | `-fPIC` | plugins and future shared-library builds (T6) |
| Optimization | `-O3` | from `CMAKE_BUILD_TYPE` | consistent with the rest of the project |
| Warnings | upstream warning set | silenced (`-w`) | vendored code is not modified to fix warnings |

`-std=c99`, `-fno-strict-aliasing`, `-DLINUX` and `-lm` match upstream.

## Updating

Replace `src/` with the `core/*.c` and `core/*.h` files of the new release, update `LICENSE.txt` and this table, and verify with `diff -r` against the extracted archive. Never edit files under `src/`.
