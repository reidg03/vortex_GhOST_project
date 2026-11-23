# GhOST on Vortex SIMX — Foundation and Working Guide

This is the working reference for the SIMX-only GhOST prototype: what is
implemented, how it fits the Vortex pipeline, its current conservative
boundaries, and how to build, test, and inspect issue order.

## Status at a glance

**Current state: opt-in stateful SIMX scheduler, still under validation.**
Build with `SIMX_GHOST_ENABLE` to use the dependence checker (DC), per-warp
issue buffer (IsB), and age-ordered GhOST Scheduler/ITab before existing issue
arbitration. The path tracks register dependencies, scoreboard hazards,
conservative memory ordering, branch holds, and synchronization/unsupported
instruction fallback. IsB and ITab capacities are compile-time configurable.

The `ghost` regression verifies a load-dependent result and its debug trace
demonstrates a younger independent instruction issuing before the dependent
instruction. The default SIMX path remains available without the define.

Scope is **SIMX only**. No RTL, runtime API, or other-driver GhOST changes are
part of this foundation.

## Files changed or added

| File | Purpose |
|---|---|
| `sim/simx/core.cpp` | Under `SIMX_GHOST_ENABLE`, admits decoded traces from each I-buffer into GhOST and feeds selected candidates through the existing sequencer, scoreboard, structural checks, and issue arbiter. Sends issue, writeback, completion, branch, and flush updates back to GhOST. |
| `sim/simx/ghost.h` | Defines paper-shaped GhOST records and the stateful DC, IsB, Scheduler/ITab, plus the `GhostPipeline` coordinator. Defaults IsB to 8 entries/warp and ITab to 2 candidates/warp. |
| `tests/regression/ghost/Makefile` | Builds the standalone `ghost` regression with explicit host/device dependencies on the shared argument header. |
| `tests/regression/ghost/kernel.cpp` | Tests load RAW, register WAR/WAW ordering, independent-instruction overtaking, and final dependent result calculation. |
| `tests/regression/ghost/main.cpp` | Initializes the input buffer, launches the kernel, and checks every thread's result is 62. |
| `tests/regression/ghost/common.h` | Defines the device/host kernel argument structure. |
| `tests/regression/Makefile` | Registers `ghost` in the regression list and excludes it from RTL/OPAE/XRT runs; it is intended for SIMX only. |
| `ci/trace_order.py` | Parses SIMX debug logs and emits CSV rows for candidate and accepted issue events. |
| `docs/proposals/ghost_simx_foundation_proposal.md` | Historical foundation proposal; this guide documents the current stateful implementation. |

This guide is the expanded, practical reference; the original paper text and
component checklist remain in the parent notes folder. The checklist is a
research aid, **not** a verified Vortex implementation specification.

## SIMX fundamentals and current pipeline

### SIMX is a C++ timing model

SIMX models the core using C++ `SimObject`s and typed channels. It advances
the whole core model in a deterministic order each cycle; it is not literally
the RTL's independently clocked module hierarchy. Instruction traces
(`instr_trace_t`) carry decoded instruction state such as core/warp IDs, PC,
thread mask, decoded instruction pointer, source and destination registers,
functional-unit type, and UUID.

The principal per-core components relevant to this project are:

- `Scheduler`: owns warp lifecycle, per-warp PC and thread mask, barriers,
  control-flow stack, and CTA dispatch. Its `schedule()` method selects a warp
  for **fetch**.
- Per-warp fetch and decode latches, I-cache request/response tracking, and
  `Decoder`.
- One FIFO I-buffer per warp (`ibuffers_` in `Core::Impl`).
- One per-warp `Sequencer`, which expands selected instructions into
  micro-ops where applicable.
- `Scoreboard`: tracks register/FCSR reservations for instructions that have
  issued and have not completed writeback.
- `ibuffer_arbs_`: existing issue arbitration, one arbiter per issue slice.
- `Operands`, dispatchers, functional units, and commit/writeback handling.

