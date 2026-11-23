# GhOST on Vortex SIMX — Foundation and Working Guide

This is the working reference for the current GhOST-on-SIMX foundation:
what is implemented, how SIMX moves instructions, where future GhOST logic
belongs, what remains to be designed, and how to build, run, and inspect the
test workload.

## Status at a glance

**Current state: typed SIMX pass-through foundation and observability only.**
The three GhOST C++ shells pass paper-shaped input/output records through
unchanged. They do not calculate dependencies, buffer extra instructions,
choose ready instructions, or reorder issue. The intent is to establish the
interfaces and insertion point before implementing the scheduling algorithm.

The test workload verifies the result of a small integer instruction sequence.
With debug tracing enabled, a CSV utility records both issue candidates and
instructions accepted by the existing issue arbiter.

Scope is **SIMX only**. No RTL, runtime API, or other-driver GhOST changes are
part of this foundation.

## Files changed or added

| File | Purpose |
|---|---|
| `sim/simx/core.cpp` | Under `SIMX_GHOST_PASSTHROUGH`, maps the per-warp I-buffer head and available decoded metadata into typed GhOST records. Accepted issue and scoreboard/FCSR release events pass through typed feedback records. Adds an `issue-candidate` trace after the existing readiness checks. |
| `sim/simx/ghost.h` | Defines paper-shaped instruction, IsB, scoreboard, warp-control, location-pointer, ITab, issue, and writeback records, plus typed input/output records for `GhostDependenceChecker`, `GhostIssueBuffer`, and `GhostScheduler`. The shells copy all fields unchanged. |
| `tests/regression/ghost/Makefile` | Adds the standalone `ghost` regression application and selects the `vortex2` kernel startup library. |
| `tests/regression/ghost/kernel.cpp` | Contains a deliberate integer ALU instruction sequence with independent and dependent operations, followed by a result store. |
| `tests/regression/ghost/main.cpp` | Builds and launches the kernel, reads back one value per hardware thread, and checks every result is 16. |
| `tests/regression/ghost/common.h` | Defines the device/host kernel argument structure. |
| `tests/regression/Makefile` | Registers `ghost` in the regression list and excludes it from RTL/OPAE/XRT runs; it is intended for SIMX only. |
| `ci/trace_order.py` | Parses SIMX debug logs and emits CSV rows for candidate and accepted issue events. |
| `docs/proposals/ghost_simx_foundation_proposal.md` | Short design proposal describing the baseline insertion point and pass-through boundary. |

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
  -> per-warp FIFO I-buffer (instr_trace_t*)
  -> [future GhOST insertion point]
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

The actual issue logic is in `Core::Impl::issue()`:

- For each issue slice, it looks at the head of each assigned warp's FIFO
  I-buffer.
- It gets that trace's current micro-op from the warp's `Sequencer`.
- It checks the existing scoreboard, sequencer startup timing, issue-lock
  ownership, functional-unit queue credits, and optional feature-specific
  structural constraints.
- Eligible warps are marked in `ready_set`.
- The existing arbiter selects one eligible warp.
- If `Operands` accepts the micro-op, SIMX captures operands, records issue
  timing, reserves scoreboard state where needed, and emits the
  `pipeline issue` trace.
- The I-buffer head is popped when its final micro-op issues.

Thus, GhOST belongs at the **issue-side input-selection boundary** in
`Core::Impl::issue()`, after decode has populated the per-warp I-buffer and
before the existing per-warp sequencer / readiness and arbitration path.
`Scheduler::schedule()` is the fetch-side scheduler and should not be changed
to implement this issue scheduler.

## Current GhOST shells and exact connections

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

With the build define `SIMX_GHOST_PASSTHROUGH`, the I-buffer head and its
available decode metadata are copied into a `GhostPipelinePorts` record and
passed through all three shell calls. In this temporary path, candidate slot
zero carries the same trace pointer so the existing Sequencer still receives
the original instruction. No state is stored in the shells; no trace ownership
or retirement changes.

The typed record makes data available to future modules without pretending
SIMX already has GhOST state:

- **Mapped from SIMX:** validity, warp ID, UUID, PC, instruction bits, thread
  mask, trace and decoded-instruction pointers, source/destination registers,
  FU/op type, writeback indication, and decoded FCSR read/write masks.
