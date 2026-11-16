// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk
//
// lt_capture: in-design logic capture core for LogicTracer.
//
// Records `probe` in the clock domain of the design under test, so delays between probe bits are
// exact integer clock cycles. Storage is edge-encoded: a record {timestamp, probe} is written only
// when the probe vector changes (plus one record at arm time, at the trigger and at the end), so
// buffer depth counts events, not samples. Before the trigger the buffer is a ring (pre-trigger
// history); after the trigger capture continues for POST_CYCLES clock cycles or until the ring
// would overwrite the trigger record (overflow).
//
// Register interface: AXI4-Lite, same clock as `probe`. Every write is answered OKAY and every
// read OKAY (unknown addresses read 0), so no bus error can reach the processor. Register map:
// hdl/README.md.
//
// Parameters:
//   PROBE_WIDTH  1..32 probe bits
//   ADDR_WIDTH   log2(records); 13 = 8192 records
//   TS_WIDTH     33..63 timestamp bits (48 = 32 days at 100 MHz)
//   SYNC_STAGES  0 for probes already in `clk` domain; 2 for asynchronous inputs (pins). The
//                synchronizer adds a fixed latency to every bit and +-1 cycle of uncertainty.
//   CLOCK_HZ     frequency of `clk`, reported to software

`default_nettype none

module lt_capture #(
    parameter integer PROBE_WIDTH = 32,
    parameter integer ADDR_WIDTH  = 13,
    parameter integer TS_WIDTH    = 48,
    parameter integer SYNC_STAGES = 0,
    parameter [31:0]  CLOCK_HZ    = 32'd100_000_000
) (
    input  wire                   clk,
    input  wire                   rst_n,

    input  wire [PROBE_WIDTH-1:0] probe,
    input  wire                   ext_trig,   // external trigger (level, sampled like the probes)
    output reg                    trig_out,   // one-cycle pulse when the capture triggers

    input  wire [11:0]            s_axi_awaddr,
    input  wire                   s_axi_awvalid,
    output reg                    s_axi_awready,
    input  wire [31:0]            s_axi_wdata,
    input  wire [3:0]             s_axi_wstrb,
    input  wire                   s_axi_wvalid,
    output reg                    s_axi_wready,
    output wire [1:0]             s_axi_bresp,
    output reg                    s_axi_bvalid,
    input  wire                   s_axi_bready,
    input  wire [11:0]            s_axi_araddr,
    input  wire                   s_axi_arvalid,
    output reg                    s_axi_arready,
    output reg  [31:0]            s_axi_rdata,
    output wire [1:0]             s_axi_rresp,
    output reg                    s_axi_rvalid,
    input  wire                   s_axi_rready
);

    // ---------------------------------------------------------------------------------------------
    // Constants and register map

    localparam integer DEPTH = 1 << ADDR_WIDTH;
    localparam integer REC_W = TS_WIDTH + PROBE_WIDTH;
    localparam [ADDR_WIDTH:0] DEPTH_V      = {1'b1, {ADDR_WIDTH{1'b0}}};
    localparam [ADDR_WIDTH:0] LAST_SLOT_V  = {1'b0, {ADDR_WIDTH{1'b1}}};
    localparam [31:0]         CONFIG_VALUE = (TS_WIDTH << 24) | (SYNC_STAGES << 16) | (ADDR_WIDTH << 8) | PROBE_WIDTH;

    localparam [31:0] ID_VALUE      = 32'h4C54_5243;  // "LTRC"
    localparam [31:0] VERSION_VALUE = 32'h0001_0000;  // 1.0

    localparam [11:0] A_ID          = 12'h000;
    localparam [11:0] A_VERSION     = 12'h004;
    localparam [11:0] A_CTRL        = 12'h008;
    localparam [11:0] A_STATUS      = 12'h00C;
    localparam [11:0] A_TRIG_MASK   = 12'h010;
    localparam [11:0] A_TRIG_VALUE  = 12'h014;
    localparam [11:0] A_TRIG_RISE   = 12'h018;
    localparam [11:0] A_TRIG_FALL   = 12'h01C;
    localparam [11:0] A_POST_LO     = 12'h020;
    localparam [11:0] A_POST_HI     = 12'h024;
    localparam [11:0] A_WRITE_PTR   = 12'h028;
    localparam [11:0] A_TRIG_PTR    = 12'h02C;
    localparam [11:0] A_TRIG_TS_LO  = 12'h030;
    localparam [11:0] A_TRIG_TS_HI  = 12'h034;
    localparam [11:0] A_END_TS_LO   = 12'h038;
    localparam [11:0] A_END_TS_HI   = 12'h03C;
    localparam [11:0] A_CLOCK_HZ    = 12'h040;
    localparam [11:0] A_CONFIG      = 12'h044;
    localparam [11:0] A_READ_INDEX  = 12'h048;
    localparam [11:0] A_READ_TS_LO  = 12'h04C;
    localparam [11:0] A_READ_TS_HI  = 12'h050;
    localparam [11:0] A_READ_PROBE  = 12'h054;  // reading it advances READ_INDEX
    localparam [11:0] A_RECORDS     = 12'h058;
    localparam [11:0] A_OLDEST      = 12'h05C;

    // CTRL bits (write): 0 arm, 1 abort, 2 force trigger (pulses); 3 external-trigger mode (stored).
    // STATUS bits: 0 armed, 1 triggered, 2 done, 3 overflow, 4 wrapped, 5 aborted.

    // ---------------------------------------------------------------------------------------------
    // Parameter checks (elaboration fails on an unsupported configuration)

    generate
        if (PROBE_WIDTH < 1 || PROBE_WIDTH > 32) begin : g_bad_probe_width
            lt_capture_unsupported_PROBE_WIDTH bad();
        end
        if (TS_WIDTH < 33 || TS_WIDTH > 63) begin : g_bad_ts_width
            lt_capture_unsupported_TS_WIDTH bad();
        end
        if (ADDR_WIDTH < 2 || ADDR_WIDTH > 20) begin : g_bad_addr_width
            lt_capture_unsupported_ADDR_WIDTH bad();
        end
    endgenerate

    // ---------------------------------------------------------------------------------------------
    // Optional input synchronizer and probe register

    wire [PROBE_WIDTH:0] raw_in = {ext_trig, probe};
    wire [PROBE_WIDTH:0] sync_in;

    generate
        if (SYNC_STAGES == 0) begin : g_nosync
            assign sync_in = raw_in;
        end else begin : g_sync
            (* ASYNC_REG = "TRUE" *) reg [PROBE_WIDTH:0] sync_q [0:SYNC_STAGES-1];
            integer s;
            always @(posedge clk) begin
                sync_q[0] <= raw_in;
                for (s = 1; s < SYNC_STAGES; s = s + 1)
                    sync_q[s] <= sync_q[s-1];
            end
            assign sync_in = sync_q[SYNC_STAGES-1];
        end
    endgenerate

    reg [PROBE_WIDTH-1:0] p_q;     // current probe value
    reg [PROBE_WIDTH-1:0] prev_q;  // value one cycle earlier
    reg                   ext_q;

    always @(posedge clk) begin
        p_q    <= sync_in[PROBE_WIDTH-1:0];
        ext_q  <= sync_in[PROBE_WIDTH];
        prev_q <= p_q;
    end

    // ---------------------------------------------------------------------------------------------
    // Configuration registers (written over AXI4-Lite)

    reg [PROBE_WIDTH-1:0] trig_mask, trig_value, trig_rise, trig_fall;
    reg [TS_WIDTH-1:0]    post_cycles;
    reg                   ext_mode;
    reg [ADDR_WIDTH-1:0]  read_index;

    // Command pulses from the register interface
    reg cmd_arm, cmd_abort, cmd_force;

    // ---------------------------------------------------------------------------------------------
    // Capture engine

    reg                  armed, triggered, done, overflow, wrapped, aborted, first;
    reg [TS_WIDTH-1:0]   ts, trig_ts, end_ts;
    reg [ADDR_WIDTH-1:0] wr_ptr, trig_ptr;
    reg [ADDR_WIDTH:0]   post_count;  // records written since (and including) the trigger record

    wire [PROBE_WIDTH-1:0] rise = p_q & ~prev_q;
    wire [PROBE_WIDTH-1:0] fall = ~p_q & prev_q;
    wire edge_cfg  = |(trig_rise | trig_fall);
    wire edge_hit  = |((rise & trig_rise) | (fall & trig_fall)) && !first;
    wire level_hit = ((p_q ^ trig_value) & trig_mask) == {PROBE_WIDTH{1'b0}};
    wire pattern   = level_hit && (edge_cfg ? edge_hit : 1'b1);
    wire running   = armed && !done;
    wire fire      = running && !triggered && (cmd_force || (ext_mode ? ext_q : pattern));
    wire elapsed   = triggered && ((ts - trig_ts) >= post_cycles);
    wire stop      = running && elapsed;
    wire changed   = first || (p_q != prev_q);
    // An abort or re-arm in the same cycle must not write: after a ring wrap the slot at wr_ptr still holds
    // the oldest pre-trigger record, and neither command advances wr_ptr.
    wire do_write  = running && !cmd_abort && !cmd_arm && (changed || fire || stop);
    // The ring holds the trigger record plus DEPTH-1 later records; the write that fills the last
    // free slot ends the capture (overflow unless the post-trigger time also elapsed).
    wire post_full = triggered && do_write && (post_count == LAST_SLOT_V);

    (* ram_style = "block" *) reg [REC_W-1:0] mem [0:DEPTH-1];
    reg [REC_W-1:0] rd_q;

    always @(posedge clk) begin
        if (do_write)
            mem[wr_ptr] <= {ts, p_q};
        rd_q <= mem[read_index];
    end

    always @(posedge clk) begin
        if (!rst_n) begin
            armed      <= 1'b0;
            triggered  <= 1'b0;
            done       <= 1'b0;
            overflow   <= 1'b0;
            wrapped    <= 1'b0;
            aborted    <= 1'b0;
            first      <= 1'b0;
            ts         <= {TS_WIDTH{1'b0}};
            trig_ts    <= {TS_WIDTH{1'b0}};
            end_ts     <= {TS_WIDTH{1'b0}};
            wr_ptr     <= {ADDR_WIDTH{1'b0}};
            trig_ptr   <= {ADDR_WIDTH{1'b0}};
            post_count <= {(ADDR_WIDTH+1){1'b0}};
            trig_out   <= 1'b0;
        end else begin
            trig_out <= 1'b0;
            if (cmd_arm) begin
                armed      <= 1'b1;
                triggered  <= 1'b0;
                done       <= 1'b0;
                overflow   <= 1'b0;
                wrapped    <= 1'b0;
                aborted    <= 1'b0;
                first      <= 1'b1;
                ts         <= {TS_WIDTH{1'b0}};
                wr_ptr     <= {ADDR_WIDTH{1'b0}};
                post_count <= {(ADDR_WIDTH+1){1'b0}};
            end else if (cmd_abort && running) begin
                armed   <= 1'b0;
                aborted <= 1'b1;
                end_ts  <= ts;
            end else if (running) begin
                ts    <= ts + 1'b1;
                first <= 1'b0;
                if (do_write) begin
                    wr_ptr <= wr_ptr + 1'b1;
                    if (&wr_ptr)
                        wrapped <= 1'b1;
                    if (triggered)
                        post_count <= post_count + 1'b1;
                end
                if (fire) begin
                    triggered  <= 1'b1;
                    trig_ptr   <= wr_ptr;
                    trig_ts    <= ts;
                    post_count <= {{ADDR_WIDTH{1'b0}}, 1'b1};
                    trig_out   <= 1'b1;
                end
                if (stop || post_full) begin
                    done     <= 1'b1;
                    armed    <= 1'b0;
                    overflow <= post_full && !stop;
                    end_ts   <= ts;
                end
            end
        end
    end

    // ---------------------------------------------------------------------------------------------
    // AXI4-Lite slave (single outstanding transaction per channel)

    assign s_axi_bresp = 2'b00;
    assign s_axi_rresp = 2'b00;

    // 64-bit views of the timestamps and the selected record, zero-extended.
    reg [63:0] trig_ts64, end_ts64, rd_ts64, post64;
    reg [31:0] rd_probe32, cfg_mask32, cfg_value32, cfg_rise32, cfg_fall32;
    always @(*) begin
        trig_ts64 = 64'd0;  trig_ts64[TS_WIDTH-1:0] = trig_ts;
        end_ts64  = 64'd0;  end_ts64[TS_WIDTH-1:0]  = end_ts;
        post64    = 64'd0;  post64[TS_WIDTH-1:0]    = post_cycles;
        rd_ts64   = 64'd0;  rd_ts64[TS_WIDTH-1:0]   = rd_q[REC_W-1:PROBE_WIDTH];
        rd_probe32  = 32'd0; rd_probe32[PROBE_WIDTH-1:0]  = rd_q[PROBE_WIDTH-1:0];
        cfg_mask32  = 32'd0; cfg_mask32[PROBE_WIDTH-1:0]  = trig_mask;
        cfg_value32 = 32'd0; cfg_value32[PROBE_WIDTH-1:0] = trig_value;
        cfg_rise32  = 32'd0; cfg_rise32[PROBE_WIDTH-1:0]  = trig_rise;
        cfg_fall32  = 32'd0; cfg_fall32[PROBE_WIDTH-1:0]  = trig_fall;
    end

    wire [ADDR_WIDTH:0] records = wrapped ? DEPTH_V : {1'b0, wr_ptr};
    wire [ADDR_WIDTH-1:0] oldest = wrapped ? wr_ptr : {ADDR_WIDTH{1'b0}};

    reg        rd_pending;
    reg [11:0] rd_addr;
    reg        rd_advance;  // READ_PROBE was read: select the next record
    reg        idx_chg;     // READ_INDEX was assigned at the last edge: the BRAM output is one cycle behind

    // Write channel: address and data are accepted together.
    always @(posedge clk) begin
        if (!rst_n) begin
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            s_axi_bvalid  <= 1'b0;
            cmd_arm       <= 1'b0;
            cmd_abort     <= 1'b0;
            cmd_force     <= 1'b0;
            ext_mode      <= 1'b0;
            trig_mask     <= {PROBE_WIDTH{1'b0}};
            trig_value    <= {PROBE_WIDTH{1'b0}};
            trig_rise     <= {PROBE_WIDTH{1'b0}};
            trig_fall     <= {PROBE_WIDTH{1'b0}};
            post_cycles   <= {TS_WIDTH{1'b0}};
            read_index    <= {ADDR_WIDTH{1'b0}};
            idx_chg       <= 1'b0;
        end else begin
            idx_chg       <= 1'b0;
            // The auto-increment is independent of bus writes; a write to READ_INDEX below wins over it.
            if (rd_advance) begin
                read_index <= read_index + 1'b1;
                idx_chg    <= 1'b1;
            end
            cmd_arm       <= 1'b0;
            cmd_abort     <= 1'b0;
            cmd_force     <= 1'b0;
            s_axi_awready <= 1'b0;
            s_axi_wready  <= 1'b0;
            if (s_axi_bvalid && s_axi_bready)
                s_axi_bvalid <= 1'b0;
            if (s_axi_awvalid && s_axi_wvalid && !s_axi_awready && !s_axi_bvalid) begin
                s_axi_awready <= 1'b1;
                s_axi_wready  <= 1'b1;
                s_axi_bvalid  <= 1'b1;
                case (s_axi_awaddr & 12'hFFC)
                    A_CTRL: begin
                        cmd_arm   <= s_axi_wdata[0];
                        cmd_abort <= s_axi_wdata[1];
                        cmd_force <= s_axi_wdata[2];
                        ext_mode  <= s_axi_wdata[3];
                    end
                    A_TRIG_MASK:  trig_mask  <= s_axi_wdata[PROBE_WIDTH-1:0];
                    A_TRIG_VALUE: trig_value <= s_axi_wdata[PROBE_WIDTH-1:0];
                    A_TRIG_RISE:  trig_rise  <= s_axi_wdata[PROBE_WIDTH-1:0];
                    A_TRIG_FALL:  trig_fall  <= s_axi_wdata[PROBE_WIDTH-1:0];
                    A_POST_LO:    post_cycles[31:0] <= s_axi_wdata;
                    A_POST_HI:    post_cycles[TS_WIDTH-1:32] <= s_axi_wdata[TS_WIDTH-33:0];
                    A_READ_INDEX: begin
                        read_index <= s_axi_wdata[ADDR_WIDTH-1:0];
                        idx_chg    <= 1'b1;
                    end
                    default: ;  // read-only or unknown: accepted and ignored
                endcase
            end
        end
    end

    // Read channel: the address is latched, the data returned one cycle later (so a record selected
    // by READ_INDEX has been read from the block RAM).

    always @(posedge clk) begin
        if (!rst_n) begin
            s_axi_arready <= 1'b0;
            s_axi_rvalid  <= 1'b0;
            s_axi_rdata   <= 32'd0;
            rd_pending    <= 1'b0;
            rd_addr       <= 12'd0;
            rd_advance    <= 1'b0;
        end else begin
            s_axi_arready <= 1'b0;
            rd_advance    <= 1'b0;
            if (s_axi_rvalid && s_axi_rready)
                s_axi_rvalid <= 1'b0;
            if (s_axi_arvalid && !s_axi_arready && !rd_pending && !s_axi_rvalid) begin
                s_axi_arready <= 1'b1;
                rd_addr       <= s_axi_araddr & 12'hFFC;
                rd_pending    <= 1'b1;
            end else if (rd_pending && !idx_chg) begin
                // (idx_chg: wait one cycle until the record selected by a just-written READ_INDEX is in rd_q)
                rd_pending   <= 1'b0;
                s_axi_rvalid <= 1'b1;
                case (rd_addr)
                    A_ID:         s_axi_rdata <= ID_VALUE;
                    A_VERSION:    s_axi_rdata <= VERSION_VALUE;
                    A_CTRL:       s_axi_rdata <= {28'd0, ext_mode, 3'd0};
                    A_STATUS:     s_axi_rdata <= {26'd0, aborted, wrapped, overflow, done, triggered, armed};
                    A_TRIG_MASK:  s_axi_rdata <= cfg_mask32;
                    A_TRIG_VALUE: s_axi_rdata <= cfg_value32;
                    A_TRIG_RISE:  s_axi_rdata <= cfg_rise32;
                    A_TRIG_FALL:  s_axi_rdata <= cfg_fall32;
                    A_POST_LO:    s_axi_rdata <= post64[31:0];
                    A_POST_HI:    s_axi_rdata <= post64[63:32];
                    A_WRITE_PTR:  s_axi_rdata <= {{(31-ADDR_WIDTH){1'b0}}, wrapped, wr_ptr};
                    A_TRIG_PTR:   s_axi_rdata <= {{(32-ADDR_WIDTH){1'b0}}, trig_ptr};
                    A_TRIG_TS_LO: s_axi_rdata <= trig_ts64[31:0];
                    A_TRIG_TS_HI: s_axi_rdata <= trig_ts64[63:32];
                    A_END_TS_LO:  s_axi_rdata <= end_ts64[31:0];
                    A_END_TS_HI:  s_axi_rdata <= end_ts64[63:32];
                    A_CLOCK_HZ:   s_axi_rdata <= CLOCK_HZ;
                    A_CONFIG:     s_axi_rdata <= CONFIG_VALUE;
                    A_READ_INDEX: s_axi_rdata <= {{(32-ADDR_WIDTH){1'b0}}, read_index};
                    A_READ_TS_LO: s_axi_rdata <= rd_ts64[31:0];
                    A_READ_TS_HI: s_axi_rdata <= rd_ts64[63:32];
                    A_READ_PROBE: begin
                        s_axi_rdata <= rd_probe32;
                        rd_advance  <= 1'b1;
                    end
                    A_RECORDS:    s_axi_rdata <= {{(31-ADDR_WIDTH){1'b0}}, records};
                    A_OLDEST:     s_axi_rdata <= {{(32-ADDR_WIDTH){1'b0}}, oldest};
                    default:      s_axi_rdata <= 32'd0;
                endcase
            end
        end
    end

    // s_axi_wstrb is ignored: registers are written as whole 32-bit words.
    wire unused_ok = &{1'b0, s_axi_wstrb};

endmodule

`default_nettype wire
