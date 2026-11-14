# GhOST on SIMX: Foundation

## Goal

Provide a low-risk place to learn and measure instruction issue order before implementing any GhOST scheduling behavior. This foundation is SIMX-only and must preserve the baseline order and results.

## Vortex SIMX issue path

`Core::Impl::tick()` calls commit, dispatch-credit return, issue, queue-full update, decode, fetch, and finally fetch-side warp scheduling. `Scheduler::schedule()` chooses a warp for fetch; it is not the instruction issue arbiter.

The fetch scheduler's trace passes through the fetch latch and I-cache, then the decode latch. `Core::Impl::decode()` decodes it and appends the `instr_trace_t` to a per-warp I-buffer. `Core::Impl::issue()` examines each eligible warp's I-buffer head, expands its micro-op through the per-warp sequencer, checks the scoreboard and structural constraints, then uses the existing issue arbiter to select an instruction. Acceptance sends the instruction to `Operands`; dispatch, execution, and commit follow. This makes `Core::Impl::issue()` the GhOST insertion point, leaving the fetch-side `Scheduler` alone.

Issue traces distinguish:

- **Candidate** — an instruction has passed existing issue eligibility checks and is presented to arbitration.
- **Accepted** — the existing arbiter grants it and it enters `Operands`.

The candidate event can repeat while the arbiter chooses another warp or a later cycle retries it. It must not be interpreted as a committed or issued instruction.

## Pass-through structure

When built with `SIMX_GHOST_PASSTHROUGH`, the issue path passes each decoded I-buffer trace through three separate shells before the existing sequencer and issue checks:

1. **Dependence Checker** — future role: compare a decoded instruction with older buffered instructions and attach dependency information.
2. **GhOST Issue Buffer** — future role: retain multiple decoded instructions and dependency state per warp.
3. **GhOST Scheduler / ITab** — future role: choose oldest ready instructions and present per-warp candidates to the existing issue arbitration.

At this stage each shell returns its input trace unchanged. There is no dependency calculation, additional queue, candidate filtering, or reordering. Existing scoreboard checks, issue locks, FU backpressure, operand capture, and retirement remain authoritative. The feature define is consumed only by SIMX; it is not a hardware configuration option.

Future design work must settle buffer sizing, trace lifetime/ownership, register and memory ordering, control-flow/barrier rules, and interaction with the micro-op sequencer before adding GhOST behavior.

## Baseline kernel and trace workflow

The `tests/regression/ghost` kernel contains two independent integer operations separated by a dependent operation, followed by another dependent operation. Its output verifies the arithmetic while its disassembly/log exposes the instruction sequence.

From a configured build directory:

```bash
./ci/blackbox.sh --driver=simx --app=ghost --debug=3 --log=ghost.log
python3 ci/trace_order.py ghost.log > ghost_issue_order.csv

CONFIGS="-DSIMX_GHOST_PASSTHROUGH" ./ci/blackbox.sh --driver=simx --app=ghost --debug=3 --log=ghost_passthrough.log
python3 ci/trace_order.py ghost_passthrough.log > ghost_passthrough_issue_order.csv
```

The two issue CSVs should show the same ordering. A later scheduling implementation can use the same kernel and trace format to demonstrate a deliberate change while checking output and dependency correctness.
