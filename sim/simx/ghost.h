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

namespace vortex {

class GhostDependenceChecker {
public:
  instr_trace_t* pass_through(instr_trace_t* trace) const {
    // TODO: Calculate same-warp dependencies against older GhOST entries.
    return trace;
  }
};

class GhostIssueBuffer {
public:
  instr_trace_t* pass_through(instr_trace_t* trace) const {
    // TODO: Hold multiple decoded traces and track their dependence metadata.
    return trace;
  }
};

class GhostScheduler {
public:
  instr_trace_t* pass_through(instr_trace_t* trace) const {
    // TODO: Present the oldest dependency-free entry for each warp.
    return trace;
  }
};

class GhostPipeline {
public:
  instr_trace_t* pass_through(instr_trace_t* trace) const {
    trace = dependence_checker_.pass_through(trace);
    trace = issue_buffer_.pass_through(trace);
    return scheduler_.pass_through(trace);
  }

private:
  GhostDependenceChecker dependence_checker_;
  GhostIssueBuffer issue_buffer_;
  GhostScheduler scheduler_;
};

} // namespace vortex
