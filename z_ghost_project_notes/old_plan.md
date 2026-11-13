# GhOST-on-SimX Foundation Plan

## Problem and approach

Establish the SIMX pipeline and GhOST insertion point, make candidate-vs-accepted issue order observable, and then build an opt-in SIMX-only GhOST prototype. The first prototype will reorder simple integer ALU instructions only; memory, branches, barriers, and other special or side-effecting instructions stay on a conservative in-order path. No RTL, runtime, or non-SIMX behavior is in scope.

## SIMX fundamentals relevant to the design

`Core::Impl::tick()` advances the SIMX core in this order: commit, return dispatch credits, issue, update functional-unit queue-full state, decode, fetch, then select a warp for fetch. This ordering is a C++ timing model, not a set of independently clocked RTL modules.

The fetch-side `Scheduler::schedule()` chooses an active, unstalled warp and creates an instruction trace with its PC and thread mask. That trace moves through the fetch latch and I-cache request/response, then the decode latch. `decode()` fills the trace with decoded instruction, source/destination registers, FU and operation types, advances the warp PC, and places the trace into that warp's FIFO `ibuffers_`.

The actual issue arbitration is in `Core::Impl::issue()`, not `Scheduler::schedule()`. For each issue slot it examines the head instruction of each eligible warp's I-buffer, expands its micro-op through that warp's `Sequencer`, checks the existing `Scoreboard`, sequence locks, and FU queue credits, and uses `ibuffer_arbs_` to grant one ready warp. An accepted trace is sent to `Operands`, its destination is reserved on the scoreboard, and the I-buffer entry is popped when its final micro-op issues. The dispatchers then feed functional units; commit writes results back and releases scoreboard reservations.

So the baseline has two distinct events worth tracing:

1. **Candidate presentation**: a warp's current instruction passes the issue gates and is offered to the issue arbiter.
2. **Accepted issue**: the arbiter grants it and it enters the operands stage.

Existing `-pipeline issue` tracing records the latter; the foundation should add a clearly distinct candidate event and retain accepted-issue events. A decoded instruction entering the I-buffer is an earlier event, not issue.

## Proposed GhOST placement and connections

Keep the existing fetch, decode, per-warp I-buffer, operands, dispatcher, functional units, and commit path. Add the three GhOST structures between the decoded I-buffer and the existing issue arbitration:

```text
fetch Scheduler -> fetch / decode -> per-warp I-buffer
                                      |
                                      v
                              1. Dependence Checker
                                      |
                                      v
                              2. GhOST Issue Buffer
                                      |
                         ready per-warp candidate(s)
                                      v
                     3. GhOST Scheduler / Instruction Table
                                      |
                                      v
       existing issue gates + warp arbiter -> Operands -> Dispatcher -> FUs -> Commit
                   ^                                  |                    |
                   |                                  +-- accepted issue --+
             existing Scoreboard <------------------------ writeback/release
```

- **Dependence Checker (DC):** takes the oldest decoded trace from a warp's existing I-buffer, its register/opcode metadata, and the relevant older GhOST Issue Buffer entries. It computes the conservative intra-buffer dependence information and transfers the trace to the GhOST Issue Buffer. The existing Scoreboard remains responsible for hazards from already-issued instructions.
- **GhOST Issue Buffer (IsB):** owns decoded traces after DC transfer, retains their order/dependence metadata, and offers entries to the GhOST Scheduler. It receives successful-issue updates to free entries and observes scoreboard/writeback state as needed to make issued dependencies eligible. Preserve one clear owner for each `instr_trace_t` and its existing pending/retirement lifetime.
- **GhOST Scheduler / ITab:** selects the oldest eligible ALU entries per warp and supplies those as that warp's candidates to the existing issue gates and `ibuffer_arbs_`. The existing scoreboard, FU backpressure, issue locks, operand capture, and functional-unit interface remain in place. A candidate is removed from IsB only after accepted issue, not merely when selected or presented.

This is the SIMX analogue of the paper's decode/I-buffer → DC → IsB → GhOST Scheduler/ITab → unmodified warp scheduler/issue path. In this repository the concrete join point is the head-of-I-buffer selection in `Core::Impl::issue()`. The scheduler class in `scheduler.cpp` is instead the fetch-side warp picker.

The supplied `components.txt` is a useful checklist, not yet a Vortex implementation contract. Its paper-specific capacities and metadata widths (for example 8 entries and 2 ITab candidates) should be treated as starting hypotheses and verified against the paper and the existing SIMX configuration before encoding them. SIMX trace pointers, per-warp sequencers, scoreboard lifetime, and control-flow behavior need explicit mapping; do not copy hardware wiring assumptions literally.

## Todos

1. **Documenting SIMX issue flow and GhOST interface** — turn the pipeline map above into a concrete, minimal interface/design note during implementation, confirming component ownership, supported operations, and trace event semantics.
2. **Adding ALU scheduling traces and kernels** — add a compact regression workload that exposes independent and dependent integer ALU instructions; log both issue candidates and accepted issues with cycle, warp, PC/UUID, opcode, and dependency/readiness outcome. Keep this baseline usable without GhOST.
3. **Implementing an opt-in SIMX GhOST prototype** — add DC, IsB, and GhOST Scheduler/ITab only in SIMX, controlled by an explicit build/runtime option that defaults to current in-order behavior. Reorder simple integer ALU operations only; preserve conservative in-order handling for memory/control/special instructions and retain existing issue/FU gates.
4. **Validating ordering and compatibility** — verify the independent-instruction test actually issues out of program order, dependent instructions do not violate register dependencies, and output matches the baseline. Run targeted SIMX regression(s) with GhOST both disabled and enabled; ensure RTL and other drivers are unchanged.

## Considerations

- Candidate presentation and accepted issue must remain separate in the trace; otherwise an arbitration candidate could be mistaken for an issued instruction.
- The initial implementation should not reproduce all paper mechanisms or claim full paper fidelity. In particular, memory ordering, control-flow speculation, barriers, atomics, micro-op sequences, and non-ALU classes require separate rules before they can safely be reordered.
- Avoid changing `scheduler.cpp`'s fetch warp selection for this feature; initially the reusable existing issue arbiter should still choose among GhOST-provided per-warp candidates.
- A selected IsB entry must stay resident if existing issue gates reject it; it is consumed only when accepted into `Operands`.
- Preserve all unrelated pre-existing worktree changes. The current worktree has deleted original paper files and an untracked `z_ghost_project_notes/` directory; neither should be cleaned up or rewritten as part of this task.