### Per-cycle ordering in `Core::Impl::tick()`

In the current SIMX implementation, the core tick calls these operations in
this order:

1. `commit()` handles completed instructions and register writeback.
2. `return_dispatch_credits()` returns available functional-unit credits.
3. `issue()` checks and issues I-buffer instructions.
4. `update_queue_full()` updates functional-unit backpressure state.
5. `decode()` decodes returned instruction words and appends traces to their
   warp's I-buffer.
6. `fetch()` handles I-cache responses and requests.
7. `schedule()` chooses a warp for a future instruction fetch.

This ordering is important: an instruction that enters the I-buffer during
`decode()` is not immediately considered by the already-completed `issue()`
call in that same core tick.

### Instruction flow and module inputs/outputs

```text
Scheduler::schedule() (warp ID, PC, thread mask)
  -> fetch latch
  -> I-cache request / response
  -> decode latch (instruction bits/code)
  -> Decoder (Instr + source/destination/FU/op metadata)
  -> per-warp FIFO I-buffer
  -> GhOST DC -> per-warp IsB -> GhOST Scheduler / ITab
  -> per-warp Sequencer (zero or more micro-ops)
  -> Scoreboard + issue locks + FU-credit checks
  -> ibuffer_arbs_ (grant one eligible warp per issue slice)
  -> Operands (register operands captured)
  -> Dispatcher -> functional-unit input/execute/output
  -> Commit (writeback, scoreboard release, retirement)
```

The fetch scheduler's input is a mask of warps that can fetch (for example,
their I-buffer inflight slots are not saturated). It produces the next warp
trace to fetch. It does **not** choose the decoded instruction that issues.

The actual issue logic is in `Core::Impl::issue()`. With GhOST enabled, the
DC admits at most one decoded I-buffer head per issue tick, round-robin across
warps. The I-buffer retains ownership until admission succeeds; after that,
the IsB owns the trace. The scheduler presents its oldest ready IsB entries;
the core checks them oldest-first and offers the first structurally eligible
one from that warp to arbitration. When GhOST is disabled, the legacy
I-buffer-head path is used.

For either path, issue then:

- Gets the selected trace's current micro-op from the warp's `Sequencer`.
- It checks the existing scoreboard, sequencer startup timing, issue-lock
  ownership, functional-unit queue credits, and optional feature-specific
  structural constraints.
- Eligible warps are marked in `ready_set`.
- The existing arbiter selects one eligible warp.
- If `Operands` accepts the micro-op, SIMX captures operands, records issue
  timing, reserves scoreboard state where needed, and emits the
  `pipeline issue` trace.
- An I-buffer trace is popped when its final micro-op issues. A GhOST entry is
  consumed only after accepted issue; macro-op micro-ops remain pinned to their
  IsB entry until the sequence finishes.

Thus, GhOST belongs at the **issue-side input-selection boundary** in
`Core::Impl::issue()`, after decode has populated the per-warp I-buffer and
before the existing per-warp sequencer / readiness and arbitration path.
`Scheduler::schedule()` is the fetch-side scheduler and should not be changed
to implement this issue scheduler.

## GhOST stages and exact inputs/outputs

The opt-in path is:

```text
per-warp I-buffer head
  -> GhostDependenceChecker
  -> GhostIssueBuffer
  -> GhostScheduler
  -> existing Sequencer
  -> existing scoreboard / structural eligibility checks
  -> existing issue arbiter
  -> Operands and the unchanged downstream pipeline
```

The three modules are plain SIMX C++ state/logic, not new `SimObject`s or
`SimChannel`s. The build define affects SIMX only; RTL and other drivers do
not include this path.

