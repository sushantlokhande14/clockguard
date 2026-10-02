#include "analysis.h"

#include <algorithm>
#include <cctype>
#include <deque>
#include <sstream>

namespace cg {

Constraints Constraints::from_json(const json& j) {
  Constraints c;
  if (j.contains("clocks"))
    for (const auto& [k, v] : j["clocks"].items()) {
      c.clocks.insert(k);
      if (v.is_object() && v.contains("period")) c.periods[k] = v["period"].get<double>();
    }
  if (j.contains("groups"))
    for (const auto& g : j["groups"]) c.groups.push_back(g.get<std::vector<std::string>>());
  if (j.contains("async_inputs"))
    for (const auto& s : j["async_inputs"]) c.async_inputs.insert(s.get<std::string>());
  if (j.contains("inputs"))
    for (const auto& [k, v] : j["inputs"].items()) c.input_clock[k] = v.get<std::string>();
  if (j.contains("resets"))
    for (const auto& [k, v] : j["resets"].items())
      if (v.is_object() && v.contains("sync_to")) c.reset_sync_to[k] = v["sync_to"].get<std::string>();
  if (j.contains("waivers"))
    for (const auto& w : j["waivers"])
      c.waivers.push_back({w.value("rule", "*"), w.value("from", "*"), w.value("to", "*"), w.value("reason", "")});
  return c;
}

bool glob_match(const std::string& p, const std::string& s) {
  size_t i = 0, j = 0, star = std::string::npos, mark = 0;
  while (j < s.size()) {
    if (i < p.size() && (p[i] == '?' || p[i] == s[j])) {
      i++;
      j++;
    } else if (i < p.size() && p[i] == '*') {
      star = i++;
      mark = j;
    } else if (star != std::string::npos) {
      i = star + 1;
      j = ++mark;
    } else {
      return false;
    }
  }
  while (i < p.size() && p[i] == '*') i++;
  return i == p.size();
}

namespace {

bool is_not(const Cell& c) {
  return c.type == "$not" || c.type == "$_NOT_" || (c.type == "$logic_not" && c.pin("A").size() == 1);
}
bool is_buf(const Cell& c) { return c.type == "$pos" || c.type == "$_BUF_"; }
bool is_and(const Cell& c) {
  return c.type == "$and" || c.type == "$_AND_" ||
         (c.type == "$logic_and" && c.pin("A").size() == 1 && c.pin("B").size() == 1);
}
bool is_or(const Cell& c) {
  return c.type == "$or" || c.type == "$_OR_" ||
         (c.type == "$logic_or" && c.pin("A").size() == 1 && c.pin("B").size() == 1);
}
bool is_mux(const Cell& c) { return c.type == "$mux" || c.type == "$_MUX_"; }

int in_bit(const Cell& c, const char* p, int idx) {
  const auto& v = c.pin(p);
  if (v.empty()) return kConstX;
  return idx < (int)v.size() ? v[idx] : v.back();
}

// "a_r[3]" -> "a_r"
std::string base(const std::string& n) {
  auto lb = n.rfind('[');
  return lb != std::string::npos && n.back() == ']' ? n.substr(0, lb) : n;
}

bool looks_like_clock(std::string n) {
  for (char& c : n) c = (char)std::tolower((unsigned char)c);
  if (n.find("clk") == std::string::npos && n.find("clock") == std::string::npos) return false;
  std::stringstream ss(n);
  std::string tok;
  while (std::getline(ss, tok, '_'))
    if (tok == "en" || tok == "enable" || tok == "sel" || tok == "gate" || tok == "gated") return false;
  return true;
}

struct Gate {
  int cell, out, clk, en;
  bool is_and, clk_inv;
};

struct Trace {
  enum Kind { Port, Flop, Latch, Mux, Const, Logic, Combined } kind = Logic;
  int root = kConstX;
  bool inv = false;
  std::vector<Gate> gates;
  std::vector<std::pair<int, int>> data_muxes;  // (cell, data bit): a clock muxed with a data signal
  std::vector<int> bits;
  int mux_cell = -1;
};

struct Domain {
  std::string name, kind;  // primary, derived, mux, logic, async
  int root = kConstX;
  int parent = -1;
  std::vector<int> inputs;
  int flops = 0, latches = 0, pos = 0, neg = 0;
  std::set<std::string> gates;
};

struct Viol {
  std::string rule, sev, msg, from, to, from_dom, to_dom;
  std::vector<int> path;       // bits, source first
  std::vector<int> dest_bits;  // where the fan-in cone starts
  json monitor = json::object();
  int count = 0;
  std::set<std::string> bits;  // aggregated destination bits
  std::set<std::string> extra; // rule-specific details collected while aggregating
};

struct ResetSrc {
  std::string id, name, kind;  // kind: input, synchronizer, generated
  int sync_dom = -1;           // domain it releases synchronously to; -1: an asynchronous input
  std::set<std::string> roots; // reset inputs behind it
  int stages = 0;
  int bit = kConstX;
};

struct Sync {
  int src_bit = kConstX;
  int src_cell = -1;  // -1: an input port
  std::string src_name;
  int src_group = -1, src_dom = -1, dst_dom = -1;
  std::vector<std::pair<int, int>> stages;  // (cell, bit index)
};

struct Source {
  int bit;
  bool qualified;
  bool direct;
  std::vector<int> path;
};

class Analyzer {
 public:
  Analyzer(const Netlist& nl, const Constraints& con)
      : nl_(nl), con_(con), cell_dom_(nl.cells.size(), -3), cell_pos_(nl.cells.size(), 1),
        cell_trace_(nl.cells.size()) {}

  json run() {
    for (int c = 0; c < (int)nl_.cells.size(); c++) {
      CellKind k = nl_.kind(c);
      if (k == CellKind::Flop) flops_.push_back(c);
      if (k == CellKind::Latch) latches_.push_back(c);
      if (k == CellKind::Flop || k == CellKind::Latch) seq_.push_back(c);
    }
    clocks();
    resets();
    find_syncs();
    reset_consumers();
    cdc();
    bus_and_reconvergence();
    return finish();
  }

 private:
  // ---------------------------------------------------------------- domains

  int find(int d) {
    while (uf_[d] != d) d = uf_[d] = uf_[uf_[d]];
    return d;
  }
  void unite(int a, int b) { uf_[find(a)] = find(b); }

  int domain(const std::string& key, const std::string& name, const std::string& kind, int root, bool* created) {
    auto it = dom_key_.find(key);
    *created = it == dom_key_.end();
    if (!*created) return it->second;
    Domain d;
    d.name = name;
    d.kind = kind;
    d.root = root;
    doms_.push_back(d);
    uf_.push_back((int)uf_.size());
    return dom_key_[key] = (int)doms_.size() - 1;
  }

  int domain_by_name(const std::string& n) {
    for (int i = 0; i < (int)doms_.size(); i++)
      if (doms_[i].name == n) return i;
    return -1;
  }

  bool clockish(int b, bool weak) {
    if (is_const(b)) return false;
    if (strong_seed_.count(b) || (weak && weak_seed_.count(b))) return true;
    auto& memo = clockish_memo_[weak];
    auto it = memo.find(b);
    if (it != memo.end()) return it->second;
    memo[b] = false;  // cycle guard
    bool r = false;
    auto d = nl_.driver.find(b);
    if (d != nl_.driver.end() && d->second.cell >= 0) {
      const Cell& c = nl_.cells[d->second.cell];
      int i = d->second.index;
      if (is_not(c) || is_buf(c)) r = clockish(in_bit(c, "A", i), weak);
      else if (is_and(c) || is_or(c) || is_mux(c))
        r = clockish(in_bit(c, "A", i), weak) || clockish(in_bit(c, "B", i), weak);
    }
    return memo[b] = r;
  }

  // Walk back from a clock pin to where the clock comes from. Gates record
  // the inversion between the pin and the gate while walking; afterwards
  // that's turned into the clock's polarity at the gate relative to the root.
  Trace trace_clock(int b) {
    Trace t = walk_clock(b);
    for (auto& g : t.gates) g.clk_inv = g.clk_inv != t.inv;
    return t;
  }

