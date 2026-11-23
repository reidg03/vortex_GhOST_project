// Copyright © 2019-2023
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#pragma once

#include "instr_trace.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <cassert>
#include <deque>
#include <memory>
#include <optional>
#include <vector>

#ifndef SIMX_GHOST_ISB_SIZE
#define SIMX_GHOST_ISB_SIZE 8
#endif

#ifndef SIMX_GHOST_ITAB_SIZE
#define SIMX_GHOST_ITAB_SIZE 2
#endif

namespace vortex {

constexpr uint32_t kGhostIsbBanks = SIMX_GHOST_ISB_SIZE;
constexpr uint32_t kGhostInflightDependencies = NUM_SRC_REGS;
constexpr uint32_t kGhostDependencyBits = kGhostIsbBanks + kGhostInflightDependencies;
constexpr uint32_t kGhostItabEntries = SIMX_GHOST_ITAB_SIZE;

static_assert(SIMX_GHOST_ISB_SIZE > 0, "GhOST IsB must have at least one entry");
static_assert(SIMX_GHOST_ITAB_SIZE > 0, "GhOST ITab must have at least one entry");
static_assert(SIMX_GHOST_ITAB_SIZE <= SIMX_GHOST_ISB_SIZE,
              "GhOST ITab cannot exceed IsB capacity");

struct GhostInstructionClass {
  bool is_load = false;
  bool is_store = false;
  bool is_atomic = false;
  bool is_barrier = false;
  bool is_branch = false;
  bool is_ordered = false;
  bool is_macro = false;
};

inline bool ghost_is_read_only_cycle_read(const Instr& instr) {
  const auto& op_type = instr.get_op_type();
  auto csr_type = std::get_if<CsrType>(&op_type);
  auto csr_args = std::get_if<IntrCsrArgs>(&instr.get_args());
  if (!csr_type || !csr_args || csr_args->csr != VX_CSR_MCYCLE)
    return false;
  if (*csr_type != CsrType::CSRRS && *csr_type != CsrType::CSRRC)
    return false;
  // Only a non-writing MCYCLE read may pass older memory operations.
  if (csr_args->is_imm)
    return csr_args->imm == 0;
  auto source = instr.get_src_reg(0);
  return source.type == RegType::Integer && source.idx == 0;
}

inline GhostInstructionClass ghost_classify(const Instr& instr) {
  GhostInstructionClass result;
  auto op_type = instr.get_op_type();
  if (auto lsu = std::get_if<LsuType>(&op_type)) {
    result.is_load = (*lsu == LsuType::LOAD);
    result.is_store = (*lsu == LsuType::STORE);
    result.is_barrier = (*lsu == LsuType::FENCE);
    result.is_ordered = result.is_barrier;
  } else if (std::holds_alternative<AmoType>(op_type)) {
    result.is_atomic = true;
    result.is_ordered = true;
  } else if (auto branch = std::get_if<BrType>(&op_type)) {
    result.is_branch = (*branch == BrType::BR
                     || *branch == BrType::JAL
                     || *branch == BrType::JALR);
    result.is_ordered = !result.is_branch;
  } else if (auto warp_ctl = std::get_if<WctlType>(&op_type)) {
    result.is_barrier = (*warp_ctl == WctlType::BAR || *warp_ctl == WctlType::WSYNC);
    result.is_ordered = true;
  } else if (std::holds_alternative<AluType>(op_type)
          || std::holds_alternative<MdvType>(op_type)
          || std::holds_alternative<VoteType>(op_type)
          || std::holds_alternative<ShflType>(op_type)) {
    result.is_ordered = false;
  } else if (std::holds_alternative<FpuType>(op_type)) {
    result.is_ordered = (instr.fcsr_reads() != 0 || instr.fcsr_writes() != 0);
  } else if (std::holds_alternative<CsrType>(op_type)) {
    result.is_ordered = !ghost_is_read_only_cycle_read(instr);
  } else {
    result.is_ordered = true;
  }
  result.is_macro = instr.is_macro_op();
  result.is_ordered = result.is_ordered || result.is_macro;
  return result;
}

inline GhostInstructionClass ghost_classify(const instr_trace_t& trace) {
  assert(trace.instr_ptr);
  return ghost_classify(*trace.instr_ptr);
}

struct GhostInstructionEntry {
  bool valid = false;
  bool issued = false;
  uint32_t warp_id = 0;
  uint64_t uuid = 0;
  Word pc = 0;
  uint32_t instruction_bits = 0;
  uint32_t age = 0;
  ThreadMask mask;
  std::bitset<kGhostDependencyBits> dependencies;
  instr_trace_t* trace = nullptr;
  std::shared_ptr<Instr> decoded_instruction;
  std::array<RegOpd, NUM_SRC_REGS> src_regs{};
  RegOpd dst_reg;
  bool writeback = false;
  uint8_t fcsr_read_mask = 0;
  uint8_t fcsr_write_mask = 0;
  GhostInstructionClass instruction_class;