| Stage | Inputs | Outputs and behavior |
|---|---|---|
| `GhostDependenceChecker` (DC) | One decoded I-buffer entry, same-warp IsB snapshot, and current scoreboard source-use bits. | Returns the entry annotated with IsB-bank dependency bits and in-flight source bits. Detects RAW/WAR/WAW; orders branches after older entries; allows load/load pairs but blocks other memory pairs. Invalid/full inputs are not accepted. |
| `GhostIssueBuffer` (IsB) | A checked entry; accepted-issue, writeback, completion, branch-resolution, and flush updates. | Owns accepted traces, assigns per-warp bank/age, reports full/free locations and snapshots. Register dependencies are converted to scoreboard hazards at issue. Issued memory entries remain resident until completion so younger memory operations cannot pass an outstanding conflicting access. Flush frees unissued traces but preserves already-issued memory traces. |
| `GhostScheduler` / ITab | IsB snapshot, per-entry scoreboard readiness, and optional pinned sequencer bank. | Places up to `SIMX_GHOST_ITAB_SIZE` oldest-ready entries in the ITab. `Core::Impl::issue()` checks candidates oldest-first against the existing scoreboard, lock, and FU-queue checks, then offers the first eligible entry per warp to the existing arbiter. |

`Core::Impl::issue()` calls `ghost_admit_one()` after that tick's issue
arbitration. Admission pops the I-buffer and decrements its fetch-inflight
count only after IsB acceptance. Accepted issue updates the selected IsB bank;
writeback releases source dependencies; completion removes retained memory
entries. Branch and ordered-operation state is cleared by the existing SIMX
control/commit events. Candidate presentation alone never consumes an entry.

The existing Scoreboard, issue locks, FU credits, operand capture, functional
units, commit, and retirement remain authoritative. Supported speculative
classes are integer ALU/MDV/Vote/Shfl, FPU instructions without FCSR effects,
read-only `MCYCLE`, loads, stores, and branches. Loads can pass older loads;
other memory pairs are conservative. Branches wait for older IsB entries and
stop younger admission until branch resolution. Atomics, fences, warp-control
operations, macro-ops, FCSR-affecting FPU operations, other CSRs, and unknown
classes use the ordered I-buffer fallback. Barriers/FENCE may reach their
existing drain logic while already-issued memory entries remain in flight.

The per-warp defaults are `SIMX_GHOST_ISB_SIZE=8` and
`SIMX_GHOST_ITAB_SIZE=2`. Both are compile-time SIMX options; the ITab size
must be positive and no larger than the IsB. Each issue slice still grants at
most one warp per cycle; additional ITab candidates are alternatives when an
older ready entry fails a structural eligibility check, not extra issue lanes.

## Issue trace meanings

At debug level 3, SIMX emits:

- `pipeline issue-candidate`: this warp's current trace passed the current
  scoreboard and structural eligibility checks and was included in the set
  offered to the issue arbiter.
- `pipeline issue`: the existing arbiter granted that trace and `Operands`
  accepted it.

These are intentionally different events. A candidate may be retried on a
later cycle; candidate does not mean accepted, executed, or committed. With
multiple eligible warps, there may be multiple candidate rows for a cycle but
only one accepted row per issue slice. The `ghost` test uses one issue slice.

Each event carries the normal trace display fields, including cycle, core,
warp, PC, thread mask, destination/source register metadata where present, and
UUID. The CSV parser associates the `DEBUG Instr` opcode line with the event by
UUID.

## Test kernel and observed issue reordering

The kernel includes these inline assembly operations:

```asm
lw   t0, 0(src_ptr)
addi t2, t0, 5
addi t1, zero, 7
addi t0, zero, 9
add  result, t2, t1
addi t2, zero, 88
```

Each thread loads 50; the first `ADDI` computes 55, and the independent `ADDI`
computes 7. The following write to `t0` must not pass the older read of `t0`
(WAR), and the final write to `t2` must not pass the older write/read sequence
(WAW/WAR). The result operation must therefore produce 62 before `t2` is
overwritten. The host checks every thread's output. With GhOST enabled, the
accepted issue trace should show the independent `ADDI` overtaking the
load-dependent `ADDI`; cycles and PCs can shift when the kernel changes.

