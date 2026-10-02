// expect: CDC_UNSYNC
// A push button (declared asynchronous in async_input.cg.json) drives
// logic directly.
module async_input (
  input            clk,
  input            rst_n,
  input            button,
  output reg [7:0] presses
);
  wire rst_s_n;
  rst_sync u_r (.clk(clk), .arst_n(rst_n), .rst_n(rst_s_n));

  reg last;
  always @(posedge clk or negedge rst_s_n)
    if (!rst_s_n) begin
      last <= 1'b0;
      presses <= 8'd0;
    end else begin
      last <= button;
      if (button && !last) presses <= presses + 8'd1;
    end
endmodule
