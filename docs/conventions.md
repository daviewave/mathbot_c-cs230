# CS230 C project conventions

This document is the contract every project repository in `cs230/` follows. Each
project copies it into its own `docs/conventions.md` so the repository is
self-contained. Course rubric requirements always win over this document; where
they conflict, the project's `docs/design.md` records the deviation and why.

## 1. Repository layout

Every project lives at `cs230/<N>/<name>_c-cs230/` and is its own git repository.

```
Makefile              drives everything (see section 3)
README.txt            the graded deliverable: overview, rubric map, video URL placeholder
.gitignore            build/, dist/, output artifacts, core dumps, editor files
docs/
  spec.md             the project spec, extracted verbatim from the .docx
  research.md         topic research: what was learned, from where, what it changed
  design.md           data structures, algorithms, every non-obvious decision and WHY
  plan.md             the implementation plan (writing-plans format, checkbox tasks)
  conventions.md      copy of this file
src/                  the sources that get submitted (exact file names the spec demands)
test/
  unit/               C unit tests, one file per source module: test_<module>.c
  unit/check.h        the shared assertion harness (section 5)
  e2e/                shell scripts that drive the built binary end to end
  e2e/cases/          stdin scripts and expected outputs for the e2e runner
  run_tests.sh        builds and runs every unit and e2e test; non-zero exit on any failure
build/                ignored; objects, test binaries, the program binary
dist/                 ignored; flat submission bundle produced by `make dist`
```

Single-file projects (1, 4, 5) still use this layout: the one deliverable file
sits in `src/`, and tests exercise it with the include trick in section 5.

## 2. Language and compiler

- Standard: `-std=c99`. Project 2's spec requires it explicitly; the others
  compile on the course VM with the same flag, so it is the floor everywhere.
- POSIX functions (`open`, `read`, `pthread_*`, `sem_*`, sockets, `nanosleep`)
  need `-D_POSIX_C_SOURCE=200809L` on the compile line under `-std=c99`.
  Project 4 also needs `-pthread` on both compile and link lines.
- Development warning set, mandatory and treated as errors:
  `-Wall -Wextra -Wpedantic -Wshadow -Wstrict-prototypes -Wmissing-prototypes
  -Wconversion -Wvla -Werror`
  Project 1 may drop `-Wvla` only if it uses a variable length array, which its
  spec explicitly allows.
- `make check` runs `gcc -fanalyzer` over the sources. The result must be clean.
- Sanitizer runtimes (`libasan`, `libubsan`) are NOT installed on this machine
  and valgrind is absent. Memory discipline is enforced by review and by
  `-fanalyzer`, so every `malloc` must have a visibly paired `free` on every path.
- Debug build: `-O0 -g3`. Release build: `-O2`.

## 3. Makefile contract

Every Makefile exposes exactly these targets. Tests and sub-agents depend on them.

| Target  | Does |
| ------- | ---- |
| `all`   | default; builds the release binary into `build/` |
| `debug` | builds with `-O0 -g3` into `build/` |
| `test`  | builds, then runs `test/run_tests.sh`; exits non-zero on any failure |
| `check` | static analysis with `gcc -fanalyzer` over every file in `src/` |
| `run`   | builds and runs the program with sensible default arguments |
| `dist`  | assembles `dist/` as the flat Gradescope bundle (section 7) and proves it builds |
| `clean` | removes `build/` and `dist/` and any generated output files |

Rules:
- `CC ?= gcc`, flags in `CFLAGS`, link flags in `LDFLAGS`, so a reviewer can override.
- Pattern rules, not one rule per file. Header dependencies via `-MMD -MP`.
- No recursive make. Phony targets declared.
- Comments in the Makefile are welcome and expected (config files are exempt
  from the comment rule): explain each flag group and each target.
- Object files and binaries never land next to sources.

## 4. Code style

- Four-space indentation, braces on the same line as the construct (K&R),
  `else` on its own line after the closing brace is NOT used: `} else {`.
  One consistent style is what the rubric grades; this is the one.
- Every function that is not `main` and not shared through a header is
  `static`. Every header has an include guard named `<FILE>_H`.
- Names: `snake_case` for functions and variables, `UPPER_SNAKE` for
  macros and enum constants, `PascalCase` for `typedef`ed structs.
  Names say what the thing is or does; no abbreviations a stranger could not expand.
- Functions do one thing and fit on a screen. A block that needs a comment
  to be followed becomes a named function (see `sub-functions` rule below).
- No global mutable state unless the spec demands it (project 4 does).
  Where the spec demands globals they are `static` and documented once.
- Magic numbers become `enum` constants or `#define`s with names.
- Every input is validated and every system call's return value is checked.
  Error messages go to `stderr`; program exit codes are `EXIT_SUCCESS`/`EXIT_FAILURE`.
- Prefer `size_t` for sizes and indices, fixed-width or `long long` where range matters.
- `scanf` family only with field widths or for `%d`-style numeric reads whose
  failure path drains the rest of the line; `gets` never; `fgets` + parse preferred.

## 5. Comments: the rubric compromise

The user's repo-wide rule is "the why lives in `docs/`, not in code; narration
comments are deleted; a block that wants a comment becomes a function." The
course rubrics award points for documented functions and documented variables.
The reconciliation, applied everywhere:

- Every function in `src/` carries a short header comment: one line of purpose,
  then `@param`/`@return` lines only when the signature does not already say it.
  This is API documentation, which the comment rule protects, not narration.
