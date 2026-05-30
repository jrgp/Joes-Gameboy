# Agent Instructions for cgb

This is a Game Boy DMG emulator written in C (`gb.c`). When making changes, you
must satisfy all four gates below on every commit. **Never trade a regression in
Blargg or Mooneye for extra GBMicro passes.**

---

## Required Gates (all must pass before committing)

### 1. Strict warning-free build

```bash
make
```

Flags in use: `-Wall -Wextra -Wconversion -Wsign-conversion -Wshadow -Wundef -Werror`

Zero warnings, zero errors required. Fix warnings with proper casts — do **not**
suppress them.

### 2. ASan / UBSan build

```bash
make asan          # produces ./gb_asan
```

Same strict flags plus `-fsanitize=undefined,address`. Must compile cleanly.
Treat optimization-dependent behavior as a UB bug — fix the source, not the flags.

### 3. Blargg test suites — target: 31/31

```bash
bash tests/run_tests.sh
```

All 31 ROMs across `cpu_instrs`, `mem_timing`, `mem_timing-2`, `instr_timing`,
`halt_bug`, `interrupt_time`, and `oam_bug` must pass.

### 4. Mooneye test suite — target: 75/75

```bash
bash tests/run_mooneye.sh
```

All 75 ROMs must pass.

---

## GBMicro test suite (improvement target, not a gate)

```bash
bash tests/run_gbmicrotest.sh
```

513 ROMs; results reported as `Passed / Failed / Unknown`. Improving this score
is the main ongoing work, but **never at the cost of regressions in gates 1–4**.

Current known-good baseline: **302/513** (from commit `9b98849`, after
re-adding the `--gbmicrotest` harness at `51fdbff`).

The test script passes `--headless --gbmicrotest --cycles 500000` to the binary
and reads `0xFF82` for the pass/fail result after the cycle limit.

---

## Workflow rules

1. **Run all four gates before every commit.** If any gate fails, fix it first.
2. **git revert to the last green commit** if you accidentally regress Blargg or
   Mooneye. Do not try to patch forward — bisect and revert.
3. **Commit after every net improvement.** If GBMicro improves (even by one
   test) and all gates are green, commit and push so progress isn't lost.
4. **No logic changes in warning fixes.** When resolving compiler warnings, only
   add explicit casts. Do not restructure or reorder code.
5. **One concern per commit.** Timing fix commits should not mix in refactors or
   unrelated changes.

---

## Build targets

| Target      | Command       | Output    | Purpose                        |
|-------------|---------------|-----------|--------------------------------|
| Default     | `make`        | `./gb`    | Normal build, strict warnings  |
| ASan/UBSan  | `make asan`   | `./gb_asan` | Sanitizer build              |
| Clean       | `make clean`  | —         | Remove both binaries           |

---

## Key source files

| File           | Purpose                                      |
|----------------|----------------------------------------------|
| `gb.c`         | Single-file emulator (~5100 lines)           |
| `bits.h`       | Bit-manipulation macros (`bit_set`, etc.)    |
| `constants.h`  | Interrupt constants and priority table       |
| `Makefile`     | Build rules with strict flags                |
| `tests/`       | Test scripts and ROM directories             |
| `roms/blargg/` | Blargg test ROMs                             |
| `roms/mooneye/`| Mooneye test ROMs                            |
| `roms/gbmicrotest/` | GBMicro test ROMs (513 ROMs)           |

---

## Running a single ROM headlessly

```bash
# Blargg-style (serial output → stdout):
./gb --headless --cycles 100000000 roms/blargg/cpu_instrs/01-special.gb

# GBMicrotest-style (0xFF82 pass/fail register):
./gb --headless --gbmicrotest --cycles 500000 roms/gbmicrotest/add_hl_timing.gb
```