## Build, run, and inspect

Commands below assume a configured out-of-tree XLEN=32 build at
`<repo>/build/`. Run application commands **from that build directory** because
the generated scripts and Makefiles there contain the configured paths.

### 1. Configure once, or refresh after source/config/build-file changes

From the repository root:

```bash
mkdir -p build
cd build
../configure --xlen=32 --tooldir="$HOME/tools"
```

Run `../configure` again from `build/` after changing source Makefiles,
configuration TOMLs, or adding/removing a build-participating directory.

### 2. Compile the regression and SIMX runtime

From the repository root, use the generated Makefile:

```bash
make -C build/tests/regression/ghost clean
make -C build/tests/regression/ghost -j2
make -C build/sw/runtime/simx -j2
```

To build the generated SIMX executable and run the test through the canonical
blackbox flow, run from `build/` instead:

```bash
cd build
make -C tests/regression/ghost -j2
```

`blackbox.sh` builds the matching driver runtime before the application run.
Building the application compiles the host runner plus `kernel.vxbin` and
`kernel.dump`; the dump is useful for confirming the emitted kernel
instructions.

Do not invoke `make -C tests/regression/ghost` from the source tree. Like the
other generated regression Makefiles, this Makefile expects the generated
`config.mk` and configuration from the out-of-tree build.

### 3. Run baseline SIMX and produce a debug log

From `build/`:

```bash
CONFIGS="" ./ci/blackbox.sh \
  --driver=simx \
  --app=ghost \
  --debug=3 \
  --log=ghost_baseline.log

python3 ci/trace_order.py ghost_baseline.log > ghost_baseline_issue_order.csv
```

The test should print `PASSED!`. Debug traces require a serial SIMX build;
leave `SIMX_MT` unset (or set to 0/1) for trace collection.

### 4. Run GhOST with a trace

From `build/`:

```bash
CONFIGS="-DSIMX_GHOST_ENABLE" ./ci/blackbox.sh \
  --driver=simx \
  --app=ghost \
  --debug=3 \
  --log=ghost.log

python3 ci/trace_order.py ghost.log > ghost_issue_order.csv
```

To configure the capacities, pass compile-time defines through `CONFIGS`:

```bash
CONFIGS="-DSIMX_GHOST_ENABLE -DSIMX_GHOST_ISB_SIZE=8 -DSIMX_GHOST_ITAB_SIZE=2" \
  ./ci/blackbox.sh --driver=simx --app=ghost
```

The defaults are 8 IsB entries and 2 ITab candidate slots per warp. `ITAB_SIZE`
must be between 1 and `ISB_SIZE`.

### 5. Inspect candidate and accepted issue order

The CSV columns are:

```text
cycle,event,core,cid,wid,pc,uuid,opcode
```

To inspect the small load-dependent sequence:

```bash
grep -E ',(LW|ADDI|ADD)$' ghost_issue_order.csv
```

In the verified feature-on trace, the independent `ADDI` at PC `0x80000048`
was accepted before the load-dependent `ADDI` at PC `0x80000044`. The actual
trace is authoritative if compiler output changes. `issue-candidate` can
repeat and is not equivalent to accepted issue.

If the parser reports no issue events, confirm the log was generated with
`--debug=3` and that the application actually ran. The parser exits with an
error rather than silently emitting an empty CSV.

### 6. Run from the regression Makefile

From `build/`:

```bash
CONFIGS="-DSIMX_GHOST_ENABLE" make -C tests/regression run-simx-ghost
```

The test is excluded from `run-rtlsim`, `run-opae`, and `run-xrt` by design.
Use `blackbox.sh` with `--debug=3` when a trace is needed.

### 7. Check disabled behavior

From `build/`:

