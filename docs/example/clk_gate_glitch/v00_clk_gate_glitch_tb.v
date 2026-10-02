// clockguard: targeted test for CLK_GATE_GLITCH (en_q -> gclk)
// monitor: glitch, window 0.5 ns, prints CG_HAZARD <time> <detail> for each hit
`timescale 1ns/1ps
module tb;
  integer seed = 7, hazards = 0, k, n;
  realtime first = -1, dt;
  reg clk;
  reg rst_n;
  reg en;
  reg [7:0] d;
  wire [7:0] q;
  clk_gate_glitch dut (.clk(clk), .rst_n(rst_n), .en(en), .d(d), .q(q));
  initial begin
    clk = 0;
    rst_n = 0;
    en = 0;
    d = 0;
  end
  initial begin #0.000; forever #5.0000 clk = ~clk; end
  always @(negedge clk) d = $random(seed);
  always @(negedge clk) en = $random(seed);
  initial #40.37 rst_n = 1;
  task hazard(input real d);
    begin
      hazards = hazards + 1;
      if (first < 0) first = $realtime;
      if (hazards <= 20) $display("CG_HAZARD %0.3f %0.3f", $realtime, d);
    end
  endtask
  realtime t_r = -1e9, t_f = -1e9;
  always @(dut.gclk) begin if (dut.gclk === 1'b1) begin if ($realtime > 70.37 && $realtime - t_f < 4.000) hazard($realtime - t_f); t_r = $realtime; end else if (dut.gclk === 1'b0) begin if ($realtime > 70.37 && $realtime - t_r < 4.000) hazard($realtime - t_r); t_f = $realtime; end end
  initial begin $dumpfile("docs/example/clk_gate_glitch/v00_clk_gate_glitch.vcd"); $dumpvars(0, tb.clk, tb.rst_n, tb.dut.gclk, tb.dut.en_q); end
  initial begin #20000; $display("CG_DONE %0d %0.3f", hazards, first); $finish; end
endmodule