  GhostInstructionEntry()
    : mask(VX_CFG_NUM_THREADS) {}
};

inline GhostInstructionEntry ghost_make_entry(instr_trace_t* trace) {
  assert(trace);
  GhostInstructionEntry entry;
  entry.valid = true;
  entry.warp_id = trace->wid;
  entry.uuid = trace->uuid;
  entry.pc = trace->PC;
  entry.instruction_bits = trace->code;
  entry.mask = trace->tmask;
  entry.trace = trace;
  entry.decoded_instruction = trace->instr_ptr;
  entry.dst_reg = trace->dst_reg;
  entry.writeback = trace->wb;
  entry.fcsr_read_mask = trace->instr_ptr->fcsr_reads();
  entry.fcsr_write_mask = trace->instr_ptr->fcsr_writes();
  entry.instruction_class = ghost_classify(*trace);
  for (uint32_t i = 0; i < NUM_SRC_REGS; ++i) {
    entry.src_regs.at(i) = trace->src_regs.at(i);
  }
  return entry;
}

inline bool ghost_same_reg(const RegOpd& lhs, const RegOpd& rhs) {
  return lhs.type != RegType::None && lhs.type == rhs.type && lhs.idx == rhs.idx;
}

inline bool ghost_has_memory_dependency(const GhostInstructionClass& newer,
                                        const GhostInstructionClass& older) {
  const bool new_memory = newer.is_load || newer.is_store || newer.is_atomic;
  const bool old_memory = older.is_load || older.is_store || older.is_atomic;
  if (!new_memory || !old_memory)
    return false;
  return !(newer.is_load && older.is_load);
}

struct GhostIsbSnapshot {
  uint32_t warp_id = 0;
  std::vector<GhostInstructionEntry> entries;
  bool full = false;
  std::vector<uint32_t> free_locations;
};

struct GhostScoreboardState {
  bool ready_valid = false;
  bool ready = false;
  std::array<bool, NUM_SRC_REGS> source_dependencies{};
  bool destination_dependency = false;
};

struct GhostWarpControl {
  uint32_t warp_id = 0;
  ThreadMask mask;
  bool mask_valid = false;
  bool branch_pending = false;
  bool synchronization_pending = false;