  Trace walk_clock(int b) {
    Trace t;
    for (int guard = 0; guard < 256; guard++) {
      t.root = b;
      if (is_const(b)) {
        t.kind = Trace::Const;
        return t;
      }
      t.bits.push_back(b);
      auto it = nl_.driver.find(b);
      if (it == nl_.driver.end()) {
        t.kind = Trace::Const;  // undriven
        return t;
      }
      const PinRef& d = it->second;
      if (d.cell < 0) {
        t.kind = Trace::Port;
        return t;
      }
      const Cell& c = nl_.cells[d.cell];
      CellKind k = nl_.kind(d.cell);
      if (k == CellKind::Flop) {
        t.kind = Trace::Flop;
        return t;
      }
      if (k == CellKind::Latch) {
        t.kind = Trace::Latch;
        return t;
      }
      if (is_not(c)) {
        t.inv = !t.inv;
        b = in_bit(c, "A", d.index);
        continue;
      }
      if (is_buf(c)) {
        b = in_bit(c, "A", d.index);
        continue;
      }
      if (is_and(c) || is_or(c) || is_mux(c)) {
        int x = in_bit(c, "A", d.index), y = in_bit(c, "B", d.index);
        bool cx = clockish(x, false), cy = clockish(y, false);
        if (!cx && !cy) {
          cx = clockish(x, true);
          cy = clockish(y, true);
        }
        if (!cx && !cy) {
          t.kind = Trace::Logic;
          return t;
        }
        if (cx && cy) {
          t.kind = is_mux(c) ? Trace::Mux : Trace::Combined;
          t.mux_cell = d.cell;
          return t;
        }
        int clk = cx ? x : y, other = cx ? y : x;
        if (!is_mux(c)) {
          t.gates.push_back({d.cell, b, clk, other, is_and(c), t.inv});
        } else if (is_const(other)) {  // sel ? clk : 0 is a gate written as a mux
          t.gates.push_back({d.cell, b, clk, in_bit(c, "S", 0), other == kConst0, t.inv});
        } else {
          t.data_muxes.push_back({d.cell, other});
        }
        b = clk;
        continue;
      }
      t.kind = Trace::Logic;
      return t;
    }
    t.kind = Trace::Logic;
    return t;
  }

  int domain_for_trace(const Trace& t, bool latch) {
    bool created = false;
    switch (t.kind) {
      case Trace::Port: {
        std::string p = nl_.input_port(t.root);
        clock_ports_.insert(p);
        return domain("p:" + p, p, "primary", t.root, &created);
      }
      case Trace::Flop: {
        int d = domain("q:" + std::to_string(t.root), nl_.name(t.root), "derived", t.root, &created);
        if (created) {
          int pd = cell_domain(nl_.driver.at(t.root).cell);
          doms_[d].parent = pd;
          if (pd >= 0) unite(d, pd);
        }
        return d;
      }
      case Trace::Mux: {
        int d = domain("m:" + std::to_string(t.root), nl_.name(t.root), "mux", t.root, &created);
        if (created) {
          const Cell& m = nl_.cells[t.mux_cell];
          int idx = nl_.driver.at(t.root).index;
          std::set<int> groups;
          for (const char* p : {"A", "B"}) {
            int di = domain_for_trace(trace_clock(in_bit(m, p, idx)), false);
            doms_[d].inputs.push_back(di);
            groups.insert(di >= 0 ? find(di) : -1);
          }
          if (groups.size() == 1 && *groups.begin() >= 0) unite(d, *groups.begin());
        }
        return d;
      }
      case Trace::Const:
        return -1;
      default:
        if (latch) return -1;  // a data latch, not a clocked element
        return domain("l:" + std::to_string(t.root), nl_.name(t.root), "logic", t.root, &created);
    }
  }

  int cell_domain(int c) {
    if (cell_dom_[c] != -3) return cell_dom_[c];
    cell_dom_[c] = -2;  // in progress: a flop clocked by its own output
    const Cell& cell = nl_.cells[c];
    bool latch = nl_.kind(c) == CellKind::Latch;
    Trace t = trace_clock(in_bit(cell, latch ? "EN" : "CLK", 0));
    int d = domain_for_trace(t, latch);
    bool pol = cell.param(latch ? "EN_POLARITY" : "CLK_POLARITY", 1) != 0;
    cell_pos_[c] = pol != t.inv;
    cell_trace_[c] = t;
    return cell_dom_[c] = d;
  }

  // ----------------------------------------------------------------- clocks

  void clocks() {
    for (const auto& p : con_.clocks)
      if (nl_.inputs.count(p)) {
        clock_ports_.insert(p);
        for (int b : nl_.inputs.at(p)) strong_seed_.insert(b);
      }
    // ports and flop outputs that reach a flop clock pin through buffers and inverters only
    for (int c : flops_) {
      int b = in_bit(nl_.cells[c], "CLK", 0);
      for (int guard = 0; guard < 64 && !is_const(b); guard++) {
        auto it = nl_.driver.find(b);
        if (it == nl_.driver.end()) break;
        if (it->second.cell < 0) {
          clock_ports_.insert(it->second.port);
          strong_seed_.insert(b);
          break;
        }
        const Cell& d = nl_.cells[it->second.cell];
        if (nl_.kind(it->second.cell) == CellKind::Flop) {
          strong_seed_.insert(b);
          break;
        }
        if (!is_not(d) && !is_buf(d)) break;
        b = in_bit(d, "A", it->second.index);
      }
    }
    for (const auto& [p, bits] : nl_.inputs)
      if (looks_like_clock(p))
        for (int b : bits) weak_seed_.insert(b);

    for (int c : seq_) cell_domain(c);
    for (const auto& g : con_.groups)
      for (size_t i = 1; i < g.size(); i++) {
        int a = domain_by_name(g[0]), b = domain_by_name(g[i]);
        if (a >= 0 && b >= 0) unite(a, b);
      }

    std::map<int, Viol> from_logic, consts;
    std::set<int> gates_seen, muxes_seen;
    for (int c : seq_) {
      const Trace& t = cell_trace_[c];
      int d = cell_dom_[c];
      bool latch = nl_.kind(c) == CellKind::Latch;
      int width = (int)nl_.cells[c].pin("Q").size();
      if (d >= 0) {
        (latch ? doms_[d].latches : doms_[d].flops) += width;
        (cell_pos_[c] ? doms_[d].pos : doms_[d].neg) += latch ? 0 : width;
      }
      if (d < 0 && latch) continue;  // a data latch: its enable isn't a clock
      // every clock net except a flop output feeding a clock pin (that's a plain flop)
      size_t keep = t.bits.size();
      if (keep && (t.kind == Trace::Flop || t.kind == Trace::Latch || t.kind == Trace::Logic ||
                   t.kind == Trace::Combined))
        keep--;
      for (size_t i = 0; i < keep; i++) clock_bits_.insert(t.bits[i]);

      for (const auto& g : t.gates) {
        if (d >= 0) doms_[d].gates.insert(nl_.name(g.out));
        if (gates_seen.insert(g.cell).second && d >= 0) check_gate(g, d);
      }
      for (const auto& [mc, data] : t.data_muxes)
        if (muxes_seen.insert(mc).second) {
          Viol v;
          v.rule = "CLK_MUX";
          v.sev = "warning";
          int out = nl_.cells[mc].pin("Y").empty() ? kConstX : nl_.cells[mc].pin("Y")[0];
          v.from = nl_.name(data);
          v.to = nl_.name(out);
          v.to_dom = d >= 0 ? doms_[d].name : "";
          v.msg = "clock " + v.to_dom + " is muxed with data signal " + v.from + " at " + v.to +
                  "; flops behind it can be clocked by data";
          v.monitor = {{"type", "glitch"}, {"net", v.to}, {"clock", v.to_dom}};
          v.path = {data, out};
          add(v);
        }
      if (t.kind == Trace::Logic || t.kind == Trace::Combined || (t.kind == Trace::Latch && !latch)) {
        Viol& v = from_logic[t.root];
        v.count += width;
        if (v.to.empty()) {
          v.to = base(nl_.name(nl_.cells[c].pin("Q")[0]));
          v.dest_bits = {in_bit(nl_.cells[c], "CLK", 0)};
          v.path = t.bits;
          std::reverse(v.path.begin(), v.path.end());
          v.extra.insert(t.kind == Trace::Combined ? "combined" : t.kind == Trace::Latch ? "latch" : "logic");
        }
      }
      if (t.kind == Trace::Const && !latch) {
        Viol& v = consts[t.root];
        v.count += width;
        if (v.to.empty()) v.to = base(nl_.name(nl_.cells[c].pin("Q")[0]));
      }
    }
    for (auto& [root, v] : from_logic) {
      v.rule = "CLK_FROM_LOGIC";
      v.sev = "error";
      v.from = nl_.name(root);
      v.to_dom = v.from;
      std::string what = *v.extra.begin() == "combined" ? "combines two clocks in logic"
                         : *v.extra.begin() == "latch" ? "is a latch output"
                                                       : "comes from logic, not from a clock input, clock gate or divider flop";
      v.msg = std::to_string(v.count) + " flop bit(s) (" + v.to + "...) are clocked by " + v.from + ", which " + what +
              "; the clock can glitch and has no defined relation to any other domain";
      v.monitor = {{"type", "glitch"}, {"net", v.from}, {"clock", v.from}};
      add(v);
    }
    for (auto& [root, v] : consts) {
      v.rule = "CLK_CONST";
      v.sev = "warning";
      v.from = nl_.name(root);
      v.msg = std::to_string(v.count) + " flop bit(s) (" + v.to + "...) have a constant or undriven clock and never update";
      add(v);
    }
    for (int d = 0; d < (int)doms_.size(); d++) {
      const Domain& dm = doms_[d];
      if (dm.kind == "mux" && dm.inputs.size() == 2) {
        Viol v;
        v.rule = "CLK_MUX";
        v.sev = "warning";
        auto nm = [&](int i) { return i >= 0 ? doms_[i].name : std::string("?"); };
        v.from = nm(dm.inputs[0]) + "," + nm(dm.inputs[1]);
        v.to = dm.name;
        v.to_dom = dm.name;
        v.msg = "clock mux " + dm.name + " switches between " + nm(dm.inputs[0]) + " and " + nm(dm.inputs[1]) +
                "; changing the select while either clock is high produces a short pulse unless the mux is "
                "glitch-free";
        v.monitor = {{"type", "glitch"}, {"net", dm.name}, {"clock", nm(dm.inputs[0])}};
        add(v);
      }
      if (dm.kind == "derived")
        notes_.push_back("derived clock " + dm.name + " (a flop output) clocks " + std::to_string(dm.flops) +
                         " flop bit(s); treated as synchronous to " +
                         (dm.parent >= 0 ? doms_[dm.parent].name : std::string("its source")));
    }
  }

