// SPDX-License-Identifier: CERN-OHL-W-2.0
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk
//
// Self-checking testbench for lt_capture (Icarus Verilog -g2012 or Verilator --binary).
// Prints "LT_CAPTURE_TB PASS" and finishes normally, or stops with $fatal on the first failure.
// Run with -Ptb.SYNC=2 to exercise the input synchronizer.

`timescale 1ns / 1ps
`default_nettype none

module tb;
    parameter integer SYNC = 0;

    localparam integer PW = 8;
    localparam integer AW = 6;   // 64 records: small enough to exercise wrap and overflow
    localparam integer DEPTH = 1 << AW;

    localparam [11:0] A_ID = 12'h000, A_VERSION = 12'h004, A_CTRL = 12'h008, A_STATUS = 12'h00C,
                      A_MASK = 12'h010, A_VALUE = 12'h014, A_RISE = 12'h018, A_FALL = 12'h01C,
                      A_POST_LO = 12'h020, A_POST_HI = 12'h024, A_WPTR = 12'h028, A_TPTR = 12'h02C,
                      A_TTS_LO = 12'h030, A_TTS_HI = 12'h034, A_ETS_LO = 12'h038, A_CLOCK = 12'h040,
                      A_CONFIG = 12'h044, A_RIDX = 12'h048, A_RTS_LO = 12'h04C, A_RTS_HI = 12'h050,
                      A_RPROBE = 12'h054, A_RECORDS = 12'h058, A_OLDEST = 12'h05C;
    localparam [31:0] ST_ARMED = 1, ST_TRIG = 2, ST_DONE = 4, ST_OVF = 8, ST_WRAP = 16, ST_ABORT = 32;

    reg clk = 1'b0;
    always #5 clk = ~clk;  // 100 MHz
    reg rst_n = 1'b0;

    reg  [PW-1:0] probe = {PW{1'b0}};
    reg           ext_trig = 1'b0;
    wire          trig_out;

    reg  [11:0] awaddr = 0, araddr = 0;
    reg  [31:0] wdata = 0;
    reg         awvalid = 0, wvalid = 0, bready = 0, arvalid = 0, rready = 0;
    wire        awready, wready, bvalid, arready, rvalid;
    wire [1:0]  bresp, rresp;
    wire [31:0] rdata;

    lt_capture #(.PROBE_WIDTH(PW), .ADDR_WIDTH(AW), .TS_WIDTH(48), .SYNC_STAGES(SYNC),
                 .CLOCK_HZ(32'd100_000_000)) dut (
        .clk(clk), .rst_n(rst_n), .probe(probe), .ext_trig(ext_trig), .trig_out(trig_out),
        .s_axi_awaddr(awaddr), .s_axi_awvalid(awvalid), .s_axi_awready(awready),
        .s_axi_wdata(wdata), .s_axi_wstrb(4'hF), .s_axi_wvalid(wvalid), .s_axi_wready(wready),
        .s_axi_bresp(bresp), .s_axi_bvalid(bvalid), .s_axi_bready(bready),
        .s_axi_araddr(araddr), .s_axi_arvalid(arvalid), .s_axi_arready(arready),
        .s_axi_rdata(rdata), .s_axi_rresp(rresp), .s_axi_rvalid(rvalid), .s_axi_rready(rready));

    integer errors = 0;
    integer trig_pulses = 0;
    always @(posedge clk) if (trig_out) trig_pulses = trig_pulses + 1;

    task check(input cond, input [8*96-1:0] what);
        begin
            if (!cond) begin
                $display("FAIL: %0s", what);
                $fatal(1, "lt_capture testbench failed");
            end
        end
    endtask

    // ---------------------------------------------------------------------------------------------
    // AXI4-Lite master

    task axi_write(input [11:0] a, input [31:0] d);
        begin
            @(posedge clk); #1;
            awaddr = a; wdata = d; awvalid = 1; wvalid = 1; bready = 1;
            @(posedge clk); #1;
            while (!(awready && wready)) begin @(posedge clk); #1; end
            awvalid = 0; wvalid = 0;
            while (!bvalid) begin @(posedge clk); #1; end
            check(bresp == 2'b00, "write response must be OKAY");
            @(posedge clk); #1;
            bready = 0;
        end
    endtask

    task axi_read(input [11:0] a, output [31:0] d);
        begin
            @(posedge clk); #1;
            araddr = a; arvalid = 1; rready = 1;
            @(posedge clk); #1;
            while (!arready) begin @(posedge clk); #1; end
            arvalid = 0;
            while (!rvalid) begin @(posedge clk); #1; end
            d = rdata;
            check(rresp == 2'b00, "read response must be OKAY");
            @(posedge clk); #1;
            rready = 0;
        end
    endtask

    // ---------------------------------------------------------------------------------------------
    // Record readout into arrays (in time order, oldest first)

    reg [47:0]   rec_ts    [0:DEPTH-1];
    reg [PW-1:0] rec_probe [0:DEPTH-1];
    integer      n_rec;
    integer      trig_rec;  // position of the trigger record in rec_* (time order)

    task read_records;
        reg [31:0] lo, hi, pr, count, oldest, tptr;
        integer i;
        begin
            axi_read(A_RECORDS, count);
            axi_read(A_OLDEST, oldest);
            axi_read(A_TPTR, tptr);
            n_rec = count;
            trig_rec = (tptr - oldest + DEPTH) % DEPTH;
            axi_write(A_RIDX, oldest);
            for (i = 0; i < n_rec; i = i + 1) begin
                if (i > 0 && ((oldest + i) % DEPTH) == 0)
                    axi_write(A_RIDX, 0);  // READ_INDEX wraps by itself; rewrite to stay explicit
                axi_read(A_RTS_LO, lo);
                axi_read(A_RTS_HI, hi);
                axi_read(A_RPROBE, pr);  // advances READ_INDEX
                rec_ts[i] = {hi[15:0], lo};
                rec_probe[i] = pr[PW-1:0];
            end
        end
    endtask

    task wait_done(input integer max_cycles);
        reg [31:0] st;
        integer n;
        begin
            n = 0;
            axi_read(A_STATUS, st);
            while (!(st & ST_DONE) && n < max_cycles) begin
                repeat (20) @(posedge clk);
                n = n + 20;
                axi_read(A_STATUS, st);
            end
            check((st & ST_DONE) != 0, "capture must finish");
        end
    endtask

    // ---------------------------------------------------------------------------------------------
    // Stimulus: bit0 STIM pulses; bit1 REACT = STIM delayed by exactly 2 clock cycles.

    reg stim = 1'b0;
    reg [1:0] stim_dly = 2'b00;
    reg react_en = 1'b1;
    always @(posedge clk) begin
        stim_dly <= {stim_dly[0], stim};
        probe[0] <= stim;
        probe[1] <= react_en ? stim_dly[1] : 1'b0;
    end

    task stim_burst(input integer pulses, input integer width, input integer period);
        integer k;
        begin
            for (k = 0; k < pulses; k = k + 1) begin
                @(posedge clk); #1 stim = 1'b1;
                repeat (width) @(posedge clk);
                #1 stim = 1'b0;
                repeat (period - width - 1) @(posedge clk);
            end
        end
    endtask

    // ---------------------------------------------------------------------------------------------

    reg [31:0] v, st;
    integer i, j, events, stim_ups;
    reg [47:0] tts;

    initial begin
        repeat (4) @(posedge clk);
        rst_n = 1'b1;
        repeat (2) @(posedge clk);

        // T1: identification and bus behaviour --------------------------------------------------
        axi_read(A_ID, v);       check(v == 32'h4C545243, "ID must read 'LTRC'");
        axi_read(A_VERSION, v);  check(v == 32'h00010000, "VERSION 1.0");
        axi_read(A_CONFIG, v);   check(v == {8'd48, SYNC[7:0], AW[7:0], PW[7:0]}, "CONFIG fields");
        axi_read(A_CLOCK, v);    check(v == 32'd100_000_000, "CLOCK_HZ");
        axi_read(12'hFF0, v);    check(v == 0, "unknown address reads 0");
        axi_write(A_ID, 32'hDEADBEEF);
        axi_read(A_ID, v);       check(v == 32'h4C545243, "read-only register ignores writes");
        axi_read(A_STATUS, v);   check(v == 0, "idle after reset");

        // T2: rising-edge trigger on STIM, exact 2-cycle STIM->REACT latency --------------------
        axi_write(A_MASK, 32'h1);
        axi_write(A_VALUE, 32'h1);
        axi_write(A_RISE, 32'h1);
        axi_write(A_FALL, 32'h0);
        axi_write(A_POST_LO, 32'd800);
        axi_write(A_POST_HI, 32'd0);
        axi_write(A_CTRL, 32'h1);  // arm
        repeat (30) @(posedge clk);
        axi_read(A_STATUS, st);
        check((st & ST_ARMED) && !(st & ST_TRIG), "armed and waiting for the trigger");
        stim_burst(10, 10, 50);
        wait_done(5000);
        axi_read(A_STATUS, st);
        check((st & ST_TRIG) && !(st & ST_OVF) && !(st & ST_WRAP), "T2 status: triggered, no overflow/wrap");
        check(trig_pulses == 1, "trig_out pulses once");
        read_records;
        check(n_rec == 1 + 40 + 1, "T2: initial + 4 changes x 10 pulses + end record");
        check(rec_probe[trig_rec][0] && !rec_probe[trig_rec - 1][0], "trigger record holds the STIM rising edge");
        axi_read(A_TTS_LO, v);
        check(rec_ts[trig_rec] == {16'd0, v}, "TRIG_TS matches the trigger record");
        events = 0;
        stim_ups = 0;
        for (i = 1; i < n_rec; i = i + 1) begin
            if (rec_probe[i][0] && !rec_probe[i-1][0]) begin
                stim_ups = stim_ups + 1;
                for (j = i + 1; j < n_rec; j = j + 1) begin
                    if (rec_probe[j][1] && !rec_probe[j-1][1]) begin
                        check(rec_ts[j] - rec_ts[i] == 2, "STIM->REACT must be exactly 2 cycles");
                        events = events + 1;
                        j = n_rec;
                    end
                end
            end
        end
        check(stim_ups == 10 && events == 10, "T2: 10 paired events");
        for (i = 1; i < n_rec; i = i + 1)
            check(rec_ts[i] > rec_ts[i-1], "timestamps strictly increase");
        axi_read(A_ETS_LO, v);
        check(rec_ts[n_rec-1] == {16'd0, v}, "last record is the end marker at END_TS");
        check(v - rec_ts[trig_rec] >= 800, "post-trigger time honoured");

        // T3: pattern (STIM low) + REACT falling edge ------------------------------------------
        axi_write(A_MASK, 32'h1);
        axi_write(A_VALUE, 32'h0);
        axi_write(A_RISE, 32'h0);
        axi_write(A_FALL, 32'h2);
        axi_write(A_POST_LO, 32'd100);
        axi_write(A_CTRL, 32'h1);
        repeat (10) @(posedge clk);
        stim_burst(3, 10, 50);
        wait_done(2000);
        read_records;
        check(!rec_probe[trig_rec][1] && rec_probe[trig_rec - 1][1] && !rec_probe[trig_rec][0],
              "T3: trigger on REACT falling while STIM is low");

        // T4: ring wrap keeps the newest pre-trigger history; force trigger ----------------------
        axi_write(A_MASK, 32'h0);
        axi_write(A_RISE, 32'h80);  // bit7 never toggles: only the force command triggers
        axi_write(A_FALL, 32'h0);
        axi_write(A_POST_LO, 32'd5);
        axi_write(A_CTRL, 32'h1);
        for (i = 0; i < 200; i = i + 1) begin
            @(posedge clk); #1 probe[2] = ~probe[2];
        end
        axi_write(A_CTRL, 32'h4);  // force
        wait_done(500);
        axi_read(A_STATUS, st);
        check((st & ST_WRAP) && !(st & ST_OVF), "T4: wrapped, no overflow");
        read_records;
        check(n_rec == DEPTH, "T4: ring full");
        for (i = 1; i < n_rec; i = i + 1)
            check(rec_ts[i] > rec_ts[i-1], "T4: ring read in time order");
        check(trig_rec > DEPTH / 2, "T4: most of the ring is pre-trigger history");

        // T5: immediate trigger, buffer fills before POST_CYCLES -> overflow ----------------------
        axi_write(A_RISE, 32'h0);
        axi_write(A_POST_LO, 32'd100000);
        axi_write(A_CTRL, 32'h1);
        for (i = 0; i < 200; i = i + 1) begin
            @(posedge clk); #1 probe[2] = ~probe[2];
        end
        wait_done(500);
        axi_read(A_STATUS, st);
        check((st & ST_OVF) && (st & ST_TRIG), "T5: overflow reported");
        read_records;
        check(n_rec == DEPTH && trig_rec == 0 && rec_ts[0] == 0, "T5: trigger record kept at the start");

        // T6: abort ------------------------------------------------------------------------------
        axi_write(A_MASK, 32'h80);
        axi_write(A_VALUE, 32'h80);  // bit7 never goes high
        axi_write(A_CTRL, 32'h1);
        repeat (20) @(posedge clk);
        axi_write(A_CTRL, 32'h2);
        axi_read(A_STATUS, st);
        check((st & ST_ABORT) && !(st & ST_ARMED) && !(st & ST_TRIG), "T6: aborted");

        // T7: external trigger mode --------------------------------------------------------------
        trig_pulses = 0;
        axi_write(A_POST_LO, 32'd10);
        axi_write(A_CTRL, 32'h9);  // arm + external-trigger mode
        repeat (15) @(posedge clk);
        #1 ext_trig = 1'b1;
        @(posedge clk); #1 ext_trig = 1'b0;
        wait_done(200);
        axi_read(A_STATUS, st);
        check((st & ST_TRIG) != 0 && trig_pulses == 1, "T7: external trigger");
        axi_read(A_TTS_LO, v);
        check(v >= 15, "T7: trigger after the external pulse");

        $display("LT_CAPTURE_TB PASS (SYNC_STAGES=%0d)", SYNC);
        $finish;
    end

    initial begin
        #2_000_000;
        $fatal(1, "timeout");
    end
endmodule

`default_nettype wire
