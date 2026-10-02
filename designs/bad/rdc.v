// expect: RDC
// cfg is reset by its own (synchronized) software reset, the datapath by
// the main reset. Asserting sw_rst_n alone clears cfg asynchronously while
// acc keeps sampling it.
module rdc (
  input            clk,
  input            rst_n,
  input            sw_rst_n,
  input      [7:0] cfg_in,
  output reg [7:0] acc
);
  wire main_rst_n, cfg_rst_n;
  rst_sync u_rm (.clk(clk), .arst_n(rst_n), .rst_n(main_rst_n));
  rst_sync u_rc (.clk(clk), .arst_n(sw_rst_n), .rst_n(cfg_rst_n));

  reg [7:0] cfg;
  always @(posedge clk or negedge cfg_rst_n)
    if (!cfg_rst_n) cfg <= 8'd0;
    else cfg <= cfg_in;

  always @(posedge clk or negedge main_rst_n)
    if (!main_rst_n) acc <= 8'd0;
    else acc <= acc + cfg;
endmodule