- **Present but not populated by this foundation:** IsB entries/full/free
  locations, dependency vector and age/index, scoreboard readiness and
  dependencies, instruction-class flags not represented explicitly by current
  decode metadata, scheduler wakeup vectors, location-pointer state, ITab
  occupancy, branch/synchronization controls, and decode `idx_val`. Optional
  values remain unset rather than being guessed.
- **Feedback records:** accepted issue is forwarded after `Operands` accepts
  the micro-op; scoreboard destination and FCSR releases are forwarded at
  their existing release points. The existing SIMX scoreboard is not replaced
  or modified.

These are synchronous record copies, not new `SimObject`s or `SimChannel`s.
The existing per-cycle schedule and latency are unchanged. The feedback
methods currently have no state-changing effect.

| Shell | Intended future job | Current behavior |
|---|---|---|
| `GhostDependenceChecker` | Compare an incoming instruction's register and instruction-class metadata against older not-yet-issued instructions; attach dependency/ordering metadata. | Copies the typed port record unchanged. |
| `GhostIssueBuffer` | Hold multiple decoded instructions per warp, record age and dependency state, and update state when issue/writeback events occur. | Copies the typed port record and feedback records unchanged; it is not a buffer yet. |
| `GhostScheduler` | Select eligible older-ready entries from the issue buffer and offer per-warp candidates to the existing issue path. | Copies the typed port record and feedback records unchanged; it does not select or reorder. |

The existing Scoreboard remains responsible for hazards from already-issued
instructions. The existing arbiter, issue locks, FU credits, operand capture,
functional units, commit, and scoreboard release remain unchanged. The define
does not enable any GhOST behavior in RTL or another backend. `GhostIssueUpdate`
currently identifies the accepted sequencer micro-op; its optional IsB bank
and index are unset because the current path has no IsB location.

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

## Test kernel and expected instruction stream

The kernel includes these inline assembly operations:

```asm
addi t0, zero, 1
add  t1, t0, t0
addi t2, zero, 7
add  t3, t2, t2
add  result, t1, t3
```

The first `ADDI`/`ADD` chain computes 2. The second independent `ADDI`/`ADD`
chain computes 14. The last `ADD` consumes both results, computes 16, and the
kernel stores it to the output location for each thread. The host runner checks
that all thread outputs equal 16.

`li` aliases may appear in the assembly listing for the immediate loads, but
the trace reports the decoded opcode (`ADDI`). In the checked SIMX run, the
accepted operation order was:

```text
ADDI -> ADD -> ADDI -> ADD -> ADD
```

That is expected for the current pass-through foundation. The independent
second chain is there to give a future OoO scheduler a safe candidate to move
ahead of the first chain's dependent `ADD`; this foundation does not do so.

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

### 3. Run baseline SIMX test and produce a full debug log

From `build/`:

```bash
./ci/blackbox.sh \
  --driver=simx \
  --app=ghost \
  --debug=3 \
  --log=ghost.log
```

The test should print `PASSED!`. Debug traces require a serial SIMX build;
leave `SIMX_MT` unset (or set to 0/1) for trace collection.

### 4. Convert the log to candidate/accepted issue CSV

Still from `build/`:

```bash
python3 ci/trace_order.py ghost.log > ghost_issue_order.csv
head -n 20 ghost_issue_order.csv
```

CSV columns:

```text
cycle,event,core,cid,wid,pc,uuid,opcode
```

To view only accepted issue rows:

```bash
python3 - <<'PY'
import csv
with open("ghost_issue_order.csv", newline="") as f:
    for row in csv.DictReader(f):
        if row["event"] == "accepted":
            print(row["cycle"], row["wid"], row["pc"], row["uuid"], row["opcode"])
PY
```

To inspect the small ALU sequence by its PC range from the disassembly (the
exact address can shift if the kernel changes):

```bash
grep -E ',(ADDI|ADD)$' ghost_issue_order.csv
```

If the parser reports no issue events, confirm the log was generated with
`--debug=3` and that the application actually ran. The parser exits with an
error rather than silently emitting an empty CSV.

### 5. Run the opt-in pass-through path and compare

From `build/`:

```bash
CONFIGS="-DSIMX_GHOST_PASSTHROUGH" \
  ./ci/blackbox.sh \
    --driver=simx \
    --app=ghost \
    --debug=3 \
    --log=ghost_passthrough.log

python3 ci/trace_order.py ghost_passthrough.log \
  > ghost_passthrough_issue_order.csv

diff -u ghost_issue_order.csv ghost_passthrough_issue_order.csv
```