  GhostWarpControl()
    : mask(VX_CFG_NUM_THREADS) {}
};

struct GhostIssueUpdate {
  bool valid = false;
  uint32_t warp_id = 0;
  uint32_t bank_id = 0;
  bool instruction_complete = false;
  instr_trace_t* trace = nullptr;
};

struct GhostWritebackUpdate {
  bool valid = false;
  uint32_t warp_id = 0;
  RegOpd dst_reg;
  uint8_t fcsr_mask = 0;
  instr_trace_t* trace = nullptr;
};

struct GhostLocationPointer {
  bool full = false;
  std::vector<uint32_t> free_locations;
};

struct GhostItabCandidate {
  bool valid = false;
  uint32_t bank_id = 0;
  GhostInstructionEntry entry;
};

struct GhostInstructionTable {
  std::array<GhostItabCandidate, kGhostItabEntries> candidates;
  bool empty = true;
};

struct GhostDecodeSchedulerPort {
  bool valid = false;
  uint32_t warp_id = 0;
  uint32_t idx_val = 0;
};

struct GhostSchedulerState {
  std::vector<uint8_t> writeback_warps;
  std::vector<uint8_t> itab_empty_warps;
};

struct GhostPipelinePorts {
  GhostInstructionEntry incoming;
  GhostIsbSnapshot isb;
  GhostScoreboardState scoreboard;
  GhostWarpControl warp_control;
  GhostDecodeSchedulerPort decode_scheduler;
  GhostSchedulerState scheduler_state;
  GhostLocationPointer location_pointer;
  GhostInstructionTable itab;
  bool dc_to_isb_valid = false;
};

struct GhostDependenceCheckerInput {
  GhostPipelinePorts ports;
  std::array<bool, NUM_SRC_REGS> source_dependencies{};
};

struct GhostDependenceCheckerOutput {
  GhostInstructionEntry entry;
  bool accepted = false;
};

class GhostDependenceChecker {
public:
  // Input: one decoded instruction, older same-warp IsB entries, and current
  // scoreboard source hazards. Output: that entry with bank/source dependency
  // bits; no instruction is selected or issued here.
  GhostDependenceCheckerOutput check(const GhostDependenceCheckerInput& input) const {
    auto entry = input.ports.incoming;
    if (!entry.valid || input.ports.isb.full)
      return {entry, false};

    entry.dependencies.reset();
    for (uint32_t src = 0; src < NUM_SRC_REGS; ++src) {
      entry.dependencies.set(kGhostIsbBanks + src, input.source_dependencies.at(src));
    }

    for (uint32_t bank = 0; bank < input.ports.isb.entries.size(); ++bank) {
      const auto& older = input.ports.isb.entries.at(bank);
      if (!older.valid)
        continue;
      if (has_register_dependency(entry, older)
       || ghost_has_memory_dependency(entry.instruction_class, older.instruction_class)
       || entry.instruction_class.is_branch
       || older.instruction_class.is_branch
       || entry.instruction_class.is_ordered
       || older.instruction_class.is_ordered) {
        entry.dependencies.set(bank);
      }
    }
    return {entry, true};
  }

private:
  static bool has_register_dependency(const GhostInstructionEntry& newer,
                                      const GhostInstructionEntry& older) {
    if (older.writeback && ghost_same_reg(newer.dst_reg, older.dst_reg))
      return true;
    if (older.writeback) {
      for (const auto& source : newer.src_regs) {
        if (ghost_same_reg(source, older.dst_reg))
          return true;
      }
    }
    if (newer.writeback) {
      for (const auto& source : older.src_regs) {
        if (ghost_same_reg(newer.dst_reg, source))
          return true;
      }
      if (ghost_same_reg(newer.dst_reg, older.dst_reg))
        return true;
    }
    return false;
  }

};

struct GhostIssueBufferInput {
  GhostInstructionEntry entry;
};

struct GhostIssueBufferOutput {
  bool accepted = false;
  uint32_t bank_id = 0;
};

class GhostIssueBuffer {
public:
  struct WarpState {
    std::vector<GhostInstructionEntry> entries;
    std::deque<uint32_t> free_locations;
    uint32_t next_age = 0;
    bool branch_pending = false;
    bool ordered_pending = false;
    instr_trace_t* ordered_trace = nullptr;
    std::optional<uint32_t> pinned_bank;
  };

  explicit GhostIssueBuffer(uint32_t num_warps = VX_CFG_NUM_WARPS)
    : warps_(num_warps) {
    reset();
  }

  void reset() {
    for (auto& warp : warps_) {
      warp.entries.assign(kGhostIsbBanks, GhostInstructionEntry{});
      warp.free_locations.clear();
      for (uint32_t bank = 0; bank < kGhostIsbBanks; ++bank) {
        warp.free_locations.push_back(bank);
      }
      warp.next_age = 0;
      warp.branch_pending = false;
      warp.ordered_pending = false;
      warp.ordered_trace = nullptr;
      warp.pinned_bank.reset();
    }
    std::fill(scheduler_dirty_.begin(), scheduler_dirty_.end(), true);
  }

