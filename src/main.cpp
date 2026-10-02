// cg_engine: clock, reset and CDC structure checks on a flattened Yosys netlist.
// The clockguard script runs Yosys, then this, then renders the report.

#include <fstream>
#include <iostream>

#include "analysis.h"
#include "netlist.h"

using namespace cg;

static const char* kUsage = R"(usage: cg_engine [options] netlist.json

  -c FILE      constraints (clocks, groups, async inputs, waivers)
  --top NAME   top module (default: the one Yosys marked as top)
  -o FILE      write the report here (default: stdout)
  -q           no summary on stderr
)";

int main(int argc, char** argv) {
  std::string in, out, cons, top;
  bool quiet = false;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-c" && i + 1 < argc) cons = argv[++i];
    else if (a == "-o" && i + 1 < argc) out = argv[++i];
    else if (a == "--top" && i + 1 < argc) top = argv[++i];
    else if (a == "-q") quiet = true;
    else if (a == "-h" || a == "--help") {
      std::cout << kUsage;
      return 0;
    } else if (a[0] != '-' && in.empty()) in = a;
    else {
      std::cerr << kUsage;
      return 2;
    }
  }
  if (in.empty()) {
    std::cerr << kUsage;
    return 2;
  }

  json report;
  try {
    std::ifstream f(in);
    if (!f) throw std::runtime_error("cannot open " + in);
    Netlist nl = Netlist::load(json::parse(f), top);
    Constraints c;
    if (!cons.empty()) {
      std::ifstream cf(cons);
      if (!cf) throw std::runtime_error("cannot open " + cons);
      c = Constraints::from_json(json::parse(cf));
    }
    report = analyze(nl, c);
  } catch (const std::exception& e) {
    std::cerr << "cg_engine: " << e.what() << "\n";
    return 2;
  }

  if (out.empty()) std::cout << report.dump(1) << "\n";
  else std::ofstream(out) << report.dump(1) << "\n";

  const json& s = report["summary"];
  if (!quiet)
    std::cerr << "cg_engine: " << report["top"].get<std::string>() << ": " << s["domains"] << " domain(s), "
              << s["synchronizers"] << " synchronizer(s), " << s["errors"] << " error(s), " << s["warnings"]
              << " warning(s), " << s["waived"] << " waived\n";
  return s["errors"].get<int>() > 0 ? 1 : 0;
}