The test should again print `PASSED!`, and `diff` should print nothing and
return zero. This confirms that enabling the current pass-through shells
preserves the observed event order and instruction results.

`CONFIGS` must be applied to the blackbox invocation (not just to a previous
application build), so the SIMX runtime is built with the define. The
`SIMX_GHOST_PASSTHROUGH` flag is a SIMX C++ build define, not a `VX_CFG_*`
hardware parameter.

### 6. Run from the regression Makefile

From `build/`:

```bash
make -C tests/regression run-simx-ghost
```

The test is excluded from `run-rtlsim`, `run-opae`, and `run-xrt` by design.
The `run-simx-ghost` target is a functional regression run; use `blackbox.sh`
with `--debug=3` when a trace is needed.

## Future implementation TODOs

These are **future work**. They are not implemented by the current shells.

### Dependence Checker

- Decide which decoded fields and per-warp state it consumes, including
  register source/destination information, instruction class, current active
  thread mask, age, and older IsB contents.
- Define RAW, WAR, and WAW detection across not-yet-issued instructions and
  how the existing scoreboard covers dependencies on already-issued
  instructions.
- Define interactions with the `Sequencer`: a macro-instruction may expand to
  multiple micro-ops, while the current shell sits before the Sequencer. Pick
  whether GhOST tracks decoded macro-instructions or sequenced micro-ops and
  specify ownership/age mapping before coding a real queue.
- Define conservative handling for memory operations, atomics, branches,
  barriers, fences, warp-control operations, traps, and other side effects.
- Establish backpressure and transfer semantics: a trace must not be dropped,
  duplicated, or advanced if a downstream structure cannot accept it.

### GhOST Issue Buffer

- Choose per-warp capacity and whether a shared or per-warp structure best
  matches the SIMX timing model. Do not copy the paper checklist's 8-entry
  capacity or metadata bit widths without confirming them against the paper
  and the current Vortex configuration.
- Define entry age/order and validity representation, plus dependency state
  with explicit lifetime rules.
- Establish a single owner and deallocation rule for each `instr_trace_t`.
  Existing `pending_instrs_`, I-buffer backpressure counts, the sequencer's
  cached uop, and async-trap flush behavior all need to remain consistent.
- Specify IsB updates on accepted issue and on scoreboard/writeback release.
  Selection/candidate presentation must not free an entry; only a successful
  transfer into `Operands` may count as issue.
- Define reset and per-warp flush behavior, including what happens if a trap
  flushes instructions that have entered the future GhOST buffer.

### GhOST Scheduler / ITab

- Specify what makes an entry ready: intra-buffer dependencies, existing
  scoreboard state, special-register hazards, functional-unit availability,
  issue locks, and any class-specific constraints.
- Decide whether to select one or multiple ready entries per warp, how the
  existing `ibuffer_arbs_` receives candidates, and how failed downstream
  acceptance retries without losing or duplicating entries.
- Preserve the existing issue arbitration policy and structural checks unless
  the later design explicitly calls for changing them.
- Track actual accepted issue to update IsB dependency state. Do not update
  as though an instruction issued merely because it was presented as a
  candidate.

### Follow-on validation

- Add correctness cases for independent instructions that should be eligible
  to overtake a blocked older instruction, as well as RAW/WAR/WAW hazards that
  must not be violated.
- Add memory ordering, control-flow, barrier, trap, and micro-op tests before
  allowing those classes to reorder.
- Compare baseline and feature-enabled outputs and use cycle-tagged issue
  traces to demonstrate a real reordering only after the algorithm exists.
- Ensure default builds and all non-SIMX drivers remain unchanged.

## Current verification record

The typed-record foundation was verified in the configured XLEN=32 build:

- Built the default SIMX runtime with `make -C sw/runtime/simx -j2` from
  `build/`.
- Ran the `ghost` regression once without the define and once with
  `CONFIGS="-DSIMX_GHOST_PASSTHROUGH"`; both logs contained `PASSED!`.
- Parsed both debug logs with `python3 ci/trace_order.py`; the baseline and
  pass-through CSV files compared equal with `diff -u`.
- `git diff --check` passed.

A synthetic event-log smoke test previously confirmed that the parser reports
candidate and accepted events separately.

These checks validate pass-through compatibility and the trace tool. They do
**not** validate out-of-order scheduling, since that has intentionally not
been implemented.
