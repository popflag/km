# km

[English](README.md) | [Chinese](README_zh.md)

`km` is a native C17 terminal text editor with Emacs-style commands. It
focuses on reliable UTF-8 editing, predictable terminal rendering, and safe
file updates on POSIX and Windows.

The project is usable but still under development. It does not attempt to
provide the GNU Emacs Lisp runtime or full GNU Emacs compatibility.

## Features

- UTF-8 validation and Unicode-aware terminal layout using vendored
  `utf8proc`.
- Grapheme rendering, CJK and tab cell widths, soft wrapping, line numbers,
  and region highlighting.
- Multiple buffers and equal-height windows with an Emacs-style minibuffer.
- Mark and region editing, kill ring, undo/redo, incremental search, and
  literal query replace.
- Rectangle mark, kill, and yank with tab, wide-character, and short-line
  handling.
- Safe file replacement with external-change checks and UTF-8 BOM/EOL
  preservation.
- Native POSIX terminal and Windows Console backends.

## Build

The only build prerequisite is a C17 compiler. Dependencies required by the
editor are vendored in the repository.

### POSIX

```sh
cc -std=c17 nob.c -o nob
./nob build
./build/km [file]
```

GCC and Clang are supported.

### Windows

Run the following from an MSVC developer prompt:

```bat
cl /nologo /std:c17 nob.c /Fe:nob.exe
nob.exe build
build\km.exe [file]
```

Running `km` without a file opens a scratch buffer. One optional file path is
accepted on startup; use `C-x C-f` to open more files.

## Key Bindings

`C-` means Control and `M-` means Alt/Meta. The editor also accepts numeric
prefix arguments through `C-u` and `M--`/`M-0` through `M-9`.

| Action | Binding |
| --- | --- |
| Open file | `C-x C-f` |
| Save buffer | `C-x C-s` |
| Switch buffer | `C-x b` |
| List buffers | `C-x C-b` |
| Close buffer | `C-x k` |
| Exit | `C-x C-c` |
| Run a named command | `M-x` |
| Set mark | `C-SPC` |
| Kill / copy region | `C-w` / `M-w` |
| Yank / rotate kill ring | `C-y` / `M-y` |
| Undo / redo | `C-/` or `C-x u` / `C-x C-r` |
| Search forward / backward | `C-s` / `C-r` |
| Query replace | `M-%` |
| Split / select window | `C-x 2` / `C-x o` |
| Delete current / other windows | `C-x 0` / `C-x 1` |
| Rectangle mark mode | `C-x SPC` |
| Kill / yank rectangle | `C-x r k` / `C-x r y` |
| Cancel the active operation | `C-g` |

Standard Emacs movement bindings such as `C-f`, `C-b`, `C-n`, `C-p`, `M-f`,
`M-b`, `C-v`, and `M-v` are available. Arrow, Home, End, Delete, and Backspace
keys are also supported.

During query replace, use `y` to replace, `n` to skip, `!` to replace all
remaining matches, and `q` or `C-g` to stop.

## Configuration

Compile-time preferences live in [`config.h`](config.h). It controls terminal
styles, tab and width policy, line numbers, ring capacities, input limits, and
custom key bindings.

After changing it, rebuild the editor:

```sh
./nob build
```

There is no runtime configuration parser or reload mechanism.

## Validation

```sh
./nob test
./nob sanitize
./nob bench-layout
```

`sanitize` requires GCC or Clang. If LeakSanitizer cannot start in a
ptrace-restricted environment, run:

```sh
ASAN_OPTIONS=detect_leaks=0 ./nob sanitize
```

CI runs the test suite with GCC, Clang, and MSVC.

## Current Limits

- Only UTF-8 text is supported. Invalid UTF-8 and mixed line endings are
  rejected instead of being converted silently.
- Search and query replace are case-sensitive literal operations; regular
  expressions are not implemented.
- Only horizontal, equal-height window splits and the core rectangle commands
  are implemented.
- There is no Elisp runtime, package ecosystem, GUI, bidirectional text,
  shaping engine, or complete IME preedit UI.
- Compatibility experiments currently use GNU Emacs 31.0.90 development
  builds as a reference; compatibility is limited to documented commands.

See [`docs/architecture-research.md`](docs/architecture-research.md) for the
design baseline and detailed behavioral contracts.