- Non-obvious variables (especially the spec-mandated globals in project 4) get a
  one-line comment at their declaration.
- Inside function bodies there are no step-by-step narration comments. If a body
  needs them, extract named functions until it reads as English.
- Algorithm rationale (why greedy AI, why cancel-then-join, why 4 KiB read chunks)
  lives in `docs/design.md`; the code may carry a one-line pointer to it.
- The `=== start N` / `a)` refactoring markers from the sub-functions rule are
  scaffolding and must never be committed.

## 6. Testing

`make test` must prove the rubric. "It compiled" is not evidence.

### Unit tests (`test/unit/`)

`test/unit/check.h` is the whole harness; no external framework:

```c
#ifndef CHECK_H
#define CHECK_H
#include <stdio.h>
#include <string.h>

static int check_failures = 0;
static int check_count = 0;

#define CHECK(cond) do { \
    check_count++; \
    if (!(cond)) { \
        check_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define CHECK_EQ_INT(a, b) do { \
    long long check_a_ = (long long)(a), check_b_ = (long long)(b); \
    check_count++; \
    if (check_a_ != check_b_) { \
        check_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (%lld != %lld)\n", \
                __FILE__, __LINE__, #a, #b, check_a_, check_b_); \
    } \
} while (0)

#define CHECK_EQ_STR(a, b) do { \
    const char *check_a_ = (a), *check_b_ = (b); \
    check_count++; \
    if (strcmp(check_a_, check_b_) != 0) { \
        check_failures++; \
        fprintf(stderr, "FAIL %s:%d: %s == %s (\"%s\" != \"%s\")\n", \
                __FILE__, __LINE__, #a, #b, check_a_, check_b_); \
    } \
} while (0)

#define CHECK_REPORT(name) do { \
    fprintf(stderr, "%s: %d checks, %d failures\n", name, check_count, check_failures); \
    return check_failures == 0 ? 0 : 1; \
} while (0)

#endif
```

Each `test_<module>.c` has its own `main` that calls test functions and ends with
`CHECK_REPORT("test_<module>")`. Multi-file projects link tests against the
module objects (never against `main.o`). Single-file projects use the include trick:

```c
#define main program_main          /* rename the real main out of the way */
#include "../../src/Santorini.c"   /* static functions become testable */
#undef main
#include "check.h"
```

Unit tests cover every pure function: board updates, move validation, AI
choice, tokenizers, BST insert/traverse, formatting, protocol parsing and
arithmetic, linked-list add/drop, room linking.

### End-to-end tests (`test/e2e/`)

Shell scripts (bash, `set -euo pipefail`) that run the built binary under
`timeout`, feed stdin from `test/e2e/cases/<name>.in`, and diff stdout against
`test/e2e/cases/<name>.expected` or assert properties with grep. Rules:

- Any randomness in the program must be seedable through an environment
  variable named `<PROJECT>_SEED` (e.g. `CLUE_SEED`) so e2e cases are
  deterministic. Without the variable the program seeds from time.
  Project 1 needs no randomness: its AI is deterministic.
- Invalid-input cases are mandatory: bad coordinates, off-board moves,
  occupied squares, unknown commands, missing files, bad ports, EOF on stdin.
- Project 5 tests run against a local mock server (Python, standard library
  only, run with `python3 -I`) that speaks the spec's protocol and deliberately
  splits and coalesces messages across TCP writes.
- Project 4 tests parse the program's output and verify invariants: every
  message carries an id, ids are unique, per-caller message order is correct,
  the connected-line count reconstructed from the trace never exceeds 5, the
  concurrent-operator count never exceeds 2, and wall time tracks the argument.

### `test/run_tests.sh`

Builds the unit test binaries into `build/test/`, runs each, runs each e2e
script, prints a one-line PASS/FAIL per test, and exits non-zero if anything
failed. `make test` calls it and nothing else.

## 7. The submission bundle (`make dist`)

Gradescope wants flat files. `make dist` copies the spec-named sources from
`src/`, `README.txt`, and a generated flat `Makefile` (same flags, no `build/`
prefix, builds in place) into `dist/`, then runs `make -C dist` to prove the
bundle compiles with `gcc -std=c99 -Wall` as the course requires. `dist/` is
ignored by git.

## 8. README.txt

Plain text, made for a grader skimming for rubric items:

1. One-paragraph overview of the implementation.
2. Build and run instructions (`make`, `./…`).
3. A "Requirements map" section: every bullet from the spec's requirement and
   rubric lists, each with the file and function that satisfies it.
4. Design notes worth a grader's attention (AI strategy, data structures).
5. `Video: <VIDEO URL TO BE ADDED>` on its own line.

## 9. Git

- Commit as each unit of work completes: a docs commit, a scaffold commit, then
  one commit per implementation task, test included in the same commit.
- Conventional-commit style subjects: `feat:`, `test:`, `docs:`, `build:`, `fix:`, `refactor:`.
- Never push. Never add `Co-Authored-By` trailers or any mention of Claude,
  Anthropic or AI assistance anywhere in the repository.
- Binaries, objects, `output.txt`, core dumps and `dist/` are never committed.
  Project 1's history already contains two binaries; the rewrite removes them.

## 10. Documentation in `docs/`

- `research.md`: for each core topic, what sources were consulted (URLs), the
  practices adopted, and the pitfalls specifically avoided in this code.
- `design.md`: the data structures, the algorithms, and every decision a
  reviewer could question, with the reason. This is where the "why" lives.
- `plan.md`: the implementation plan the project agent executed.
