// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk
//
// LogicTracer test-signal generator. A list of segments {pin levels, duration in system clock cycles} is
// streamed by a looping DMA pair into the TEST_SIGNAL PIO program, which holds each level for exactly the
// requested number of cycles without CPU involvement, so the self-test knows the true timing of its stimulus.

#include "LogicAnalyzer_Board_Settings.h"
#include "LogicAnalyzer_TestSignal.h"

#include <stdio.h>
#include "pico/stdlib.h"

#ifdef SUPPORTS_TEST_SIGNAL

#include "hardware/clocks.h"
#include "hardware/dma.h"
#include "hardware/pio.h"
#include "LogicAnalyzer.pio.h"

#define TS_MAX_PULSES 256
#define TS_MIN_SEGMENT 3                 //out + out + one jmp
#define TS_MAX_SEGMENT ((1u << 30) + 2)  //30-bit loop counter

static uint32_t tsWords[TS_MAX_PULSES * 4];
static uint32_t* tsWordsAddr = tsWords;  //read by the control DMA channel to restart the data channel
static PIO tsPio = pio1;
static int tsSm = -1;
static int tsDataDma = -1;
static int tsCtrlDma = -1;
static uint tsOffset = 0;
static bool tsRunning = false;

static uint32_t cyclesOf(uint32_t ns, uint32_t sysclk)
{
    return (uint32_t)(((uint64_t)ns * sysclk + 500000000ull) / 1000000000ull);
}

static bool segmentOk(uint64_t cycles)
{
    return cycles >= TS_MIN_SEGMENT && cycles <= TS_MAX_SEGMENT;
}

bool TestSignalSupported(void)
{
    return true;
}

bool TestSignalRunning(void)
{
    return tsRunning;
}