  bool can_accept(uint32_t wid, const GhostInstructionEntry& entry) const {
    const auto& warp = warps_.at(wid);
    if (entry.instruction_class.is_ordered
     || warp.free_locations.empty() || warp.branch_pending || warp.ordered_pending)
      return false;
    return true;
  }

  // Input: a dependency-annotated entry. Output: acceptance and its IsB bank.
  // Issued memory entries stay resident until completion to protect ordering.
  GhostIssueBufferOutput insert(const GhostIssueBufferInput& input) {
    const auto& entry = input.entry;
    if (!entry.valid || !can_accept(entry.warp_id, entry))
      return {};

    auto& warp = warps_.at(entry.warp_id);
    uint32_t bank = warp.free_locations.front();
    warp.free_locations.pop_front();
    auto inserted = entry;
    inserted.age = warp.next_age++;
    warp.entries.at(bank) = std::move(inserted);
    if (entry.instruction_class.is_branch)
      warp.branch_pending = true;
    return {true, bank};
  }

  GhostIsbSnapshot snapshot(uint32_t wid) const {
    const auto& warp = warps_.at(wid);
    GhostIsbSnapshot result;
    result.warp_id = wid;
    result.entries = warp.entries;
    result.full = warp.free_locations.empty();
    result.free_locations.assign(warp.free_locations.begin(), warp.free_locations.end());
    return result;
  }

  std::vector<instr_trace_t*> flush(uint32_t wid) {
    auto& warp = warps_.at(wid);
    std::vector<instr_trace_t*> traces;
    for (auto& entry : warp.entries) {
      if (entry.valid && !entry.issued) {
        if (entry.trace)
          traces.push_back(entry.trace);
        entry = GhostInstructionEntry{};
      }
    }
    warp.free_locations.clear();
    warp.next_age = 0;
    for (uint32_t bank = 0; bank < warp.entries.size(); ++bank) {
      auto& entry = warp.entries.at(bank);
      if (!entry.valid) {
        warp.free_locations.push_back(bank);
        continue;
      }
      entry.age = warp.next_age++;
      for (uint32_t dependency = 0; dependency < kGhostIsbBanks; ++dependency) {
        if (!warp.entries.at(dependency).valid)
          entry.dependencies.reset(dependency);
      }
    }
    warp.branch_pending = false;
    warp.ordered_pending = false;
    warp.ordered_trace = nullptr;
    warp.pinned_bank.reset();
    scheduler_dirty_.at(wid) = true;
    return traces;
  }

  std::optional<Word> oldest_pc(uint32_t wid) const {
    const auto& warp = warps_.at(wid);
    const GhostInstructionEntry* oldest = nullptr;
    for (const auto& entry : warp.entries) {
      if (entry.valid && !entry.issued && (!oldest || entry.age < oldest->age))
        oldest = &entry;
    }
    if (!oldest)
      return std::nullopt;
    return oldest->pc;
  }

  bool empty(uint32_t wid) const {
    const auto& entries = warps_.at(wid).entries;
    return std::none_of(entries.begin(), entries.end(),
                        [](const auto& entry) { return entry.valid; });
  }

  bool has_unissued(uint32_t wid) const {
    const auto& entries = warps_.at(wid).entries;
    return std::any_of(entries.begin(), entries.end(),
                       [](const auto& entry) { return entry.valid && !entry.issued; });
  }

  bool set_source_dependencies(uint32_t wid,
                               uint32_t bank,
                               const std::array<bool, NUM_SRC_REGS>& dependencies) {
    auto& entry = warps_.at(wid).entries.at(bank);
    bool changed = false;
    for (uint32_t src = 0; src < NUM_SRC_REGS; ++src) {
      auto bit = kGhostIsbBanks + src;
      if (entry.dependencies.test(bit) != dependencies.at(src)) {
        entry.dependencies.set(bit, dependencies.at(src));
        changed = true;
      }
    }
    if (changed)
      scheduler_dirty_.at(wid) = true;
    return changed;
  }