  void check_gate(const Gate& g, int dom) {
    // The root clock level during which the enable may change without
    // chopping a pulse: low for an AND gate, high for an OR (flipped if the
    // gate sees an inverted clock).
    bool safe_high = g.is_and ? g.clk_inv : !g.clk_inv;
    auto level = [](bool high) { return std::string(high ? "high" : "low"); };
    int b = g.en;
    for (int guard = 0; guard < 64; guard++) {
      auto it = nl_.driver.find(b);
      if (it == nl_.driver.end() || it->second.cell < 0) break;
      const Cell& c = nl_.cells[it->second.cell];
      if (!is_not(c) && !is_buf(c)) break;
      b = in_bit(c, "A", it->second.index);
    }
    Viol v;
    v.from = nl_.name(b);
    v.to = nl_.name(g.out);
    v.to_dom = doms_[dom].name;
    v.path = {b, g.en, g.out};
    v.dest_bits = {g.out};
    v.monitor = {{"type", "glitch"}, {"net", v.to}, {"clock", doms_[dom].name}, {"enable", v.from}};
    std::string clk = doms_[dom].name;
    auto it = nl_.driver.find(b);
    int ec = it != nl_.driver.end() ? it->second.cell : -1;
    CellKind k = ec >= 0 ? nl_.kind(ec) : CellKind::Other;
    if (ec >= 0 && (k == CellKind::Latch || k == CellKind::Flop)) {
      int sd = cell_domain(ec);
      v.from_dom = sd >= 0 ? doms_[sd].name : "";
      if (sd != dom) {
        v.rule = "CLK_GATE_DOMAIN";
        v.sev = "error";
        v.msg = "clock gate " + v.to + " on " + clk + " takes its enable from " + v.from + ", which is clocked by " +
                (sd >= 0 ? doms_[sd].name : std::string("nothing")) +
                "; the enable can change at any phase of " + clk + ". Launch the enable from " + clk +
                " (through a synchronizer if it starts elsewhere)";
        add(v);
        return;
      }
      bool changes_high = k == CellKind::Latch
                              ? (nl_.cells[ec].param("EN_POLARITY", 1) != 0) != cell_trace_[ec].inv
                              : (bool)cell_pos_[ec];
      if (changes_high == safe_high) return;  // a proper latch-based (or opposite-edge) gate
      v.rule = "CLK_GATE_GLITCH";
      v.sev = "error";
      if (k == CellKind::Latch)
        v.msg = "clock gate " + v.to + ": enable latch " + v.from + " is transparent while " + clk + " is " +
                level(changes_high) + ", the same phase in which the gate passes the clock; the enable can change "
                "mid-pulse. The latch must be transparent while " + clk + " is " + level(safe_high);
      else
        v.msg = "clock gate " + v.to + ": enable " + v.from + " comes from a " +
                (changes_high ? "posedge" : "negedge") + " flop, so it changes while " + clk + " is " +
                level(changes_high) + " and can chop a clock pulse. Use a latch-based clock gate (latch transparent "
                "while " + clk + " is " + level(safe_high) + ")";
      add(v);
      return;
    }
    v.rule = "CLK_GATE_GLITCH";
    v.sev = "error";
    v.msg = "clock gate " + v.to + ": enable " + v.from + " comes from " +
            (ec < 0 ? std::string("an input port") : std::string("logic")) + ", not from a latch or flop of " + clk +
            "; it can change while " + clk + " is " + level(!safe_high) +
            " and glitch the gated clock. Use a latch-based clock gate";
    add(v);
  }

  // ----------------------------------------------------------------- resets

  std::vector<std::pair<int, bool>> reset_pins(int c) {  // (bit, active high at the pin)
    const Cell& cl = nl_.cells[c];
    std::vector<std::pair<int, bool>> r;
    auto take = [&](const char* p, const char* pol) {
      for (int b : cl.pin(p))
        if (!is_const(b)) {
          r.push_back({b, cl.param(pol, 1) != 0});
          return;  // the first bit stands for the vector
        }
    };
    if (cl.has("ARST")) take("ARST", "ARST_POLARITY");
    if (cl.has("CLR")) take("CLR", "CLR_POLARITY");
    if (cl.has("SET")) take("SET", "SET_POLARITY");
    if (cl.has("ALOAD")) take("ALOAD", "ALOAD_POLARITY");
    return r;
  }

  struct REnd {
    int kind;  // 0 port, 1 flop, 2 logic, 3 constant
    int bit;
    bool inv;
  };

  REnd reset_end(int b) {
    bool inv = false;
    for (int guard = 0; guard < 64; guard++) {
      if (is_const(b)) return {3, b, inv};
      auto it = nl_.driver.find(b);
      if (it == nl_.driver.end()) return {3, b, inv};
      if (it->second.cell < 0) return {0, b, inv};
      const Cell& c = nl_.cells[it->second.cell];
      CellKind k = nl_.kind(it->second.cell);
      if (k == CellKind::Flop) return {1, b, inv};
      if (is_not(c)) inv = !inv;
      else if (!is_buf(c)) return {2, b, inv};
      b = in_bit(c, "A", it->second.index);
    }
    return {2, b, inv};
  }

  // ports and flop outputs a piece of reset logic is built from
  std::vector<int> reset_roots(int b) {
    std::vector<int> out;
    std::set<int> seen;
    std::deque<int> q{b};
    while (!q.empty() && seen.size() < 512) {
      int x = q.front();
      q.pop_front();
      if (is_const(x) || !seen.insert(x).second) continue;
      auto it = nl_.driver.find(x);
      if (it == nl_.driver.end()) continue;
      if (it->second.cell < 0 || nl_.kind(it->second.cell) != CellKind::Comb) {
        out.push_back(x);
        continue;
      }
      for (int y : comb_inputs(nl_.cells[it->second.cell], it->second.port, it->second.index)) q.push_back(y);
    }
    return out;
  }