void StopTestSignal(void)
{
    if(!tsRunning)
        return;

    pio_sm_set_enabled(tsPio, tsSm, false);
    //Break the loop first (the control channel would restart the data channel), then abort both
    hw_clear_bits(&dma_hw->ch[tsDataDma].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    hw_clear_bits(&dma_hw->ch[tsCtrlDma].al1_ctrl, DMA_CH0_CTRL_TRIG_EN_BITS);
    dma_channel_abort(tsCtrlDma);
    dma_channel_abort(tsDataDma);
    dma_channel_unclaim(tsDataDma);
    dma_channel_unclaim(tsCtrlDma);
    pio_sm_clear_fifos(tsPio, tsSm);
    pio_remove_program(tsPio, &TEST_SIGNAL_program, tsOffset);
    pio_sm_unclaim(tsPio, tsSm);
    gpio_init(TEST_SIGNAL_PIN_BASE);      //back to SIO inputs
    gpio_init(TEST_SIGNAL_PIN_BASE + 1);
    tsSm = tsDataDma = tsCtrlDma = -1;
    tsRunning = false;
}

bool ProcessTestSignalRequest(const TEST_SIGNAL_REQUEST* req, char* reply, size_t replySize)
{
    StopTestSignal();
    if(!req->enable)
    {
        snprintf(reply, replySize, "TESTSIGNAL:OFF\n");
        return true;
    }

    const uint32_t sysclk = clock_get_hz(clk_sys);
    const uint64_t high = cyclesOf(req->highNs, sysclk);
    const uint64_t delay = cyclesOf(req->delayNs, sysclk);
    const uint64_t period = cyclesOf(req->periodNs, sysclk);
    const uint64_t gap = cyclesOf(req->gapNs, sysclk);
    const uint32_t pulses = req->pulses;

    //Segments of one pulse: delay == 0 -> {A+B high, rest}; else {A, A+B, B, none}
    bool ok = pulses >= 1 && pulses <= TS_MAX_PULSES && period > high + delay;
    if(ok && delay == 0)
        ok = segmentOk(high) && segmentOk(period - high) && segmentOk(period - high + gap);
    else if(ok)
        ok = high > delay && segmentOk(delay) && segmentOk(high - delay) && segmentOk(period - high - delay) &&
             segmentOk(period - high - delay + gap);
    if(!ok)
    {
        snprintf(reply, replySize, "ERR_TEST_SIGNAL\n");
        return false;
    }

    uint32_t n = 0;
    for(uint32_t k = 0; k < pulses; k++)
    {
        const uint64_t extra = (k == pulses - 1) ? gap : 0;
        if(delay == 0)
        {
            tsWords[n++] = 3u | (uint32_t)((high - TS_MIN_SEGMENT) << 2);
            tsWords[n++] = 0u | (uint32_t)((period - high + extra - TS_MIN_SEGMENT) << 2);
        }
        else
        {
            tsWords[n++] = 1u | (uint32_t)((delay - TS_MIN_SEGMENT) << 2);
            tsWords[n++] = 3u | (uint32_t)((high - delay - TS_MIN_SEGMENT) << 2);
            tsWords[n++] = 2u | (uint32_t)((delay - TS_MIN_SEGMENT) << 2);
            tsWords[n++] = 0u | (uint32_t)((period - high - delay + extra - TS_MIN_SEGMENT) << 2);
        }
    }

    if(!pio_can_add_program(tsPio, &TEST_SIGNAL_program))
    {
        snprintf(reply, replySize, "ERR_TEST_SIGNAL\n");
        return false;
    }
    tsSm = pio_claim_unused_sm(tsPio, false);
    tsDataDma = dma_claim_unused_channel(false);
    tsCtrlDma = dma_claim_unused_channel(false);
    if(tsSm < 0 || tsDataDma < 0 || tsCtrlDma < 0)
    {
        if(tsSm >= 0) pio_sm_unclaim(tsPio, tsSm);
        if(tsDataDma >= 0) dma_channel_unclaim(tsDataDma);
        if(tsCtrlDma >= 0) dma_channel_unclaim(tsCtrlDma);
        tsSm = tsDataDma = tsCtrlDma = -1;
        snprintf(reply, replySize, "ERR_TEST_SIGNAL\n");
        return false;
    }
    tsOffset = pio_add_program(tsPio, &TEST_SIGNAL_program);

    pio_sm_config c = TEST_SIGNAL_program_get_default_config(tsOffset);
    sm_config_set_out_pins(&c, TEST_SIGNAL_PIN_BASE, 2);
    sm_config_set_out_shift(&c, true, true, 32);   //LSB first: 2 pin bits, then the 30-bit count; autopull
    sm_config_set_fifo_join(&c, PIO_FIFO_JOIN_TX);  //8 words of slack for the DMA
    sm_config_set_clkdiv(&c, 1.0f);
    pio_gpio_init(tsPio, TEST_SIGNAL_PIN_BASE);
    pio_gpio_init(tsPio, TEST_SIGNAL_PIN_BASE + 1);
    pio_sm_set_consecutive_pindirs(tsPio, tsSm, TEST_SIGNAL_PIN_BASE, 2, true);
    pio_sm_init(tsPio, tsSm, tsOffset, &c);

    dma_channel_config dc = dma_channel_get_default_config(tsDataDma);
    channel_config_set_transfer_data_size(&dc, DMA_SIZE_32);
    channel_config_set_read_increment(&dc, true);
    channel_config_set_write_increment(&dc, false);
    channel_config_set_dreq(&dc, pio_get_dreq(tsPio, tsSm, true));
    channel_config_set_chain_to(&dc, tsCtrlDma);
    dma_channel_configure(tsDataDma, &dc, &tsPio->txf[tsSm], tsWords, n, false);

    dma_channel_config cc = dma_channel_get_default_config(tsCtrlDma);
    channel_config_set_transfer_data_size(&cc, DMA_SIZE_32);
    channel_config_set_read_increment(&cc, false);
    channel_config_set_write_increment(&cc, false);
    dma_channel_configure(tsCtrlDma, &cc, &dma_hw->ch[tsDataDma].al3_read_addr_trig, &tsWordsAddr, 1, false);

    dma_channel_start(tsDataDma);
    pio_sm_set_enabled(tsPio, tsSm, true);
    tsRunning = true;

    snprintf(reply, replySize, "TESTSIGNAL:%lu,%lu,%lu,%lu,%lu,%lu\n", (unsigned long)sysclk, (unsigned long)high,
             (unsigned long)delay, (unsigned long)period, (unsigned long)gap, (unsigned long)pulses);
    return true;
}

#else

bool TestSignalSupported(void)
{
    return false;
}

bool TestSignalRunning(void)
{
    return false;
}

void StopTestSignal(void)
{
}

bool ProcessTestSignalRequest(const TEST_SIGNAL_REQUEST* req, char* reply, size_t replySize)
{
    (void)req;
    snprintf(reply, replySize, "ERR_UNSUPPORTED\n");
    return false;
}

#endif
