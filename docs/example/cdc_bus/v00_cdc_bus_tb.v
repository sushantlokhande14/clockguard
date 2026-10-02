// clockguard: targeted test for CDC_BUS (count_a -> u_sync.s1)
// monitor: bus, window 0.5 ns, prints CG_HAZARD <time> <detail> for each hit
`timescale 1ns/1ps
module tb;
  integer seed = 7, hazards = 0, k, n;
  realtime first = -1, dt;
  reg clk_a;
  reg clk_b;
  reg rst_n;
  wire [3:0] count_b;
  cdc_bus dut (.clk_a(clk_a), .clk_b(clk_b), .rst_n(rst_n), .count_b(count_b));
  initial begin
    clk_a = 0;
    clk_b = 0;
    rst_n = 0;
  end
  initial begin #0.000; forever #5.0000 clk_a = ~clk_a; end
  initial begin #1.370; forever #3.6500 clk_b = ~clk_b; end
  initial #40.37 rst_n = 1;
  task hazard(input real d);
    begin
      hazards = hazards + 1;
      if (first < 0) first = $realtime;
      if (hazards <= 20) $display("CG_HAZARD %0.3f %0.3f", $realtime, d);
    end
  endtask
  realtime t_b [0:3];
  initial for (k = 0; k < 4; k = k + 1) t_b[k] = -1e9;
  always @(dut.count_a[0]) t_b[0] = $realtime;
  always @(dut.count_a[1]) t_b[1] = $realtime;
  always @(dut.count_a[2]) t_b[2] = $realtime;
  always @(dut.count_a[3]) t_b[3] = $realtime;
  always @(posedge clk_b) if ($realtime > 70.37) begin n = 0; for (k = 0; k < 4; k = k + 1) if ($realtime - t_b[k] > 0 && $realtime - t_b[k] < 0.5) n = n + 1; if (n >= 2) hazard(n); end
  initial begin $dumpfile("docs/example/cdc_bus/v00_cdc_bus.vcd"); $dumpvars(0, tb.clk_a, tb.clk_b, tb.rst_n, tb.dut.count_a, tb.dut.u_sync.s1); end
  initial begin #20000; $display("CG_DONE %0d %0.3f", hazards, first); $finish; end
endmodule