```bash
CONFIGS="" ./ci/blackbox.sh --driver=simx --app=ghost
CONFIGS="" ./ci/blackbox.sh --driver=simx --app=demo
```

The disabled run uses the original in-order I-buffer issue path. Apply
`CONFIGS` to the blackbox invocation so the runtime is rebuilt with the
requested define; GhOST options are SIMX C++ defines, not `VX_CFG_*` hardware
parameters.

## Current limitations and remaining work

This is a stateful prototype, not a claim of full paper-equivalent hardware.

- Multi-candidate structural fallback is implemented, but there is not yet a
  focused test that forces the oldest ready entry to fail a structural check
  while a younger ITab candidate issues.
- `SIMX_GHOST_ENABLE` is opt-in and SIMX-only. RTL, runtime APIs, and other
  drivers are unchanged.
- Macro-ops, atomics, most CSRs, FCSR-affecting FPU operations, fences, warp
  control, and unknown instruction classes use a conservative I-buffer path.
  Read-only `MCYCLE` is the sole CSR exception, allowing the existing WSYNC
  timing test to sample while loads are in flight.
- Branches wait for older IsB work and stop younger admission until Vortex
  resolves the branch. This does not add branch prediction or speculative
  execution.
- Loads may pass loads. A conflicting memory pair waits for older memory
  completion; synchronization instructions may reach their existing unit
  while issued memory operations drain.
- Trace correctness, IsB flush with outstanding memory, macro-op sequencing,
  capacity edge cases beyond the verified 4-entry/1-candidate configuration,
  and multiple issue widths/warps still need dedicated focused tests.
  Representative apps passing is not full coverage.
- `GhostPipelinePorts` retains paper-shaped control records; some fields are
  descriptive interfaces and are not yet independently driven as hardware
  ports in SIMX.

### Follow-on tests

- Add separate focused RAW/WAR/WAW cases with accepted-issue-order assertions
  beyond the combined kernel smoke test.
- Force FU-queue or issue-lock backpressure on the oldest ITab candidate and
  verify the selected younger candidate's identity and accepted order.
- Add directed branch, trap/flush with outstanding loads, macro-op, and custom
  IsB/ITab size cases.
- Exercise more than one issue width and verify candidate identity through the
  existing warp arbiter.
- Compare performance only after correctness; do not infer paper-level speedup
  from the small regression.

## Verification record

In the configured XLEN=32 build:

- The feature-on `ghost` regression passed and its trace showed independent
  `ADDI` at PC `0x80000048` accepted before dependent `ADDI` at PC
  `0x80000044`.
- The expanded kernel passed its load-RAW, register-WAR, and register-WAW
  checks; the accepted trace showed the dependent read before the older-register
  overwrite, and the result read before the final same-register overwrite.
- The `ghost` regression also passed with `SIMX_GHOST_ISB_SIZE=4` and
  `SIMX_GHOST_ITAB_SIZE=1`.
- Feature-on `basic`, `demo`, `fence`, `ldst_order`, `async_barrier`, and
  `wsync` regressions passed.
- The `wsync` test initially failed when in-flight memory tracking delayed its
  counter read; allowing only non-writing `MCYCLE` reads to pass older memory
  restored the existing drain measurement and the test passed.
- The feature-on `demo` initially crashed because the per-warp Sequencer
  returned a cached simple-instruction trace for a different OoO candidate.
  Simple instructions now pass through without caching; macro-op micro-ops
  remain cached and pinned. `demo` passes both with GhOST enabled and disabled.
- An initial feature-off build exposed `-Werror` unused-variable failures in
  feature-guarded issue state; the declarations now compile correctly with the
  define either present or absent.
- The selected tests above run on SIMX only; they do not establish full
  paper-fidelity, all trap/macro-op safety, or other-driver behavior.

Run `git diff --check` after edits. Keep the generated debug log and CSV in the
build directory when reproducing the observed issue order.
