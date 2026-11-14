#!/usr/bin/env python3

import argparse
import csv
import re
import sys

INSTR_RE = re.compile(r"DEBUG Instr:\s*([A-Za-z0-9_.]+).*#(\d+)")
EVENT_RE = re.compile(
    r"^TRACE\s+(\d+):\s+(\S+)-pipeline\s+(issue-candidate|issue):\s+(.*)$"
)
CID_RE = re.compile(r"\bcid=(\d+)")
WID_RE = re.compile(r"\bwid=(\d+)")
PC_RE = re.compile(r"\bPC=(0x[0-9a-fA-F]+)")
UUID_RE = re.compile(r"#(\d+)")


def parse_trace(filename):
    instructions = {}
    events = []
    with open(filename, encoding="utf-8") as trace_file:
        for line_number, line in enumerate(trace_file, start=1):
            instr_match = INSTR_RE.search(line)
            if instr_match:
                instructions[int(instr_match.group(2))] = instr_match.group(1)
                continue

            event_match = EVENT_RE.match(line)
            if not event_match:
                continue

            cycle, core, kind, details = event_match.groups()
            fields = (
                CID_RE.search(details),
                WID_RE.search(details),
                PC_RE.search(details),
                UUID_RE.search(details),
            )
            if any(field is None for field in fields):
                raise ValueError(f"{filename}:{line_number}: malformed issue trace event")
            cid, wid, pc, uuid = (field.group(1) for field in fields)
            events.append({
                "cycle": cycle,
                "event": "candidate" if kind == "issue-candidate" else "accepted",
                "core": core,
                "cid": cid,
                "wid": wid,
                "pc": pc,
                "uuid": uuid,
                "opcode": instructions.get(int(uuid), ""),
            })

    return events


def main():
    parser = argparse.ArgumentParser(
        description="Extract SIMX issue-candidate and accepted-issue events as CSV."
    )
    parser.add_argument("trace", help="SIMX debug log produced with --debug=3")
    args = parser.parse_args()

    try:
        events = parse_trace(args.trace)
    except (OSError, ValueError) as error:
        print(error, file=sys.stderr)
        return 1

    if not events:
        print(f"{args.trace}: no issue events found; enable SIMX debug level 3", file=sys.stderr)
        return 1

    writer = csv.DictWriter(sys.stdout, fieldnames=events[0].keys())
    writer.writeheader()
    writer.writerows(events)
    return 0


if __name__ == "__main__":
    sys.exit(main())