  const ResetSrc& reset_src(int bit) {
    auto it = rsrc_.find(bit);
    if (it != rsrc_.end()) return it->second;
    ResetSrc r;
    r.bit = bit;
    const PinRef& d = nl_.driver.at(bit);
    if (d.cell < 0) {
      r.id = "in:" + d.port;
      r.name = d.port;
      r.kind = "input";
      r.roots = {d.port};
      reset_ports_.insert(d.port);
      auto st = con_.reset_sync_to.find(d.port);
      if (st != con_.reset_sync_to.end()) r.sync_dom = domain_by_name(st->second);
      return rsrc_[bit] = r;
    }
    r.name = nl_.name(bit);
    r.id = "q:" + r.name;
    r.sync_dom = cell_domain(d.cell);
    r.kind = "generated";
    // walk the D chain back: const -> f1 -> f2 ... is a reset synchronizer
    std::vector<std::pair<int, int>> chain{{d.cell, d.index}};
    int start_kind = -1;  // 0 const, 1 port
    std::string start_port;
    for (int guard = 0; guard < 8; guard++) {
      auto [cc, ci] = chain.back();
      int db = in_bit(nl_.cells[cc], "D", ci);
      if (is_const(db)) {
        start_kind = 0;
        break;
      }
      auto di = nl_.driver.find(db);
      if (di == nl_.driver.end()) break;
      if (di->second.cell < 0) {
        start_kind = 1;
        start_port = di->second.port;
        break;
      }
      if (nl_.kind(di->second.cell) != CellKind::Flop || cell_domain(di->second.cell) != r.sync_dom) break;
      chain.push_back({di->second.cell, di->second.index});
    }
    if (start_kind == 0 || (start_kind == 1 && (reset_ports_.count(start_port) || looks_like_reset(start_port)))) {
      r.kind = "synchronizer";
      r.stages = (int)chain.size();
      for (const auto& [cc, ci] : chain) {
        rst_sync_cells_.insert(cc);
        for (const auto& [pb, hi] : reset_pins(cc)) {
          REnd e = reset_end(pb);
          std::vector<int> ends{e.bit};
          if (e.kind == 2) ends = reset_roots(e.bit);  // resets combined before the synchronizer
          for (int x : ends) {
            std::string p = nl_.input_port(x);
            if (p.empty()) continue;
            r.roots.insert(p);
            reset_ports_.insert(p);
          }
        }
      }
      if (start_kind == 1) {
        r.roots.insert(start_port);
        reset_ports_.insert(start_port);
      }
      if (r.stages < 2) {
        Viol v;
        v.rule = "RST_SYNC_SHORT";
        v.sev = "warning";
        v.from = r.roots.empty() ? r.name : *r.roots.begin();
        v.to = r.name;
        v.to_dom = r.sync_dom >= 0 ? doms_[r.sync_dom].name : "";
        v.msg = "reset synchronizer " + r.name + " for " + v.to_dom + " has 1 stage; its output can be metastable "
                "when the reset releases and reaches the flops it resets. Use 2 or more stages";
        v.path = {bit};
        v.dest_bits = {bit};
        v.monitor = {{"type", "reset"}, {"reset", v.from}, {"clock", v.to_dom}};
        add(v);
      }
    }
    return rsrc_[bit] = r;
  }

  static bool looks_like_reset(std::string n) {
    for (char& c : n) c = (char)std::tolower((unsigned char)c);
    return n.find("rst") != std::string::npos || n.find("reset") != std::string::npos;
  }

  // the reset sources behind one reset pin (several if it's built from logic)
  std::vector<const ResetSrc*> sources_of(int pin, int* logic_bit) {
    std::vector<const ResetSrc*> out;
    REnd e = reset_end(pin);
    *logic_bit = kConstX;
    if (e.kind == 0 || e.kind == 1) out.push_back(&reset_src(e.bit));
    if (e.kind == 2) {
      *logic_bit = e.bit;
      for (int r : reset_roots(e.bit)) {
        auto it = nl_.driver.find(r);
        if (it == nl_.driver.end()) continue;
        if (it->second.cell < 0 || nl_.kind(it->second.cell) == CellKind::Flop) out.push_back(&reset_src(r));
      }
    }
    return out;
  }

  void resets() {
    // pass 1: find every reset source (this marks synchronizer flops)
    for (int c : seq_)
      for (const auto& [pin, hi] : reset_pins(c)) {
        int lb;
        for (const ResetSrc* r : sources_of(pin, &lb)) {
          if (r->kind == "input" && !reset_active_low_.count(r->name)) {
            REnd e = reset_end(pin);
            reset_active_low_[r->name] = hi == e.inv;
          }
        }
      }
  }

  void reset_consumers() {
    std::map<std::string, Viol> agg;
    for (int c : seq_) {
      int dc = cell_dom_[c];
      if (dc < 0) continue;
      std::set<std::string> ids, roots;
      for (const auto& [pin, hi] : reset_pins(c)) {
        int logic_bit;
        auto srcs = sources_of(pin, &logic_bit);
        if (!is_const(logic_bit) && !rst_sync_cells_.count(c)) {
          std::string key = "RST_COMB|" + nl_.name(logic_bit);
          Viol& v = agg[key];
          v.count++;
          if (v.rule.empty()) {
            v.rule = "RST_COMB";
            v.sev = "warning";
            v.from = nl_.name(logic_bit);
            v.to = base(nl_.name(nl_.cells[c].pin("Q")[0]));
            v.to_dom = doms_[dc].name;
            v.path = {logic_bit, pin, nl_.cells[c].pin("Q")[0]};
            v.dest_bits = {pin};
            for (auto* r : srcs) v.extra.insert(r->name);
            v.monitor = {{"type", "pulse"}, {"net", v.from}, {"clock", v.to_dom}};
          }
        }
        for (const ResetSrc* r : srcs) {
          ids.insert(r->id);
          roots.insert(r->roots.begin(), r->roots.end());
          json& out = resets_out_[r->id];
          if (out.is_null())
            out = {{"name", r->name}, {"kind", r->kind}, {"stages", r->stages},
                   {"domain", r->sync_dom >= 0 ? doms_[r->sync_dom].name : ""},
                   {"roots", std::vector<std::string>(r->roots.begin(), r->roots.end())},
                   {"consumers", json::object()}};
          out["consumers"][doms_[dc].name] = out["consumers"].value(doms_[dc].name, 0) +
                                             (int)nl_.cells[c].pin("Q").size();
          if (rst_sync_cells_.count(c)) continue;
          std::string rule;
          if (r->sync_dom < 0) rule = "RST_UNSYNC";
          else if (find(r->sync_dom) != find(dc)) rule = "RST_WRONG_DOMAIN";
          if (rule.empty()) continue;
          Viol& v = agg[rule + "|" + r->id + "|" + std::to_string(find(dc))];
          v.count += (int)nl_.cells[c].pin("Q").size();
          if (v.rule.empty()) {
            v.rule = rule;
            v.sev = "error";
            v.from = r->name;
            v.from_dom = r->sync_dom >= 0 ? doms_[r->sync_dom].name : "";
            v.to = base(nl_.name(nl_.cells[c].pin("Q")[0]));
            v.to_dom = doms_[dc].name;
            v.path = {r->bit, pin, nl_.cells[c].pin("Q")[0]};
            v.dest_bits = {pin};
            bool low = r->roots.empty() || !reset_active_low_.count(*r->roots.begin())
                           ? true
                           : reset_active_low_[*r->roots.begin()];
            v.monitor = {{"type", "reset"}, {"reset", r->name}, {"active_low", low}, {"clock", v.to_dom},
                         {"edge", cell_pos_[c] ? "posedge" : "negedge"}};
          }
        }
      }
      if (!ids.empty() && !rst_sync_cells_.count(c)) {
        std::string id;
        for (const auto& s : ids) id += (id.empty() ? "" : "+") + s;
        cell_reset_[c] = id;
        cell_reset_roots_[c] = roots;
      }
    }
    for (auto& [key, v] : agg) {
      std::string n = std::to_string(v.count) + " flop bit(s) in " + v.to_dom + " (" + v.to + "...)";
      if (v.rule == "RST_UNSYNC")
        v.msg = n + " are reset by input " + v.from + ", which can release at any point of the " + v.to_dom +
                " cycle; some flops leave reset an edge before others (recovery/removal). Add a reset synchronizer "
                "clocked by " + v.to_dom + " (assert asynchronously, release through 2 flops)";
      else if (v.rule == "RST_WRONG_DOMAIN")
        v.msg = n + " are reset by " + v.from + ", which is synchronized to " + v.from_dom + ", not " + v.to_dom +
                "; its release is asynchronous to " + v.to_dom + ". Use a reset synchronizer clocked by " + v.to_dom;
      else {
        std::string ins;
        for (const auto& s : v.extra) ins += (ins.empty() ? "" : ", ") + s;
        v.msg = "reset " + v.from + " is built from logic (" + ins + ") and drives " + std::to_string(v.count) +
                " async reset pin(s) in " + v.to_dom + "; a glitch on any input resets them. Combine resets "
                "before the synchronizer, or make sure every input is glitch-free";
      }
      add(v);
    }
  }

