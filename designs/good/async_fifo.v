// expect: clean
// Dual-clock FIFO in the usual style: binary pointers address the memory,
// gray-coded copies cross the boundary through two-flop synchronizers, and
// full/empty are computed against the synchronized pointers.
module async_fifo #(parameter W = 8, parameter A = 3) (
  input          wclk,
  input          rclk,
  input          rst_n,
  input          winc,
  input  [W-1:0] wdata,
  output reg     wfull,
  input          rinc,
  output reg [W-1:0] rdata,
  output reg     rempty
);
  wire wrst_n, rrst_n;
  rst_sync u_rw (.clk(wclk), .arst_n(rst_n), .rst_n(wrst_n));
  rst_sync u_rr (.clk(rclk), .arst_n(rst_n), .rst_n(rrst_n));

  reg [W-1:0] mem [0:(1<<A)-1];
  reg [A:0] wbin, wptr, rbin, rptr;
  wire [A:0] wq2_rptr, rq2_wptr;
  sync2 #(.W(A+1)) u_w2r (.clk(rclk), .rst_n(rrst_n), .d(wptr), .q(rq2_wptr));
  sync2 #(.W(A+1)) u_r2w (.clk(wclk), .rst_n(wrst_n), .d(rptr), .q(wq2_rptr));

  // write side
  wire [A:0] wbinnext = wbin + {{A{1'b0}}, winc & ~wfull};
  wire [A:0] wgraynext = (wbinnext >> 1) ^ wbinnext;
  wire wfull_next = wgraynext == {~wq2_rptr[A:A-1], wq2_rptr[A-2:0]};
  always @(posedge wclk or negedge wrst_n)
    if (!wrst_n) begin
      wbin <= 0;
      wptr <= 0;
      wfull <= 1'b0;
    end else begin
      wbin <= wbinnext;
      wptr <= wgraynext;
      wfull <= wfull_next;
    end
  always @(posedge wclk)
    if (winc && !wfull) mem[wbin[A-1:0]] <= wdata;

  // read side
  wire [A:0] rbinnext = rbin + {{A{1'b0}}, rinc & ~rempty};
  wire [A:0] rgraynext = (rbinnext >> 1) ^ rbinnext;
  always @(posedge rclk or negedge rrst_n)
    if (!rrst_n) begin
      rbin <= 0;
      rptr <= 0;
      rempty <= 1'b1;
    end else begin
      rbin <= rbinnext;
      rptr <= rgraynext;
      rempty <= rgraynext == rq2_wptr;
    end
  always @(posedge rclk or negedge rrst_n)
    if (!rrst_n) rdata <= {W{1'b0}};
    else if (rinc && !rempty) rdata <= mem[rbin[A-1:0]];
endmodule
