# AGENTS.md

## Project Status

This repository contains architecture research and the Phase 0/1 foundation
for a native, Emacs-compatible terminal editor. The current implementation
includes the headless Document core and terminal platform probes.

Read these documents before making architectural or implementation changes:

- `docs/architecture-research.md` - primary design baseline
- `docs/qemacs-architecture-research.md` - QEmacs comparison and reference

## Technical Baseline

- Target C17 with the vendored `nob.h` build system.
- Support GCC, Clang, and MSVC.
- Use vendored `utf8proc` for Unicode handling.
- Keep platform code behind narrow boundaries: POSIX uses `termios`, `poll`,
  and VT sequences; Windows uses VT output and `ReadConsoleInputW`.
- Do not scatter `_WIN32` conditionals through core editor code.
- Prefer opaque public types and keep concrete structures private.
- Check size arithmetic and allocation results at trust and memory boundaries.
- Configure warnings, include paths, and compile definitions per target.

## Change Guidelines

- Treat `docs/architecture-research.md` as the current source of truth. Update it
  when an implementation decision intentionally diverges from the design.
- Prefer the C standard library, operating-system APIs, and existing project
  code before adding dependencies or abstractions.
- Keep changes small and within the module that owns the behavior.
- Fix shared root causes rather than adding guards to individual callers.
- Add one focused runnable check for each nontrivial behavior. Expand coverage
  only when the risk or shared contract warrants it.
- Preserve user changes and avoid unrelated formatting or refactoring.

## Build And Validation

Bootstrap the vendored `nob.h` build program, then use its targets:

```sh
cc -std=c17 nob.c -o nob
./nob build
./nob test
./nob sanitize
./nob clean
```

On Windows from an MSVC developer prompt, bootstrap with:

```bat
cl /nologo /std:c17 nob.c /Fe:nob.exe
nob.exe build
nob.exe test
```

`sanitize` requires GCC or Clang. In ptrace-restricted environments where
LeakSanitizer cannot start, use `ASAN_OPTIONS=detect_leaks=0 ./nob sanitize`;
this still runs AddressSanitizer and UndefinedBehaviorSanitizer checks other
than leak detection. Keep this section synchronized with verified commands.
