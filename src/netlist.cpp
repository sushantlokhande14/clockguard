#include "netlist.h"

#include <algorithm>
#include <set>
#include <stdexcept>

namespace cg {

static int bit_of(const json& b) {
  if (b.is_number_integer()) return b.get<int>();
  std::string s = b.get<std::string>();
  return s == "0" ? kConst0 : s == "1" ? kConst1 : kConstX;
}

long Cell::param(const std::string& k, long def) const {
  auto it = params.find(k);
  if (it == params.end() || it->second.empty()) return def;
  const std::string& s = it->second;
  if (s.find_first_not_of("01") == std::string::npos) {  // Yosys writes parameters as binary strings
    long v = 0;
    for (char c : s.size() > 62 ? s.substr(s.size() - 62) : s) v = v * 2 + (c == '1');
    return v;
  }
  try {
    return std::stol(s);
  } catch (...) {
    return def;
  }
}

const std::vector<int>& Cell::pin(const std::string& p) const {
  static const std::vector<int> none;
  auto it = conn.find(p);
  return it == conn.end() ? none : it->second;
}

static const std::set<std::string> kFlops = {"$dff",   "$dffe",  "$adff",   "$adffe", "$sdff", "$sdffe",
                                             "$sdffce", "$dffsr", "$dffsre", "$aldff", "$aldffe"};
static const std::set<std::string> kLatches = {"$dlatch", "$adlatch", "$dlatchsr"};
static const std::set<std::string> kBitwise = {"$and", "$or",   "$xor",   "$xnor",  "$not",   "$pos",
                                               "$_AND_", "$_OR_", "$_XOR_", "$_NOT_", "$_BUF_", "$_NAND_",
                                               "$_NOR_", "$_XNOR_"};

CellKind Netlist::kind(int cell) const {
  const std::string& t = cells[cell].type;
  if (kFlops.count(t)) return CellKind::Flop;
  if (kLatches.count(t)) return CellKind::Latch;
  if (t.empty() || t[0] != '$') return CellKind::Other;  // a blackbox or unflattened module
  return CellKind::Comb;
}

std::vector<int> comb_inputs(const Cell& c, const std::string& out_port, int idx) {
  std::vector<int> r;
  auto bitwise = [&](const char* p) {
    const auto& v = c.pin(p);
    if (idx < (int)v.size()) r.push_back(v[idx]);
    else if (!v.empty() && c.param(std::string(p) + "_SIGNED")) r.push_back(v.back());
  };
  if (kBitwise.count(c.type)) {
    bitwise("A");
    bitwise("B");
  } else if (c.type == "$mux" || c.type == "$_MUX_") {
    bitwise("A");
    bitwise("B");
    for (int b : c.pin("S")) r.push_back(b);
  } else if (c.type == "$pmux") {
    const auto& a = c.pin("A");
    const auto& b = c.pin("B");
    int w = (int)a.size();
    if (idx < w) r.push_back(a[idx]);
    for (int k = 0; w > 0 && k * w + idx < (int)b.size(); k++) r.push_back(b[k * w + idx]);
    for (int s : c.pin("S")) r.push_back(s);
  } else {
    for (const auto& [p, bits] : c.conn)
      if (p != out_port && !c.is_out.at(p)) r.insert(r.end(), bits.begin(), bits.end());
  }
  return r;
}

Netlist Netlist::load(const json& j, const std::string& want_top) {
  Netlist nl;
  const json& mods = j.at("modules");
  std::string top = want_top;
  if (top.empty())
    for (const auto& [n, m] : mods.items())
      if (m.contains("attributes") && m["attributes"].contains("top")) top = n;
  if (top.empty() && mods.size() == 1) top = mods.begin().key();
  if (!mods.contains(top)) throw std::runtime_error("top module '" + top + "' not found in netlist");
  nl.top = top;
  const json& m = mods[top];

  auto note = [&](int b) { nl.max_bit = std::max(nl.max_bit, b); };

  for (const auto& [pn, pj] : m.at("ports").items()) {
    std::vector<int> bits;
    for (const auto& b : pj.at("bits")) bits.push_back(bit_of(b));
    bool in = pj.at("direction") == "input";
    (in ? nl.inputs : nl.outputs)[pn] = bits;
    for (int i = 0; i < (int)bits.size(); i++) {
      if (is_const(bits[i])) continue;
      note(bits[i]);
      if (in) nl.driver[bits[i]] = {-1, pn, i};
      else nl.loads[bits[i]].push_back({-1, pn, i});
    }
  }

  for (const auto& [cn, cj] : m.at("cells").items()) {
    Cell c;
    c.name = cn;
    c.type = cj.at("type");
    if (cj.contains("parameters"))
      for (const auto& [k, v] : cj["parameters"].items()) c.params[k] = v.is_string() ? v.get<std::string>() : v.dump();
    for (const auto& [p, bits] : cj.at("connections").items()) {
      std::vector<int> v;
      for (const auto& b : bits) v.push_back(bit_of(b));
      c.conn[p] = v;
      std::string dir = cj.contains("port_directions") && cj["port_directions"].contains(p)
                            ? cj["port_directions"][p].get<std::string>()
                            : "input";
      c.is_out[p] = dir == "output";
    }
    int id = (int)nl.cells.size();
    for (const auto& [p, bits] : c.conn)
      for (int i = 0; i < (int)bits.size(); i++) {
        if (is_const(bits[i])) continue;
        note(bits[i]);
        if (c.is_out[p]) nl.driver[bits[i]] = {id, p, i};
        else nl.loads[bits[i]].push_back({id, p, i});
      }
    nl.cells.push_back(std::move(c));
  }

  // Names: top ports first, then public names (the shallowest alias wins:
  // it's what you see from the top, and any public alias works as a
  // hierarchical path in a testbench), then Yosys's internal ones.
  std::map<int, std::pair<int, std::string>> best;  // bit -> (score, name)
  auto offer = [&](int b, int score, const std::string& n) {
    auto it = best.find(b);
    if (it == best.end() || score > it->second.first ||
        (score == it->second.first && (n.size() < it->second.second.size() ||
                                       (n.size() == it->second.second.size() && n < it->second.second))))
      best[b] = {score, n};
  };
  auto indexed = [](const std::string& n, int width, int i, long offset, bool upto) {
    if (width == 1 && offset == 0) return n;
    long k = upto ? offset + width - 1 - i : offset + i;
    return n + "[" + std::to_string(k) + "]";
  };
  for (auto* ports : {&nl.inputs, &nl.outputs})
    for (const auto& [pn, bits] : *ports)
      for (int i = 0; i < (int)bits.size(); i++)
        if (!is_const(bits[i])) offer(bits[i], 1000, indexed(pn, (int)bits.size(), i, 0, false));
  for (const auto& [nn, nj] : m.at("netnames").items()) {
    bool hidden = nj.value("hide_name", 0) != 0 || nn.empty() || nn[0] == '$';
    int depth = (int)std::count(nn.begin(), nn.end(), '.');
    int score = hidden ? 0 : 500 - depth;
    const auto& bits = nj.at("bits");
    long offset = nj.value("offset", 0L);
    bool upto = nj.value("upto", 0) != 0;
    for (int i = 0; i < (int)bits.size(); i++) {
      int b = bit_of(bits[i]);
      if (!is_const(b)) offer(b, score, indexed(nn, (int)bits.size(), i, offset, upto));
    }
  }
  for (const auto& [b, sn] : best) nl.bit_name[b] = sn.second;
  return nl;
}

std::string Netlist::name(int bit) const {
  if (bit == kConst0) return "1'b0";
  if (bit == kConst1) return "1'b1";
  if (bit == kConstX) return "1'bx";
  auto it = bit_name.find(bit);
  return it != bit_name.end() ? it->second : "$bit" + std::to_string(bit);
}

std::string Netlist::input_port(int bit) const {
  auto it = driver.find(bit);
  return it != driver.end() && it->second.cell == -1 ? it->second.port : "";
}

}  // namespace cg