  // -------------------------------------------------------------------- CDC

  static constexpr int kUnconstrained = -1, kClockSrc = -2;

  int async_group(const std::string& port) {
    bool created;
    int d = domain("a:" + port, "async:" + port, "async", kConstX, &created);
    return find(d);
  }

  // which clock group launches this bit; -1: assume synchronous to whoever samples it
  int group_of(int b, int* dom_out = nullptr) {
    if (dom_out) *dom_out = -1;
    auto it = nl_.driver.find(b);
    if (it == nl_.driver.end()) return kUnconstrained;
    const PinRef& d = it->second;
    if (d.cell < 0) {
      const std::string& p = d.port;
      if (clock_ports_.count(p)) return kClockSrc;
      auto ic = con_.input_clock.find(p);
      if (ic != con_.input_clock.end()) {
        int dd = domain_by_name(ic->second);
        if (dom_out) *dom_out = dd;
        return dd >= 0 ? find(dd) : kUnconstrained;
      }
      auto st = con_.reset_sync_to.find(p);
      if (st != con_.reset_sync_to.end()) {
        int dd = domain_by_name(st->second);
        if (dom_out) *dom_out = dd;
        return dd >= 0 ? find(dd) : kUnconstrained;
      }
      if (reset_ports_.count(p) || con_.async_inputs.count(p)) {
        int g = async_group(p);
        if (dom_out) *dom_out = dom_key_.at("a:" + p);
        return g;
      }
      return kUnconstrained;
    }
    CellKind k = nl_.kind(d.cell);
    if (k != CellKind::Flop && k != CellKind::Latch) return kUnconstrained;
    int dd = cell_dom_[d.cell];
    if (dom_out) *dom_out = dd;
    return dd >= 0 ? find(dd) : kUnconstrained;
  }

  bool is_flop_d(const PinRef& l, int group) {
    return l.cell >= 0 && nl_.kind(l.cell) == CellKind::Flop && l.port == "D" && cell_dom_[l.cell] >= 0 &&
           find(cell_dom_[l.cell]) == group;
  }

  void find_syncs() {
    for (int c : flops_) {
      int dc = cell_dom_[c];
      if (dc < 0) continue;
      int gc = find(dc);
      const Cell& cell = nl_.cells[c];
      const auto& d = cell.pin("D");
      const auto& q = cell.pin("Q");
      for (int i = 0; i < (int)d.size() && i < (int)q.size(); i++) {
        int sd;
        int g = group_of(d[i], &sd);
        if (g < 0 || g == gc) continue;
        auto lit = nl_.loads.find(q[i]);
        if (lit == nl_.loads.end() || lit->second.size() != 1 || !is_flop_d(lit->second[0], gc)) continue;
        Sync s;
        s.src_bit = d[i];
        const PinRef& drv = nl_.driver.at(d[i]);
        s.src_cell = drv.cell;
        s.src_name = drv.cell < 0 ? drv.port : base(nl_.name(d[i]));
        s.src_group = g;
        s.src_dom = sd;
        s.dst_dom = dc;
        s.stages.push_back({c, i});
        PinRef next = lit->second[0];
        for (int guard = 0; guard < 6; guard++) {
          s.stages.push_back({next.cell, next.index});
          int qb = nl_.cells[next.cell].pin("Q")[next.index];
          auto nl = nl_.loads.find(qb);
          if (nl == nl_.loads.end() || nl->second.size() != 1 || !is_flop_d(nl->second[0], gc)) break;
          next = nl->second[0];
        }
        int id = (int)syncs_.size();
        stage1_[{c, i}] = id;
        for (size_t k = 1; k < s.stages.size(); k++)
          sync_out_[nl_.cells[s.stages[k].first].pin("Q")[s.stages[k].second]] = id;
        syncs_.push_back(s);
      }
    }
  }

  // clock groups whose synchronized signals appear in the cone of a mux select
  uint64_t qual_mask(int b) {
    auto it = qual_memo_.find(b);
    if (it != qual_memo_.end()) return it->second;
    uint64_t m = 0;
    std::set<int> seen;
    std::deque<std::pair<int, int>> q{{b, 0}};  // (bit, flops crossed)
    while (!q.empty() && seen.size() < 4096) {
      auto [x, depth] = q.front();
      q.pop_front();
      if (is_const(x) || !seen.insert(x).second) continue;
      auto so = sync_out_.find(x);
      if (so != sync_out_.end() && syncs_[so->second].src_group < 64) m |= 1ull << syncs_[so->second].src_group;
      auto dit = nl_.driver.find(x);
      if (dit == nl_.driver.end() || dit->second.cell < 0) continue;
      const Cell& c = nl_.cells[dit->second.cell];
      CellKind k = nl_.kind(dit->second.cell);
      if (k == CellKind::Comb) {
        for (int y : comb_inputs(c, dit->second.port, dit->second.index)) q.push_back({y, depth});
      } else if (k == CellKind::Flop && depth < 3) {
        for (const char* p : {"D", "EN"})
          if (c.has(p)) q.push_back({in_bit(c, p, p[0] == 'D' ? dit->second.index : 0), depth + 1});
      }
    }
    return qual_memo_[b] = m;
  }

  // Sources (flop/latch outputs, input ports) behind one destination bit,
  // and whether every path from each source passes a mux whose select is
  // synchronized from that source's clock group.
  std::map<int, Source> data_sources(const std::vector<int>& starts, int d_start, std::set<int>* clock_hits) {
    using St = std::pair<int, uint64_t>;
    std::map<St, St> parent;
    std::deque<St> q;
    for (int s : starts) {
      St k{s, 0};
      if (parent.emplace(k, St{kConstX, 0}).second) q.push_back(k);
    }
    std::map<int, Source> out;
    auto push = [&](int x, uint64_t m, const St& from) {
      St k{x, m};
      if (parent.emplace(k, from).second) q.push_back(k);
    };
    while (!q.empty() && parent.size() < 200000) {
      St cur = q.front();
      q.pop_front();
      int b = cur.first;
      if (is_const(b)) continue;
      auto it = nl_.driver.find(b);
      if (it == nl_.driver.end()) continue;
      if (clock_bits_.count(b)) {
        clock_hits->insert(b);
        continue;
      }
      const PinRef& d = it->second;
      if (d.cell < 0 || nl_.kind(d.cell) != CellKind::Comb) {
        int g = group_of(b);
        bool qual = g >= 0 && g < 64 && ((cur.second >> g) & 1);
        auto sit = out.find(b);
        if (sit == out.end()) {
          std::vector<int> path;
          for (St s = cur; !is_const(s.first); s = parent.at(s)) path.push_back(s.first);
          out[b] = {b, qual, b == d_start, path};
        } else {
          sit->second.qualified = sit->second.qualified && qual;
        }
        continue;
      }
      const Cell& c = nl_.cells[d.cell];
      if (is_mux(c) || c.type == "$pmux") {
        uint64_t qm = 0;
        for (int s : c.pin("S")) qm |= qual_mask(s);
        int w = (int)c.pin("A").size();
        push(in_bit(c, "A", d.index), cur.second | qm, cur);
        if (is_mux(c)) push(in_bit(c, "B", d.index), cur.second | qm, cur);
        else
          for (int k = 0; w > 0 && k * w + d.index < (int)c.pin("B").size(); k++)
            push(c.pin("B")[k * w + d.index], cur.second | qm, cur);
        for (int s : c.pin("S")) push(s, cur.second, cur);
      } else {
        for (int x : comb_inputs(c, d.port, d.index)) push(x, cur.second, cur);
      }
    }
    return out;
  }

