// clockguard: targeted test for RST_UNSYNC (rst_n -> q)
// monitor: reset, window 0.5 ns, prints CG_HAZARD <time> <detail> for each hit
`timescale 1ns/1ps
module tb;
  integer seed = 7, hazards = 0, k, n;
  realtime first = -1, dt;
  reg clk;
  reg rst_n;
  reg [7:0] d;
  wire [7:0] q;
  rst_unsync dut (.clk(clk), .rst_n(rst_n), .d(d), .q(q));
  initial begin
    clk = 0;
    rst_n = 0;
    d = 0;
  end
  initial begin #0.000; forever #5.0000 clk = ~clk; end
  always @(negedge clk) d = $random(seed);
  initial begin #40.37; rst_n = 1; repeat (40) begin #200.000; rst_n = 0; repeat (3) @(posedge clk); @(posedge clk); #9.7500 rst_n = 1; end end
  task hazard(input real d);
    begin
      hazards = hazards + 1;
      if (first < 0) first = $realtime;
      if (hazards <= 20) $display("CG_HAZARD %0.3f %0.3f", $realtime, d);
    end
  endtask
  realtime t_rel = -1e9;
  always @(rst_n) if (rst_n === 1'b1) t_rel = $realtime;
  always @(posedge clk) begin dt = $realtime - t_rel; if (dt > 0 && dt < 0.5) hazard(dt); end
  initial begin $dumpfile("docs/example/rst_unsync/v00_rst_unsync.vcd"); $dumpvars(0, tb.clk, tb.rst_n); end
  initial begin #20000; $display("CG_DONE %0d %0.3f", hazards, first); $finish; end
endmodule
