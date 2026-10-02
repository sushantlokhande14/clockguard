#pragma once

// A flattened Yosys netlist (write_json after proc; flatten; memory), at
// bit level. Net bits are Yosys's integer ids; constants are negative.

#include <map>
#include <string>
#include <vector>

#include <nlohmann/json.hpp>

namespace cg {

using json = nlohmann::json;

constexpr int kConst0 = -1, kConst1 = -2, kConstX = -3;
inline bool is_const(int b) { return b < 0; }

struct Cell {
  std::string name, type;
  std::map<std::string, std::vector<int>> conn;
  std::map<std::string, bool> is_out;
  std::map<std::string, std::string> params;

  long param(const std::string& k, long def = 0) const;
  const std::vector<int>& pin(const std::string& p) const;
  bool has(const std::string& p) const { return conn.count(p) && !conn.at(p).empty(); }
};

// Where a bit comes from or goes to. cell == -1 means a top-level port.
struct PinRef {
  int cell = -1;
  std::string port;
  int index = 0;
};

enum class CellKind { Comb, Flop, Latch, Other };

struct Netlist {
  std::string top;
  std::vector<Cell> cells;
  std::map<std::string, std::vector<int>> inputs, outputs;  // top ports
  std::map<int, PinRef> driver;
  std::map<int, std::vector<PinRef>> loads;
  std::map<int, std::string> bit_name;

  static Netlist load(const json& j, const std::string& top = "");

  CellKind kind(int cell) const;
  std::string name(int bit) const;  // a readable name for any bit
  bool is_input(int bit) const { auto it = driver.find(bit); return it != driver.end() && it->second.cell == -1; }
  std::string input_port(int bit) const;  // "" if not a top input
  int max_bit = 0;
};

// Input bits a combinational cell output bit depends on.
std::vector<int> comb_inputs(const Cell& c, const std::string& out_port, int idx);

}  // namespace cg