  void cdc() {
    std::map<std::string, Viol> agg;
    std::map<std::string, json> crossings;
    for (int c : seq_) {
      int dc = cell_dom_[c];
      if (dc < 0) continue;
      int gc = find(dc);
      const Cell& cell = nl_.cells[c];
      bool latch = nl_.kind(c) == CellKind::Latch;
      const auto& d = cell.pin("D");
      const auto& q = cell.pin("Q");
      std::vector<int> extra;
      if (!latch)
        for (const char* p : {"EN", "SRST"})
          for (int b : cell.pin(p)) extra.push_back(b);
      for (int i = 0; i < (int)d.size() && i < (int)q.size(); i++) {
        std::vector<int> starts{d[i]};
        starts.insert(starts.end(), extra.begin(), extra.end());
        std::set<int> clock_hits;
        auto srcs = data_sources(starts, d[i], &clock_hits);
        std::string dst = nl_.name(q[i]);

        for (int cb : clock_hits) {
          Viol& v = agg["CLK_AS_DATA|" + nl_.name(cb) + "|" + std::to_string(dc)];
          v.count++;
          if (v.rule.empty()) {
            v.rule = "CLK_AS_DATA";
            v.sev = "warning";
            v.from = nl_.name(cb);
            v.to = dst;
            v.to_dom = doms_[dc].name;
            v.dest_bits = {d[i]};
            v.path = {cb, d[i], q[i]};
            v.monitor = {{"type", "capture"}, {"src", {v.from}}, {"dst", dst}, {"clock", v.to_dom},
                         {"edge", cell_pos_[c] ? "posedge" : "negedge"}, {"same_edge", true}};
          }
        }

        std::vector<const Source*> cross;
        for (const auto& [b, s] : srcs) {
          int sd;
          int g = group_of(b, &sd);
          if (g == kUnconstrained) {
            std::string p = nl_.input_port(b);
            if (!p.empty()) inputs_seen_[p].insert(doms_[dc].name);
            continue;
          }
          if (g == gc) {
            int sc = nl_.driver.at(b).cell;
            if (sc >= 0 && sc != c) same_group_srcs_[c].insert(sc);
            continue;
          }
          if (g >= 0) cross.push_back(&s);
        }
        if (cross.empty()) continue;

        auto st1 = stage1_.find({c, i});
        bool all_qual = true;
        for (auto* s : cross) all_qual &= s->qualified;
        std::string status = st1 != stage1_.end() ? "synchronized" : all_qual ? "qualified" : "violation";
        for (auto* s : cross) {
          int sd;
          group_of(s->bit, &sd);
          std::string from = base(nl_.name(s->bit));
          json& x = crossings[from + "|" + base(dst) + "|" + status];
          if (x.is_null())
            x = {{"from", from}, {"to", base(dst)}, {"from_domain", sd >= 0 ? doms_[sd].name : ""},
                 {"to_domain", doms_[dc].name}, {"status", status}, {"bits", 0}};
          x["bits"] = x["bits"].get<int>() + 1;
        }
        if (status != "violation") continue;

        // what does the captured value feed?
        auto lit = nl_.loads.find(q[i]);
        int n_loads = lit == nl_.loads.end() ? 0 : (int)lit->second.size();
        bool flops_only = n_loads > 0;
        if (lit != nl_.loads.end())
          for (const auto& l : lit->second) flops_only &= is_flop_d(l, gc);
        bool single_direct = cross.size() == 1 && cross[0]->direct && srcs.size() == 1;

        std::string rule, kind;
        if (flops_only && single_direct) rule = "CDC_SYNC_FANOUT";
        else if (flops_only) rule = "CDC_COMB_BEFORE_SYNC";
        else {
          rule = "CDC_UNSYNC";
          kind = single_direct ? "single" : "none";
        }
        const Source* first = cross[0];
        int sd;
        group_of(first->bit, &sd);
        std::string from = base(nl_.name(first->bit));
        Viol& v = agg[rule + "|" + from + "|" + base(dst)];
        v.count++;
        v.bits.insert(dst);
        if (v.rule.empty()) {
          v.rule = rule;
          v.sev = rule == "CDC_SYNC_FANOUT" ? "warning" : "error";
          v.from = from;
          v.to = base(dst);
          v.from_dom = sd >= 0 ? doms_[sd].name : "";
          v.to_dom = doms_[dc].name;
          v.path = first->path;
          v.path.push_back(q[i]);
          v.dest_bits = {d[i]};
          v.extra.insert(kind);
          v.extra.insert("loads:" + std::to_string(n_loads));
          std::vector<std::string> src_names;
          for (auto* s : cross)
            if (src_names.size() < 8) src_names.push_back(nl_.name(s->bit));
          v.monitor = {{"type", "capture"}, {"src", src_names}, {"dst", dst}, {"clock", v.to_dom},
                       {"edge", cell_pos_[c] ? "posedge" : "negedge"}};
          if (v.from_dom.size()) v.monitor["src_clock"] = v.from_dom;
        }
        for (auto* s : cross) {
          std::string other = nl_.name(s->bit);
          if (base(other) != v.from) v.extra.insert("also:" + base(other));
        }
      }
    }
    for (auto& [key, v] : agg) {
      std::string bits = v.count > 1 ? " (" + std::to_string(v.count) + " bits)" : "";
      std::string from = v.from + " (" + v.from_dom + ")", to = v.to + " (" + v.to_dom + ")";
      std::string also;
      for (const auto& e : v.extra)
        if (e.rfind("also:", 0) == 0) also += ", " + e.substr(5);
      if (v.rule == "CLK_AS_DATA")
        v.msg = "clock " + v.from + " is used as data at " + v.to + " (" + v.to_dom +
                "); a clock sampled by a flop has no stable value. Use a toggle flop or an enable instead";
      else if (v.rule == "CDC_SYNC_FANOUT") {
        std::string n;
        for (const auto& e : v.extra)
          if (e.rfind("loads:", 0) == 0) n = e.substr(6);
        v.msg = "first synchronizer stage " + v.to + bits + " for " + from + " feeds " + n +
                " flops; each can resolve a metastable value differently. Only the second stage should read it";
      } else if (v.rule == "CDC_COMB_BEFORE_SYNC")
        v.msg = "synchronizer " + to + bits + " samples logic built from " + from + also +
                " instead of a flop output; a glitch in that logic can be captured as a real pulse. Register the "
                "signal in " + v.from_dom + " before it crosses";
      else if (v.extra.count("single"))
        v.msg = to + bits + " captures " + from + " in a single flop whose output goes straight into logic; a "
                "metastable value spreads before it settles. Add a second synchronizer stage";
      else
        v.msg = from + also + " reaches " + to + bits + " through logic with no synchronizer; " + v.to +
                " can go metastable or capture a mix of old and new bits. Use a 2-flop synchronizer for a level, a "
                "handshake or an async FIFO for data";
      add(v);
    }
    for (auto& [k, x] : crossings) crossings_.push_back(x);

    // reset domain crossings: data between flops reset by unrelated resets
    std::map<std::string, Viol> rdc;
    for (const auto& [c, srcs] : same_group_srcs_) {
      auto rc = cell_reset_.find(c);
      if (rc == cell_reset_.end()) continue;
      for (int a : srcs) {
        auto ra = cell_reset_.find(a);
        if (ra == cell_reset_.end() || ra->second == rc->second) continue;
        if (cell_reset_roots_[a] == cell_reset_roots_[c]) continue;
        Viol& v = rdc[ra->second + "|" + rc->second + "|" + std::to_string(cell_dom_[c])];
        v.count++;
        if (v.rule.empty()) {
          v.rule = "RDC";
          v.sev = "warning";
          v.from = base(nl_.name(nl_.cells[a].pin("Q")[0]));
          v.to = base(nl_.name(nl_.cells[c].pin("Q")[0]));
          v.from_dom = v.to_dom = doms_[cell_dom_[c]].name;
          v.dest_bits = {nl_.cells[c].pin("D")[0]};
          v.path = {nl_.cells[a].pin("Q")[0], nl_.cells[c].pin("D")[0], nl_.cells[c].pin("Q")[0]};
          std::string ra_name = ra->second.substr(ra->second.find(':') + 1);
          std::string rc_name = rc->second.substr(rc->second.find(':') + 1);
          v.msg = v.from + " is reset by " + ra_name + " but feeds " + v.to + ", reset by " + rc_name +
                  "; when " + ra_name + " asserts on its own, " + v.from + " changes asynchronously and " + v.to +
                  " (still running) can go metastable. Reset both from the same source, or hold " + v.to +
                  " while " + ra_name + " is asserted";
          std::vector<std::string> src;
          for (int b : nl_.cells[a].pin("Q"))
            if (src.size() < 8) src.push_back(nl_.name(b));
          std::vector<std::string> pulse;  // resets that hit the source but not the victim
          for (const auto& r : cell_reset_roots_[a])
            if (!cell_reset_roots_[c].count(r)) pulse.push_back(r);
          v.monitor = {{"type", "capture"}, {"src", src}, {"dst", nl_.name(nl_.cells[c].pin("Q")[0])},
                       {"clock", v.to_dom}, {"edge", cell_pos_[c] ? "posedge" : "negedge"}, {"pulse", pulse}};
        }
      }
    }
    for (auto& [k, v] : rdc) add(v);
  }

