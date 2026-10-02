// expect: clean
// A gray-coded counter crosses as a bus: only one bit changes per clk_a
// cycle, so a per-bit synchronizer is safe (clockguard checks the gray
// encoding in the netlist).
module gray_counter (
  input        clk_a,
  input        clk_b,
  input        rst_n,
  input        inc,
  output [4:0] count_b
);
  wire rst_a_n, rst_b_n;
  rst_sync u_ra (.clk(clk_a), .arst_n(rst_n), .rst_n(rst_a_n));
  rst_sync u_rb (.clk(clk_b), .arst_n(rst_n), .rst_n(rst_b_n));

  reg [4:0] bin, gray;
  wire [4:0] bin_next = bin + {4'd0, inc};
  always @(posedge clk_a or negedge rst_a_n)
    if (!rst_a_n) begin
      bin <= 5'd0;
      gray <= 5'd0;
    end else begin
      bin <= bin_next;
      gray <= bin_next ^ (bin_next >> 1);
    end

  sync2 #(.W(5)) u_sync (.clk(clk_b), .rst_n(rst_b_n), .d(gray), .q(count_b));
endmodule