  void issue(const GhostIssueUpdate& update) {
    if (!update.valid)
      return;
    auto& warp = warps_.at(update.warp_id);
    auto& issued = warp.entries.at(update.bank_id);
    if (!issued.valid)
      return;
    if (!update.instruction_complete) {
      warp.pinned_bank = update.bank_id;
      scheduler_dirty_.at(update.warp_id) = true;
      return;
    }

    auto issued_entry = issued;
    for (uint32_t bank = 0; bank < warp.entries.size(); ++bank) {
      if (bank == update.bank_id)
        continue;
      auto& entry = warp.entries.at(bank);
      if (!entry.valid || !entry.dependencies.test(update.bank_id))
        continue;
      if (ghost_has_memory_dependency(entry.instruction_class,
                                      issued_entry.instruction_class))
        continue;
      entry.dependencies.reset(update.bank_id);
      if (issued_entry.writeback) {
        for (uint32_t src = 0; src < NUM_SRC_REGS; ++src) {
          if (ghost_same_reg(entry.src_regs.at(src), issued_entry.dst_reg))
            entry.dependencies.set(kGhostIsbBanks + src);
        }
      }
    }

    if (issued_entry.instruction_class.is_macro) {
      warp.ordered_pending = false;
      warp.ordered_trace = nullptr;
    }
    if (issued_entry.instruction_class.is_load
     || issued_entry.instruction_class.is_store
     || issued_entry.instruction_class.is_atomic) {
      issued.issued = true;
      if (warp.pinned_bank == update.bank_id)
        warp.pinned_bank.reset();
      scheduler_dirty_.at(update.warp_id) = true;
      return;
    }
    release_entry(update.warp_id, update.bank_id);
  }

  void writeback(const GhostWritebackUpdate& update) {
    if (!update.valid)
      return;
    auto& warp = warps_.at(update.warp_id);
    for (auto& entry : warp.entries) {
      if (!entry.valid)
        continue;
      for (uint32_t src = 0; src < NUM_SRC_REGS; ++src) {
        if (ghost_same_reg(entry.src_regs.at(src), update.dst_reg))
          entry.dependencies.reset(kGhostIsbBanks + src);
      }
    }
    scheduler_dirty_.at(update.warp_id) = true;
  }

  void complete(uint32_t wid, instr_trace_t* trace) {
    auto& warp = warps_.at(wid);
    if (warp.ordered_trace == trace) {
      warp.ordered_pending = false;
      warp.ordered_trace = nullptr;
    }
    for (uint32_t bank = 0; bank < warp.entries.size(); ++bank) {
      const auto& entry = warp.entries.at(bank);
      if (entry.valid && entry.issued && entry.trace == trace) {
        release_entry(wid, bank);
        return;
      }
    }
    scheduler_dirty_.at(wid) = true;
  }

  void hold_ordered(uint32_t wid, instr_trace_t* trace) {
    auto& warp = warps_.at(wid);
    warp.ordered_pending = true;
    warp.ordered_trace = trace;
    scheduler_dirty_.at(wid) = true;
  }

  void pin(uint32_t wid, uint32_t bank) {
    auto& warp = warps_.at(wid);
    assert(warp.entries.at(bank).valid);
    warp.pinned_bank = bank;
    scheduler_dirty_.at(wid) = true;
  }

  void resolve_branch(uint32_t wid) {
    auto& warp = warps_.at(wid);
    warp.branch_pending = false;
    scheduler_dirty_.at(wid) = true;
  }

  bool has_branch_pending(uint32_t wid) const {
    return warps_.at(wid).branch_pending;
  }

  std::optional<uint32_t> pinned_bank(uint32_t wid) const {
    return warps_.at(wid).pinned_bank;
  }

  bool scheduler_dirty(uint32_t wid) const {
    return scheduler_dirty_.at(wid);
  }

