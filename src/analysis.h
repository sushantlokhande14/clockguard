#pragma once

#include <map>
#include <set>
#include <string>
#include <vector>

#include "netlist.h"

namespace cg {

// Optional facts about the design that can't be read from the netlist.
struct Constraints {
  std::set<std::string> clocks;                      // clock input ports
  std::map<std::string, double> periods;              // clock port -> ns (for testbenches)
  std::vector<std::vector<std::string>> groups;       // clocks that are synchronous to each other
  std::set<std::string> async_inputs;                 // data inputs with no relation to any clock
  std::map<std::string, std::string> input_clock;     // data input -> clock it's launched from
  std::map<std::string, std::string> reset_sync_to;   // reset input already synchronous to a clock
  struct Waiver {
    std::string rule, from, to, reason;
  };
  std::vector<Waiver> waivers;

  static Constraints from_json(const json& j);
};

// Runs every check and returns the report (see docs/report.md).
json analyze(const Netlist& nl, const Constraints& con);

bool glob_match(const std::string& pat, const std::string& s);

}  // namespace cg