  // Gray code: every bit is next ^ (next >> 1) for the same vector `next`.
  bool is_gray(int cell) {
    const Cell& c = nl_.cells[cell];
    const auto& d = c.pin("D");
    const auto& q = c.pin("Q");
    if (d.size() < 2) return false;
    int xor_cell = -1;
    std::vector<int> a, b;
    for (int i = 0; i < (int)d.size(); i++) {
      int x = d[i];
      for (int guard = 0; guard < 4; guard++) {  // skip enable (feedback) and sync-reset (constant) muxes
        auto it = nl_.driver.find(x);
        if (it == nl_.driver.end() || it->second.cell < 0) break;
        const Cell& m = nl_.cells[it->second.cell];
        if (!is_mux(m)) break;
        int ma = in_bit(m, "A", it->second.index), mb = in_bit(m, "B", it->second.index);
        if (ma == q[i] || is_const(ma)) x = mb;
        else if (mb == q[i] || is_const(mb)) x = ma;
        else break;
      }
      auto it = nl_.driver.find(x);
      if (it == nl_.driver.end() || it->second.cell < 0) return false;
      const Cell& xc = nl_.cells[it->second.cell];
      if (xc.type != "$xor" || it->second.index != i) return false;
      if (xor_cell >= 0 && xor_cell != it->second.cell) return false;
      xor_cell = it->second.cell;
      a = xc.pin("A");
      b = xc.pin("B");
    }
    auto shifted = [&](const std::vector<int>& v, const std::vector<int>& w) {  // w == v >> 1 ?
      if (w.size() < d.size() || v.size() < d.size()) return false;
      auto it = nl_.driver.find(w[0]);
      if (it != nl_.driver.end() && it->second.cell >= 0) {
        const Cell& s = nl_.cells[it->second.cell];
        if ((s.type == "$shr" || s.type == "$sshr" || s.type == "$shiftx") && s.pin("A") == v) {
          const auto& sb = s.pin("B");
          long amt = 0;
          for (size_t k = 0; k < sb.size(); k++) {
            if (!is_const(sb[k]) || sb[k] == kConstX) return false;
            if (sb[k] == kConst1) amt |= 1L << k;
          }
          return amt == 1;
        }
      }
      for (size_t k = 0; k + 1 < d.size(); k++)
        if (w[k] != v[k + 1]) return false;
      return w[d.size() - 1] == kConst0;
    };
    return shifted(a, b) || shifted(b, a);
  }

  void bus_and_reconvergence() {
    // bits of one source register synchronized separately
    std::map<std::pair<std::string, int>, std::vector<int>> by_src;
    for (int i = 0; i < (int)syncs_.size(); i++)
      by_src[{syncs_[i].src_name, find(syncs_[i].dst_dom)}].push_back(i);
    for (const auto& [key, ids] : by_src) {
      const Sync& s0 = syncs_[ids[0]];
      std::string to = base(nl_.name(nl_.cells[s0.stages[0].first].pin("Q")[s0.stages[0].second]));
      json sj = {{"from", s0.src_name}, {"to", to}, {"from_domain", s0.src_dom >= 0 ? doms_[s0.src_dom].name : ""},
                 {"to_domain", doms_[s0.dst_dom].name}, {"bits", ids.size()}, {"stages", s0.stages.size()}};
      bool gray = ids.size() > 1 && s0.src_cell >= 0 && is_gray(s0.src_cell);
      sj["kind"] = ids.size() == 1 ? "bit" : gray ? "gray-bus" : "bus";
      syncs_out_.push_back(sj);
      if (ids.size() < 2 || gray) continue;
      Viol v;
      v.rule = "CDC_BUS";
      v.sev = "error";
      v.from = s0.src_name;
      v.to = to;
      v.from_dom = sj["from_domain"];
      v.to_dom = sj["to_domain"];
      v.count = (int)ids.size();
      std::vector<std::string> src, dst;
      for (int id : ids) {
        src.push_back(nl_.name(syncs_[id].src_bit));
        const auto& st = syncs_[id].stages[0];
        dst.push_back(nl_.name(nl_.cells[st.first].pin("Q")[st.second]));
        v.dest_bits.push_back(nl_.cells[st.first].pin("D")[st.second]);
      }
      v.path = {s0.src_bit, nl_.cells[s0.stages[0].first].pin("Q")[s0.stages[0].second]};
      v.msg = v.from + " (" + std::to_string(ids.size()) + " bits, " + v.from_dom +
              ") is synchronized bit by bit into " + v.to + " (" + v.to_dom +
              "); bits that change together can land in different cycles and give a value that never existed. "
              "Gray-code it so one bit changes at a time, or use a handshake or an async FIFO";
      v.monitor = {{"type", "bus"}, {"src", src}, {"dst", dst}, {"clock", v.to_dom}, {"src_clock", v.from_dom}};
      add(v);
    }

    // separately synchronized signals from one domain meeting again
    std::map<std::string, Viol> agg;
    for (int c : seq_) {
      int dc = cell_dom_[c];
      if (dc < 0) continue;
      const Cell& cell = nl_.cells[c];
      std::set<int> seen;
      std::map<int, std::set<std::string>> by_group;  // source group -> synchronized source registers
      std::map<std::string, int> sample_bit;
      std::deque<int> q;
      for (int b : cell.pin("D")) q.push_back(b);
      for (const char* p : {"EN", "SRST"})
        for (int b : cell.pin(p)) q.push_back(b);
      while (!q.empty() && seen.size() < 20000) {
        int x = q.front();
        q.pop_front();
        if (is_const(x) || !seen.insert(x).second) continue;
        auto so = sync_out_.find(x);
        if (so != sync_out_.end()) {
          const Sync& s = syncs_[so->second];
          by_group[s.src_group].insert(s.src_name);
          sample_bit.emplace(s.src_name, x);
          continue;
        }
        auto it = nl_.driver.find(x);
        if (it == nl_.driver.end() || it->second.cell < 0 || nl_.kind(it->second.cell) != CellKind::Comb) continue;
        for (int y : comb_inputs(nl_.cells[it->second.cell], it->second.port, it->second.index)) q.push_back(y);
      }
      for (const auto& [g, names] : by_group) {
        if (names.size() < 2) continue;
        std::string key;
        for (const auto& n : names) key += n + ",";
        Viol& v = agg[key];
        v.count++;
        if (!v.rule.empty()) continue;
        v.rule = "CDC_RECONV";
        v.sev = "warning";
        auto it = names.begin();
        v.from = *it + ", " + *std::next(it);
        v.to = base(nl_.name(cell.pin("Q")[0]));
        v.to_dom = doms_[dc].name;
        int s0 = sync_out_.at(sample_bit[*names.begin()]);
        v.from_dom = syncs_[s0].src_dom >= 0 ? doms_[syncs_[s0].src_dom].name : "";
        v.dest_bits = {cell.pin("D")[0]};
        v.path = {sample_bit[*names.begin()], cell.pin("D")[0], cell.pin("Q")[0]};
        std::vector<std::string> src;
        for (const auto& n : names) src.push_back(nl_.name(syncs_[sync_out_.at(sample_bit[n])].src_bit));
        v.msg = std::to_string(names.size()) + " signals from " + v.from_dom + " (" + key.substr(0, key.size() - 1) +
                ") are synchronized separately and meet again at " + v.to + " (" + v.to_dom +
                "); they can arrive a cycle apart, so " + v.to + " can see a combination that never existed. "
                "Synchronize one signal and derive the rest, or use a handshake";
        // the risk is at the sources: two of them changing inside one capture window
        v.monitor = {{"type", "bus"}, {"src", src}, {"dst", {nl_.name(cell.pin("Q")[0])}}, {"clock", v.to_dom},
                     {"edge", cell_pos_[c] ? "posedge" : "negedge"}};
      }
    }
    for (auto& [k, v] : agg) add(v);
  }

