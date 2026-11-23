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
#include <array>
#include <bitset>
#include <memory>
#include <optional>
#include <vector>

namespace vortex {

constexpr uint32_t kGhostIsbBanks = 8;
constexpr uint32_t kGhostInflightDependencies = 3;
constexpr uint32_t kGhostDependencyBits = kGhostIsbBanks + kGhostInflightDependencies;
constexpr uint32_t kGhostItabEntries = 2;

struct GhostInstructionClass {
  FUType fu_type = FUType::ALU;
  OpType op_type{};
  std::optional<bool> is_load;
  std::optional<bool> is_store;
  std::optional<bool> is_atomic;
  std::optional<bool> is_barrier;
  std::optional<bool> is_branch;
};

struct GhostInstructionEntry {
  bool valid = false;
  uint32_t warp_id = 0;
  uint64_t uuid = 0;
  Word pc = 0;
  uint32_t instruction_bits = 0;
  std::optional<uint32_t> idx;
  ThreadMask mask;
  std::optional<std::bitset<kGhostDependencyBits>> ib_dep;
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

struct GhostIsbSnapshot {
  uint32_t warp_id = 0;
  std::array<GhostInstructionEntry, kGhostIsbBanks> entries;
  std::optional<bool> full;
  std::vector<uint32_t> free_locations;
};

struct GhostScoreboardState {
  bool ready_valid = false;
  bool ready = false;
  std::optional<bool> destination_dependency;
  std::array<std::optional<bool>, kGhostInflightDependencies> source_dependencies;
};

struct GhostWarpControl {
  uint32_t warp_id = 0;
  bool mask_valid = false;
  ThreadMask mask;
  std::optional<bool> mask_pop;
  std::optional<bool> control_flag;
  std::optional<bool> branch_resolved;
  std::optional<bool> synchronization_pending;
  std::optional<bool> synchronization_issued;

  GhostWarpControl()
    : mask(VX_CFG_NUM_THREADS) {}
};

struct GhostIssueUpdate {
  bool valid = false;
  uint32_t warp_id = 0;
  std::optional<uint32_t> bank_id;
  std::optional<uint32_t> idx;
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
  std::optional<bool> full;
  std::vector<uint32_t> free_locations;
};

struct GhostItabCandidate {
  bool valid = false;
  std::optional<uint32_t> bank_id;
  GhostInstructionEntry entry;
};

struct GhostInstructionTable {
  std::array<GhostItabCandidate, kGhostItabEntries> candidates;
  std::optional<bool> empty;
};

struct GhostDecodeSchedulerPort {
  bool valid = false;
  uint32_t warp_id = 0;
  std::optional<uint32_t> idx_val;
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
  std::optional<bool> control_flag;
  bool dc_to_isb_valid = false;
};

struct GhostDependenceCheckerInput {
  GhostPipelinePorts ports;
};

struct GhostDependenceCheckerOutput {
  GhostPipelinePorts ports;
};

class GhostDependenceChecker {
public:
  // TODO: Add dependence and instruction-order checks before the IsB transfer.
  GhostDependenceCheckerOutput pass_through(const GhostDependenceCheckerInput& input) const {
    return {input.ports};
  }

};

struct GhostIssueBufferInput {
  GhostPipelinePorts ports;
};

struct GhostIssueBufferOutput {
  GhostPipelinePorts ports;
};

class GhostIssueBuffer {
public:
  // TODO: Add per-warp entry storage and issue/writeback state updates.
  GhostIssueBufferOutput pass_through(const GhostIssueBufferInput& input) const {
    return {input.ports};
  }

  GhostIssueUpdate pass_through_issue_update(const GhostIssueUpdate& input) const {
    return input;
  }

  GhostWritebackUpdate pass_through_writeback(const GhostWritebackUpdate& input) const {
    return input;
  }
};

struct GhostSchedulerInput {
  GhostPipelinePorts ports;
};

struct GhostSchedulerOutput {
  GhostPipelinePorts ports;
};

class GhostScheduler {
public:
  // TODO: Populate ITab from the oldest dependency-free IsB entries.
  GhostSchedulerOutput pass_through(const GhostSchedulerInput& input) const {
    return {input.ports};
  }

  GhostIssueUpdate pass_through_issue_update(const GhostIssueUpdate& input) const {
    return input;
  }

  GhostWritebackUpdate pass_through_writeback(const GhostWritebackUpdate& input) const {
    return input;
  }
};

struct GhostPipelineInput {
  GhostPipelinePorts ports;
};

struct GhostPipelineOutput {
  GhostPipelinePorts ports;
};

class GhostPipeline {
public:
  GhostPipelineOutput pass_through(const GhostPipelineInput& input) const {
    auto dependence = dependence_checker_.pass_through({input.ports});
    auto issue_buffer = issue_buffer_.pass_through({dependence.ports});
    auto scheduler = scheduler_.pass_through({issue_buffer.ports});
    return {scheduler.ports};
  }

  GhostIssueUpdate pass_through_issue_update(const GhostIssueUpdate& input) const {
    auto scheduler_update = scheduler_.pass_through_issue_update(input);
    return issue_buffer_.pass_through_issue_update(scheduler_update);
  }

  GhostWritebackUpdate pass_through_writeback(const GhostWritebackUpdate& input) const {
    auto scheduler_update = scheduler_.pass_through_writeback(input);
    return issue_buffer_.pass_through_writeback(scheduler_update);
  }

private:
  GhostDependenceChecker dependence_checker_;
  GhostIssueBuffer issue_buffer_;
  GhostScheduler scheduler_;
};

} // namespace vortex