  void clear_scheduler_dirty(uint32_t wid) {
    scheduler_dirty_.at(wid) = false;
  }

private:
  void release_entry(uint32_t wid, uint32_t bank) {
    auto& warp = warps_.at(wid);
    const auto age = warp.entries.at(bank).age;
    warp.entries.at(bank) = GhostInstructionEntry{};
    warp.free_locations.push_back(bank);
    for (auto& entry : warp.entries) {
      if (entry.valid && entry.age > age)
        --entry.age;
      if (entry.valid)
        entry.dependencies.reset(bank);
    }
    warp.next_age = warp.next_age > 0 ? warp.next_age - 1 : 0;
    if (warp.pinned_bank == bank)
      warp.pinned_bank.reset();
    scheduler_dirty_.at(wid) = true;
  }

  void reset_warp(uint32_t wid) {
    auto& warp = warps_.at(wid);
    warp.entries.assign(kGhostIsbBanks, GhostInstructionEntry{});
    warp.free_locations.clear();
    for (uint32_t bank = 0; bank < kGhostIsbBanks; ++bank)
      warp.free_locations.push_back(bank);
    warp.next_age = 0;
    warp.branch_pending = false;
    warp.ordered_pending = false;
    warp.ordered_trace = nullptr;
    warp.pinned_bank.reset();
    scheduler_dirty_.at(wid) = true;
  }

  std::vector<WarpState> warps_;
  std::vector<bool> scheduler_dirty_ = std::vector<bool>(VX_CFG_NUM_WARPS, true);
};

struct GhostSchedulerInput {
  uint32_t warp_id = 0;
  GhostIsbSnapshot isb;
  std::array<bool, kGhostIsbBanks> scoreboard_ready{};
  std::optional<uint32_t> pinned_bank;
};

struct GhostSchedulerOutput {
  GhostInstructionTable itab;
};

class GhostScheduler {
public:
  explicit GhostScheduler(uint32_t num_warps = VX_CFG_NUM_WARPS)
    : tables_(num_warps) {}

  void reset() {
    for (auto& state : tables_)
      state = WarpTable{};
  }

  void reset_warp(uint32_t wid) {
    tables_.at(wid) = WarpTable{};
  }

  void mark_dirty(uint32_t wid) {
    tables_.at(wid).dirty = true;
  }

  // Input: IsB state, scoreboard readiness, and an optional sequencer pin.
  // Output: up to ITAB_SIZE oldest-ready candidates for existing arbitration.
  GhostSchedulerOutput schedule(const GhostSchedulerInput& input) {
    auto& state = tables_.at(input.warp_id);
    bool ready_changed = !state.ready_initialized || state.last_ready != input.scoreboard_ready;
    if (!state.dirty && !ready_changed)
      return {state.itab};

    auto& table = state.itab;
    table = GhostInstructionTable{};

    if (input.pinned_bank
     && input.isb.entries.at(*input.pinned_bank).valid) {
      const auto bank = *input.pinned_bank;
      table.candidates.at(0) = {true, bank, input.isb.entries.at(bank)};
      table.empty = false;
      state.dirty = false;
      state.last_ready = input.scoreboard_ready;
      state.ready_initialized = true;
      return {state.itab};
    }

    std::vector<uint32_t> ready_banks;
    for (uint32_t bank = 0; bank < input.isb.entries.size(); ++bank) {
      const auto& entry = input.isb.entries.at(bank);
      if (entry.valid && !entry.issued
       && entry.dependencies.none() && input.scoreboard_ready.at(bank))
        ready_banks.push_back(bank);
    }
    std::sort(ready_banks.begin(), ready_banks.end(), [&](uint32_t lhs, uint32_t rhs) {
      return input.isb.entries.at(lhs).age < input.isb.entries.at(rhs).age;
    });

    for (uint32_t i = 0; i < std::min<uint32_t>(ready_banks.size(), kGhostItabEntries); ++i) {
      uint32_t bank = ready_banks.at(i);
      table.candidates.at(i) = {true, bank, input.isb.entries.at(bank)};
      table.empty = false;
    }
    state.dirty = false;
    state.last_ready = input.scoreboard_ready;
    state.ready_initialized = true;
    return {state.itab};
  }

private:
  struct WarpTable {
    GhostInstructionTable itab;
    std::array<bool, kGhostIsbBanks> last_ready{};
    bool ready_initialized = false;
    bool dirty = true;
  };