  // ----------------------------------------------------------------- output

  void add(const Viol& v) { viols_.push_back(v); }

  int cone_size(const std::vector<int>& starts) {
    std::set<int> seen;
    std::deque<int> q(starts.begin(), starts.end());
    while (!q.empty() && seen.size() < 1000000) {
      int x = q.front();
      q.pop_front();
      if (is_const(x) || !seen.insert(x).second) continue;
      auto it = nl_.driver.find(x);
      if (it == nl_.driver.end() || it->second.cell < 0) continue;
      const Cell& c = nl_.cells[it->second.cell];
      CellKind k = nl_.kind(it->second.cell);
      if (k == CellKind::Comb) {
        for (int y : comb_inputs(c, it->second.port, it->second.index)) q.push_back(y);
      } else {
        for (const auto& [p, bits] : c.conn)  // data and reset pins; clock pins are a separate tree
          if (!c.is_out.at(p) && p != "CLK" && !(k == CellKind::Latch && p == "EN"))
            for (int y : bits) q.push_back(y);
      }
    }
    return (int)seen.size();
  }

  json finish() {
    json out;
    out["top"] = nl_.top;
    std::map<int, int> group_ids;
    auto gid = [&](int d) {
      int r = find(d);
      return group_ids.emplace(r, (int)group_ids.size()).first->second;
    };
    json doms = json::array();
    for (int d = 0; d < (int)doms_.size(); d++) {
      const Domain& dm = doms_[d];
      json j = {{"name", dm.name}, {"kind", dm.kind}, {"group", gid(d)}, {"flops", dm.flops},
                {"latches", dm.latches}, {"posedge", dm.pos}, {"negedge", dm.neg},
                {"gates", std::vector<std::string>(dm.gates.begin(), dm.gates.end())}};
      if (dm.parent >= 0) j["parent"] = doms_[dm.parent].name;
      if (con_.periods.count(dm.name)) j["period"] = con_.periods.at(dm.name);
      doms.push_back(j);
    }
    out["domains"] = doms;
    json resets = json::array();
    for (auto& [id, r] : resets_out_) {
      if (r["kind"] == "input" && reset_active_low_.count(r["name"])) r["active_low"] = reset_active_low_[r["name"]];
      resets.push_back(r);
    }
    out["resets"] = resets;
    json inputs = json::object();
    for (const auto& [p, bits] : nl_.inputs) {
      std::string kind = clock_ports_.count(p) ? "clock" : reset_ports_.count(p) ? "reset"
                         : con_.async_inputs.count(p) ? "async" : con_.input_clock.count(p) ? "launched" : "data";
      json j = {{"kind", kind}, {"width", bits.size()}};
      if (kind == "reset" && reset_active_low_.count(p)) j["active_low"] = reset_active_low_[p];
      if (con_.input_clock.count(p)) j["clock"] = con_.input_clock.at(p);
      auto s = inputs_seen_.find(p);
      if (s != inputs_seen_.end()) j["sampled_by"] = std::vector<std::string>(s->second.begin(), s->second.end());
      inputs[p] = j;
    }
    out["inputs"] = inputs;
    out["synchronizers"] = syncs_out_;
    out["crossings"] = crossings_;

    json vs = json::array();
    int errors = 0, warnings = 0, waived = 0;
    std::sort(viols_.begin(), viols_.end(), [](const Viol& a, const Viol& b) {
      if (a.sev != b.sev) return a.sev < b.sev;  // errors first
      return std::tie(a.rule, a.to, a.from) < std::tie(b.rule, b.to, b.from);
    });
    for (const auto& v : viols_) {
      json j = {{"rule", v.rule}, {"severity", v.sev}, {"message", v.msg}, {"from", v.from}, {"to", v.to},
                {"from_domain", v.from_dom}, {"to_domain", v.to_dom}, {"monitor", v.monitor}};
      std::vector<std::string> path;
      for (int b : v.path)
        if (path.empty() || path.back() != nl_.name(b)) path.push_back(nl_.name(b));
      j["path"] = path;
      j["path_nets"] = (int)std::set<int>(v.path.begin(), v.path.end()).size();
      j["cone_nets"] = v.dest_bits.empty() ? 0 : cone_size(v.dest_bits);
      for (const auto& w : con_.waivers)
        if ((w.rule == "*" || w.rule == v.rule) && glob_match(w.from, v.from) && glob_match(w.to, v.to)) {
          j["waived"] = w.reason.empty() ? "waived" : w.reason;
          break;
        }
      if (j.contains("waived")) waived++;
      else (v.sev == "error" ? errors : warnings)++;
      vs.push_back(j);
    }
    out["violations"] = vs;
    out["notes"] = notes_;
    int flop_bits = 0, latch_bits = 0;
    for (const auto& d : doms_) {
      flop_bits += d.flops;
      latch_bits += d.latches;
    }
    out["summary"] = {{"cells", nl_.cells.size()}, {"flop_bits", flop_bits}, {"latch_bits", latch_bits},
                      {"domains", doms_.size()}, {"groups", group_ids.size()}, {"synchronizers", syncs_out_.size()},
                      {"crossings", crossings_.size()}, {"errors", errors}, {"warnings", warnings},
                      {"waived", waived}};
    return out;
  }

  const Netlist& nl_;
  const Constraints& con_;
  std::vector<int> flops_, latches_, seq_;
  std::vector<Domain> doms_;
  std::map<std::string, int> dom_key_;
  std::vector<int> uf_;
  std::vector<int> cell_dom_;
  std::vector<char> cell_pos_;
  std::vector<Trace> cell_trace_;
  std::set<int> strong_seed_, weak_seed_, clock_bits_;
  std::set<std::string> clock_ports_;
  std::map<int, bool> clockish_memo_[2];
  std::vector<Viol> viols_;
  json notes_ = json::array();

  std::map<int, ResetSrc> rsrc_;
  std::set<int> rst_sync_cells_;
  std::set<std::string> reset_ports_;
  std::map<std::string, bool> reset_active_low_;
  std::map<std::string, json> resets_out_;
  std::map<int, std::string> cell_reset_;
  std::map<int, std::set<std::string>> cell_reset_roots_;

  std::vector<Sync> syncs_;
  std::map<std::pair<int, int>, int> stage1_;
  std::map<int, int> sync_out_;
  std::map<int, uint64_t> qual_memo_;
  std::map<int, std::set<int>> same_group_srcs_;
  std::map<std::string, std::set<std::string>> inputs_seen_;
  json syncs_out_ = json::array();
  json crossings_ = json::array();
};

}  // namespace

json analyze(const Netlist& nl, const Constraints& con) { return Analyzer(nl, con).run(); }

}  // namespace cg