  std::vector<WarpTable> tables_;
};

struct GhostPipelineInput {
  GhostPipelinePorts ports;
  std::array<bool, NUM_SRC_REGS> source_dependencies{};
};

struct GhostPipelineOutput {
  GhostInstructionEntry entry;
  bool accepted = false;
  uint32_t bank_id = 0;
};

class GhostPipeline {
public:
  GhostPipeline()
    : issue_buffer_(VX_CFG_NUM_WARPS)
    , scheduler_(VX_CFG_NUM_WARPS) {}

  void reset() {
    issue_buffer_.reset();
    scheduler_.reset();
  }

  bool can_admit(uint32_t wid, const GhostInstructionEntry& entry) const {
    return issue_buffer_.can_accept(wid, entry);
  }

  // Orchestrates DC admission, IsB ownership, and ready-candidate selection.
  GhostPipelineOutput admit(const GhostPipelineInput& input) {
    auto ports = input.ports;
    ports.isb = issue_buffer_.snapshot(ports.incoming.warp_id);
    auto checked = dependence_checker_.check({ports, input.source_dependencies});
    if (!checked.accepted)
      return {};
    auto inserted = issue_buffer_.insert({checked.entry});
    if (inserted.accepted)
      scheduler_.mark_dirty(checked.entry.warp_id);
    return {checked.entry, inserted.accepted, inserted.bank_id};
  }

  GhostIsbSnapshot snapshot(uint32_t wid) const {
    return issue_buffer_.snapshot(wid);
  }

  void set_source_dependencies(uint32_t wid,
                              uint32_t bank,
                              const std::array<bool, NUM_SRC_REGS>& dependencies) {
    if (issue_buffer_.set_source_dependencies(wid, bank, dependencies))
      scheduler_.mark_dirty(wid);
  }

  GhostInstructionTable schedule(uint32_t wid,
                                 const std::array<bool, kGhostIsbBanks>& scoreboard_ready) {
    auto isb = issue_buffer_.snapshot(wid);
    GhostSchedulerInput input;
    input.warp_id = wid;
    input.isb = std::move(isb);
    input.scoreboard_ready = scoreboard_ready;
    input.pinned_bank = issue_buffer_.pinned_bank(wid);
    return scheduler_.schedule(input).itab;
  }

  std::vector<instr_trace_t*> flush(uint32_t wid) {
    scheduler_.reset_warp(wid);
    return issue_buffer_.flush(wid);
  }

  std::optional<Word> oldest_pc(uint32_t wid) const {
    return issue_buffer_.oldest_pc(wid);
  }

  bool empty(uint32_t wid) const {
    return issue_buffer_.empty(wid);
  }

  bool has_unissued(uint32_t wid) const {
    return issue_buffer_.has_unissued(wid);
  }

  void issue(const GhostIssueUpdate& update) {
    issue_buffer_.issue(update);
    scheduler_.mark_dirty(update.warp_id);
  }

  void writeback(const GhostWritebackUpdate& update) {
    issue_buffer_.writeback(update);
    scheduler_.mark_dirty(update.warp_id);
  }

  void complete(uint32_t wid, instr_trace_t* trace) {
    issue_buffer_.complete(wid, trace);
    scheduler_.mark_dirty(wid);
  }

  void hold_ordered(uint32_t wid, instr_trace_t* trace) {
    issue_buffer_.hold_ordered(wid, trace);
    scheduler_.mark_dirty(wid);
  }

  void pin(uint32_t wid, uint32_t bank) {
    issue_buffer_.pin(wid, bank);
    scheduler_.mark_dirty(wid);
  }

  void resolve_branch(uint32_t wid) {
    issue_buffer_.resolve_branch(wid);
    scheduler_.mark_dirty(wid);
  }

private:
  GhostDependenceChecker dependence_checker_;
  GhostIssueBuffer issue_buffer_;
  GhostScheduler scheduler_;
};

} // namespace vortex
