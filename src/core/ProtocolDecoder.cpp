// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/ProtocolDecoder.hpp"

#include "core/ArgParsing.hpp"
#include "core/Format.hpp"
#include "core/Segments.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <iomanip>
#include <sstream>

namespace lt {

namespace {

std::string trim(std::string_view sv)
{
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.front())))
        sv.remove_prefix(1);
    while (!sv.empty() && std::isspace(static_cast<unsigned char>(sv.back())))
        sv.remove_suffix(1);
    return std::string(sv);
}

std::string lower(std::string_view sv)
{
    std::string out(sv);
    for (char& c : out)
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

std::vector<std::string> splitTokens(std::string_view sv, char sep1, char sep2 = '\0')
{
    std::vector<std::string> out;
    std::size_t start = 0;
    for (std::size_t i = 0; i <= sv.size(); ++i) {
        if (i == sv.size() || sv[i] == sep1 || (sep2 != '\0' && sv[i] == sep2)) {
            if (auto t = trim(sv.substr(start, i - start)); !t.empty())
                out.push_back(std::move(t));
            start = i + 1;
        }
    }
    return out;
}

std::optional<std::size_t> findChannelIndex(const std::vector<ChannelInfo>& channels, std::string_view token)
{
    const std::string t = trim(token);
    if (t.empty())
        return std::nullopt;
    const std::string lo = lower(t);
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (!channels[i].name.empty() && lower(channels[i].name) == lo)
            return i;
    }
    std::string_view numPart = t;
    if (lo.rfind("ch", 0) == 0 && t.size() > 2)
        numPart = std::string_view(t).substr(2);
    uint64_t val = 0;
    for (char c : numPart) {
        if (c < '0' || c > '9')
            return std::nullopt;
        val = val * 10 + static_cast<uint64_t>(c - '0');
        if (val > 64)
            return std::nullopt;
    }
    if (val == 0)
        return std::nullopt;
    for (std::size_t i = 0; i < channels.size(); ++i) {
        if (static_cast<uint64_t>(channels[i].displayNumber()) == val)
            return i;
    }
    return std::nullopt;
}

std::string hexByte(uint32_t v, int width = 2)
{
    std::ostringstream ss;
    ss << "0x" << std::uppercase << std::hex << std::setw(width) << std::setfill('0') << v;
    return ss.str();
}

std::optional<DecoderSpec> parseOneSpec(std::string_view rawText, std::string* error)
{
    auto fail = [&](std::string msg) -> std::optional<DecoderSpec> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    const std::string raw = trim(rawText);
    const auto colon = raw.find(':');
    if (colon == std::string::npos)
        return fail("decoder spec '" + raw + "' must start with '<protocol>:' (uart:, spi:, i2c:, or can:)");
    const std::string proto = lower(trim(std::string_view(raw).substr(0, colon)));
    const auto tokens = splitTokens(std::string_view(raw).substr(colon + 1), ':', ',');
    if (tokens.empty())
        return fail("decoder spec '" + raw + "' is missing channel parameters");

    DecoderSpec spec;
    spec.raw = raw;
    if (proto == "uart") {
        spec.kind = ProtocolKind::Uart;
        spec.baudOrBitrate = 115200;
        spec.bits = 8;
        spec.parity = 0;
        spec.stopBits = 1;
        for (const auto& tok : tokens) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) {
                if (lower(tok) == "invert") {
                    spec.invert = true;
                } else if (spec.rxOrSdaOrMosi.empty()) {
                    spec.rxOrSdaOrMosi = tok;
                } else {
                    return fail("unknown UART option '" + tok + "'");
                }
                continue;
            }
            const std::string k = lower(trim(std::string_view(tok).substr(0, eq)));
            const std::string v = trim(std::string_view(tok).substr(eq + 1));
            if (k == "rx" || k == "tx" || k == "ch" || k == "channel") {
                spec.rxOrSdaOrMosi = v;
            } else if (k == "baud" || k == "rate") {
                const auto q = cli::parseQuantity(v);
                if (!q || *q == 0 || *q > 100'000'000)
                    return fail("invalid UART baud='" + v + "'");
                spec.baudOrBitrate = static_cast<uint32_t>(*q);
            } else if (k == "bits") {
                const auto q = cli::parseQuantity(v);
                if (!q || *q < 5 || *q > 9)
                    return fail("UART bits must be 5..9");
                spec.bits = static_cast<uint8_t>(*q);
            } else if (k == "parity") {
                const std::string pv = lower(v);
                if (pv == "none" || pv == "n")
                    spec.parity = 0;
                else if (pv == "even" || pv == "e")
                    spec.parity = 1;
                else if (pv == "odd" || pv == "o")
                    spec.parity = 2;
                else
                    return fail("UART parity must be none, even, or odd");
            } else if (k == "stop") {
                const auto q = cli::parseQuantity(v);
                if (!q || (*q != 1 && *q != 2))
                    return fail("UART stop bits must be 1 or 2");
                spec.stopBits = static_cast<uint8_t>(*q);
            } else if (k == "invert") {
                const std::string iv = lower(v);
                if (iv == "1" || iv == "true" || iv == "yes")
                    spec.invert = true;
                else if (iv == "0" || iv == "false" || iv == "no")
                    spec.invert = false;
                else
                    return fail("UART invert must be true/false or 1/0");
            } else {
                return fail("unknown UART option '" + k + "'");
            }
        }
        if (spec.rxOrSdaOrMosi.empty())
            return fail("UART decoder requires a channel (e.g. uart:1:baud=115200)");
    } else if (proto == "spi") {
        spec.kind = ProtocolKind::Spi;
        spec.bits = 8;
        spec.spiMode = 0;
        for (const auto& tok : tokens) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) {
                const std::string lt = lower(tok);
                if (lt == "cs_high")
                    spec.csHigh = true;
                else if (lt == "lsb")
                    spec.lsbFirst = true;
                else if (lt == "msb")
                    spec.lsbFirst = false;
                else
                    return fail("unknown SPI option '" + tok + "'");
                continue;
            }
            const std::string k = lower(trim(std::string_view(tok).substr(0, eq)));
            const std::string v = trim(std::string_view(tok).substr(eq + 1));
            if (k == "clk" || k == "sck" || k == "sclk") {
                spec.clkOrScl = v;
            } else if (k == "mosi" || k == "sdi" || k == "data") {
                spec.rxOrSdaOrMosi = v;
            } else if (k == "miso" || k == "sdo") {
                spec.miso = v;
            } else if (k == "cs" || k == "ss" || k == "ncs") {
                spec.cs = v;
            } else if (k == "mode") {
                const auto q = cli::parseQuantity(v);
                if (!q || *q > 3)
                    return fail("SPI mode must be 0..3");
                spec.spiMode = static_cast<uint8_t>(*q);
            } else if (k == "bits") {
                const auto q = cli::parseQuantity(v);
                if (!q || *q < 4 || *q > 32)
                    return fail("SPI bits must be 4..32");
                spec.bits = static_cast<uint8_t>(*q);
            } else if (k == "cs_high") {
                const std::string iv = lower(v);
                spec.csHigh = (iv == "1" || iv == "true" || iv == "yes");
            } else if (k == "bit_order") {
                const std::string bo = lower(v);
                if (bo == "lsb")
                    spec.lsbFirst = true;
                else if (bo == "msb")
                    spec.lsbFirst = false;
                else
                    return fail("SPI bit_order must be msb or lsb");
            } else {
                return fail("unknown SPI option '" + k + "'");
            }
        }
        if (spec.clkOrScl.empty() || (spec.rxOrSdaOrMosi.empty() && spec.miso.empty()))
            return fail("SPI decoder requires clk=<ch> and mosi=<ch> (and/or miso=<ch>)");
    } else if (proto == "i2c") {
        spec.kind = ProtocolKind::I2c;
        for (const auto& tok : tokens) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos)
                return fail("unknown I2C option '" + tok + "' (expected scl=<ch>,sda=<ch>)");
            const std::string k = lower(trim(std::string_view(tok).substr(0, eq)));
            const std::string v = trim(std::string_view(tok).substr(eq + 1));
            if (k == "scl" || k == "clk")
                spec.clkOrScl = v;
            else if (k == "sda" || k == "data")
                spec.rxOrSdaOrMosi = v;
            else
                return fail("unknown I2C option '" + k + "'");
        }
        if (spec.clkOrScl.empty() || spec.rxOrSdaOrMosi.empty())
            return fail("I2C decoder requires scl=<ch> and sda=<ch>");
    } else if (proto == "can") {
        spec.kind = ProtocolKind::Can;
        spec.baudOrBitrate = 500'000;
        for (const auto& tok : tokens) {
            const auto eq = tok.find('=');
            if (eq == std::string::npos) {
                if (lower(tok) == "invert")
                    spec.invert = true;
                else if (spec.rxOrSdaOrMosi.empty())
                    spec.rxOrSdaOrMosi = tok;
                else
                    return fail("unknown CAN option '" + tok + "'");
                continue;
            }
            const std::string k = lower(trim(std::string_view(tok).substr(0, eq)));
            const std::string v = trim(std::string_view(tok).substr(eq + 1));
            if (k == "rx" || k == "tx" || k == "ch" || k == "channel") {
                spec.rxOrSdaOrMosi = v;
            } else if (k == "bitrate" || k == "baud" || k == "rate") {
                const auto q = cli::parseQuantity(v);
                if (!q || *q == 0 || *q > 20'000'000)
                    return fail("invalid CAN bitrate='" + v + "'");
                spec.baudOrBitrate = static_cast<uint32_t>(*q);
            } else if (k == "invert") {
                const std::string iv = lower(v);
                spec.invert = (iv == "1" || iv == "true" || iv == "yes");
            } else {
                return fail("unknown CAN option '" + k + "'");
            }
        }
        if (spec.rxOrSdaOrMosi.empty())
            return fail("CAN decoder requires a channel (e.g. can:1:bitrate=500k)");
    } else {
        return fail("unknown protocol '" + proto + "' (expected uart, spi, i2c, or can)");
    }
    return spec;
}

void decodeUartWindow(const CaptureBuffer& buf, const DecoderSpec& spec, std::size_t sampleOffset,
                      std::optional<uint32_t> segment, DecodedBus& out)
{
    const auto rxIdx = findChannelIndex(buf.channels(), spec.rxOrSdaOrMosi);
    if (!rxIdx || buf.size() < 2 || spec.baudOrBitrate == 0)
        return;
    const double bitSamples = static_cast<double>(buf.frequency()) / static_cast<double>(spec.baudOrBitrate);
    if (bitSamples < 2.0) {
        out.errors.push_back({sampleOffset, buf.timeOfSample(0) * 1e6, segment, "uart_undersampled",
                              "sample rate is less than 2x UART baud rate"});
        ++out.errorCount;
        return;
    }
    auto lineBit = [&](std::size_t s) -> bool { return buf.bit(s, *rxIdx) ^ spec.invert; };

    const std::size_t n = buf.size();
    std::size_t i = 1;
    // Wait until line is idle HIGH (1) before looking for the first start edge.
    if (!lineBit(0)) {
        while (i < n && !lineBit(i))
            ++i;
    }
    while (i < n) {
        if (!(lineBit(i - 1) && !lineBit(i))) {
            ++i;
            continue;
        }
        const std::size_t startEdge = i;
        const auto sampleAt = [&](double bitOffset) -> std::size_t {
            return startEdge + static_cast<std::size_t>(std::llround(bitOffset * bitSamples));
        };
        const std::size_t startCenter = sampleAt(0.5);
        if (startCenter >= n)
            break;
        if (lineBit(startCenter)) {  // glitch, not a start bit
            ++i;
            continue;
        }
        const double totalBits = 1.0 + spec.bits + (spec.parity ? 1.0 : 0.0) + spec.stopBits;
        const std::size_t lastStopCenter = sampleAt(totalBits - 0.5);
        if (lastStopCenter >= n)
            break;

        uint32_t val = 0;
        int ones = 0;
        for (uint8_t b = 0; b < spec.bits; ++b) {
            const bool bitVal = lineBit(sampleAt(1.5 + b));
            if (bitVal) {
                val |= (1u << b);
                ++ones;
            }
        }
        DecodedFrame frame;
        frame.startSample = sampleOffset + startEdge;
        frame.endSample = sampleOffset + std::min<std::size_t>(n - 1, sampleAt(totalBits));
        frame.startTimeUs = buf.timeOfSample(startEdge) * 1e6;
        frame.endTimeUs = buf.timeOfSample( std::min<std::size_t>(n - 1, sampleAt(totalBits))) * 1e6;
        frame.segment = segment;
        frame.kind = "byte";
        frame.data.push_back(val);

        if (spec.parity != 0) {
            const std::size_t pSample = sampleAt(1.5 + spec.bits);
            const bool pBit = lineBit(pSample);
            const bool expected = (spec.parity == 1) ? ((ones % 2) != 0) : ((ones % 2) == 0);
            if (pBit != expected) {
                frame.errors.push_back("parity_error");
                out.errors.push_back({sampleOffset + pSample, buf.timeOfSample(pSample) * 1e6, segment,
                                      "uart_parity_error", "UART parity mismatch on " + hexByte(val)});
                ++out.errorCount;
            }
        }

        bool stopOk = true;
        std::size_t badStopSample = lastStopCenter;
        for (uint8_t sb = 0; sb < spec.stopBits; ++sb) {
            const std::size_t sSample = sampleAt(1.5 + spec.bits + (spec.parity ? 1.0 : 0.0) + sb);
            if (!lineBit(sSample)) {
                stopOk = false;
                badStopSample = sSample;
                break;
            }
        }
        if (!stopOk) {
            frame.errors.push_back("framing_error");
            out.errors.push_back({sampleOffset + badStopSample, buf.timeOfSample(badStopSample) * 1e6, segment,
                                  "uart_framing_error", "UART stop bit was LOW after " + hexByte(val)});
            ++out.errorCount;
        }

        std::ostringstream sum;
        sum << hexByte(val);
        if (spec.bits == 8 && val >= 0x20 && val <= 0x7E)
            sum << " '" << static_cast<char>(val) << "'";
        if (!frame.errors.empty())
            sum << " [ERR]";
        frame.summary = sum.str();

        out.frames.push_back(std::move(frame));
        ++out.frameCount;

        i = std::max(startEdge + 1, lastStopCenter);
        if (!stopOk) {
            while (i < n && !lineBit(i))
                ++i;
        }
    }
}

void decodeSpiWindow(const CaptureBuffer& buf, const DecoderSpec& spec, std::size_t sampleOffset,
                     std::optional<uint32_t> segment, DecodedBus& out)
{
    const auto clkIdx = findChannelIndex(buf.channels(), spec.clkOrScl);
    const auto mosiIdx = spec.rxOrSdaOrMosi.empty() ? std::nullopt : findChannelIndex(buf.channels(), spec.rxOrSdaOrMosi);
    const auto misoIdx = spec.miso.empty() ? std::nullopt : findChannelIndex(buf.channels(), spec.miso);
    const auto csIdx = spec.cs.empty() ? std::nullopt : findChannelIndex(buf.channels(), spec.cs);
    if (!clkIdx || (!mosiIdx && !misoIdx) || buf.size() < 2)
        return;

    const bool cpol = ((spec.spiMode >> 1) & 1u) != 0;
    const bool cpha = (spec.spiMode & 1u) != 0;
    auto csActive = [&](std::size_t s) -> bool {
        if (!csIdx)
            return true;
        const bool level = buf.bit(s, *csIdx);
        return spec.csHigh ? level : !level;
    };

    bool inTransfer = false;
    bool seenLeadingEdge = false;
    std::size_t transferStart = 0;
    std::size_t wordStart = 0;
    std::size_t lastClockSample = 0;
    uint8_t bitPos = 0;
    uint32_t mosiWord = 0;
    uint32_t misoWord = 0;
    DecodedFrame current;

    auto finishTransfer = [&](std::size_t endSample) {
        if (!inTransfer)
            return;
        if (bitPos > 0) {
            current.errors.push_back("incomplete_word");
            out.errors.push_back({sampleOffset + endSample, buf.timeOfSample(endSample) * 1e6, segment,
                                  "spi_incomplete_word",
                                  "SPI transfer ended with " + std::to_string(bitPos) + " of " +
                                      std::to_string(spec.bits) + " bits clocked"});
            ++out.errorCount;
        }
        if (!current.data.empty() || !current.misoData.empty() || !current.errors.empty()) {
            current.startSample = sampleOffset + transferStart;
            current.endSample = sampleOffset + endSample;
            current.startTimeUs = buf.timeOfSample(transferStart) * 1e6;
            current.endTimeUs = buf.timeOfSample(endSample) * 1e6;
            current.segment = segment;
            current.kind = "transfer";
            const int hexW = (spec.bits + 3) / 4;
            std::ostringstream sum;
            if (!current.data.empty()) {
                sum << "MOSI:";
                for (uint32_t w : current.data)
                    sum << " " << hexByte(w, hexW);
            }
            if (!current.misoData.empty()) {
                if (sum.tellp() > 0)
                    sum << " / ";
                sum << "MISO:";
                for (uint32_t w : current.misoData)
                    sum << " " << hexByte(w, hexW);
            }
            if (!current.errors.empty())
                sum << " [INCOMPLETE]";
            current.summary = sum.str();
            out.frames.push_back(std::move(current));
            ++out.frameCount;
        }
        current = DecodedFrame{};
        inTransfer = false;
        seenLeadingEdge = false;
        bitPos = 0;
        mosiWord = 0;
        misoWord = 0;
    };

    const std::size_t n = buf.size();
    if (csActive(0)) {
        inTransfer = true;
        transferStart = 0;
    }
    for (std::size_t i = 1; i < n; ++i) {
        const bool activePrev = csActive(i - 1);
        const bool activeCur = csActive(i);
        if (!activePrev && activeCur) {
            inTransfer = true;
            seenLeadingEdge = false;
            transferStart = i;
            bitPos = 0;
            mosiWord = 0;
            misoWord = 0;
            current = DecodedFrame{};
        } else if (activePrev && !activeCur) {
            finishTransfer(i);
            continue;
        }
        if (!activeCur)
            continue;

        const bool clkPrev = buf.bit(i - 1, *clkIdx);
        const bool clkCur = buf.bit(i, *clkIdx);
        if (clkPrev == clkCur)
            continue;

        const bool isLeading = (clkCur != cpol);
        bool sampleHere = false;
        if (!cpha) {
            sampleHere = isLeading;
        } else {
            if (isLeading)
                seenLeadingEdge = true;
            else if (seenLeadingEdge)
                sampleHere = true;
        }
        if (!sampleHere)
            continue;

        if (!csIdx && !inTransfer) {
            inTransfer = true;
            transferStart = i;
        }
        if (bitPos == 0)
            wordStart = i;
        lastClockSample = i;
        const uint8_t shift = spec.lsbFirst ? bitPos : static_cast<uint8_t>(spec.bits - 1 - bitPos);
        if (mosiIdx && buf.bit(i, *mosiIdx))
            mosiWord |= (1u << shift);
        if (misoIdx && buf.bit(i, *misoIdx))
            misoWord |= (1u << shift);
        ++bitPos;
        if (bitPos == spec.bits) {
            if (mosiIdx)
                current.data.push_back(mosiWord);
            if (misoIdx)
                current.misoData.push_back(misoWord);
            bitPos = 0;
            mosiWord = 0;
            misoWord = 0;
            if (!csIdx) {
                transferStart = wordStart;
                finishTransfer(lastClockSample);
            }
        }
    }
    if (inTransfer)
        finishTransfer(lastClockSample > transferStart ? lastClockSample : (n - 1));
}

void decodeI2cWindow(const CaptureBuffer& buf, const DecoderSpec& spec, std::size_t sampleOffset,
                     std::optional<uint32_t> segment, DecodedBus& out)
{
    const auto sclIdx = findChannelIndex(buf.channels(), spec.clkOrScl);
    const auto sdaIdx = findChannelIndex(buf.channels(), spec.rxOrSdaOrMosi);
    if (!sclIdx || !sdaIdx || buf.size() < 2)
        return;

    bool active = false;
    bool haveAddress = false;
    bool bitSampledThisHigh = false;
    uint8_t bitPos = 0;
    uint8_t curByte = 0;
    DecodedFrame frame;

    auto finalizeFrame = [&](std::size_t endSample) {
        if (!active)
            return;
        if (bitSampledThisHigh && bitPos > 0) {
            --bitPos;
            curByte >>= 1;
        }
        if (bitPos > 0) {
            frame.errors.push_back("incomplete_byte");
            out.errors.push_back({sampleOffset + endSample, buf.timeOfSample(endSample) * 1e6, segment,
                                  "i2c_incomplete_byte",
                                  "I2C condition interrupted a byte after " + std::to_string(bitPos) + " bit(s)"});
            ++out.errorCount;
        }
        if (haveAddress || !frame.errors.empty()) {
            frame.endSample = sampleOffset + endSample;
            frame.endTimeUs = buf.timeOfSample(endSample) * 1e6;
            frame.segment = segment;
            std::ostringstream sum;
            if (frame.address) {
                sum << (*frame.read ? "R " : "W ") << hexByte(*frame.address);
                if (!frame.data.empty()) {
                    sum << ":";
                    for (uint32_t b : frame.data)
                        sum << " " << hexByte(b);
                }
                sum << (*frame.ack ? " [ACK]" : " [NACK]");
            } else {
                sum << "I2C [INCOMPLETE]";
            }
            frame.summary = sum.str();
            out.frames.push_back(std::move(frame));
            ++out.frameCount;
        }
        frame = DecodedFrame{};
        active = false;
        haveAddress = false;
        bitSampledThisHigh = false;
        bitPos = 0;
        curByte = 0;
    };

    const std::size_t n = buf.size();
    for (std::size_t i = 1; i < n; ++i) {
        const bool sclPrev = buf.bit(i - 1, *sclIdx);
        const bool sclCur = buf.bit(i, *sclIdx);
        const bool sdaPrev = buf.bit(i - 1, *sdaIdx);
        const bool sdaCur = buf.bit(i, *sdaIdx);

        if (sclPrev && !sclCur)
            bitSampledThisHigh = false;

        if (sclPrev && sclCur && sdaPrev != sdaCur) {
            if (sdaPrev && !sdaCur) {  // START or RESTART
                if (active)
                    finalizeFrame(i);
                active = true;
                haveAddress = false;
                bitSampledThisHigh = false;
                bitPos = 0;
                curByte = 0;
                frame = DecodedFrame{};
                frame.startSample = sampleOffset + i;
                frame.startTimeUs = buf.timeOfSample(i) * 1e6;
            } else {  // STOP
                finalizeFrame(i);
            }
            continue;
        }

        if (active && !sclPrev && sclCur) {  // SCL rising edge
            if (bitPos < 8) {
                curByte = static_cast<uint8_t>((curByte << 1) | (sdaCur ? 1u : 0u));
                ++bitPos;
                bitSampledThisHigh = true;
            } else {
                bitSampledThisHigh = false;
                const bool ack = !sdaCur;
                if (!haveAddress) {
                    haveAddress = true;
                    frame.address = static_cast<uint32_t>(curByte >> 1);
                    frame.read = (curByte & 1u) != 0;
                    frame.kind = *frame.read ? "read" : "write";
                    frame.ack = ack;
                    if (!ack) {
                        frame.errors.push_back("address_nack");
                        out.errors.push_back({sampleOffset + i, buf.timeOfSample(i) * 1e6, segment, "i2c_nack",
                                              "I2C address " + hexByte(*frame.address) + " was NACKed"});
                        ++out.errorCount;
                        ++out.nackCount;
                    }
                } else {
                    frame.data.push_back(curByte);
                    if (!ack && !*frame.read) {
                        frame.ack = false;
                        frame.errors.push_back("data_nack");
                        out.errors.push_back({sampleOffset + i, buf.timeOfSample(i) * 1e6, segment, "i2c_nack",
                                              "I2C write byte " + hexByte(curByte) + " to " +
                                                  hexByte(*frame.address) + " was NACKed"});
                        ++out.errorCount;
                        ++out.nackCount;
                    }
                }
                bitPos = 0;
                curByte = 0;
            }
        }
    }
    if (active)
        finalizeFrame(n - 1);
}

uint16_t canCrc15(const std::vector<uint8_t>& bits, std::size_t count)
{
    uint16_t crc = 0;
    for (std::size_t i = 0; i < count; ++i) {
        const uint16_t bit = bits[i] & 1u;
        const uint16_t nxt = bit ^ ((crc >> 14) & 1u);
        crc = static_cast<uint16_t>((crc << 1) & 0x7FFFu);
        if (nxt)
            crc ^= 0x4599u;
    }
    return crc;
}

void decodeCanWindow(const CaptureBuffer& buf, const DecoderSpec& spec, std::size_t sampleOffset,
                     std::optional<uint32_t> segment, DecodedBus& out)
{
    const auto rxIdx = findChannelIndex(buf.channels(), spec.rxOrSdaOrMosi);
    if (!rxIdx || buf.size() < 4 || spec.baudOrBitrate == 0)
        return;
    const double bitSamples = static_cast<double>(buf.frequency()) / static_cast<double>(spec.baudOrBitrate);
    if (bitSamples < 2.0) {
        out.errors.push_back({sampleOffset, buf.timeOfSample(0) * 1e6, segment, "can_undersampled",
                              "sample rate is less than 2x CAN bitrate"});
        ++out.errorCount;
        return;
    }
    auto lineBit = [&](std::size_t s) -> uint8_t { return (buf.bit(s, *rxIdx) ^ spec.invert) ? 1u : 0u; };

    const std::size_t n = buf.size();
    std::size_t i = 1;
    while (i < n && lineBit(i - 1) == 0)
        ++i;

    while (i < n) {
        if (!(lineBit(i - 1) == 1 && lineBit(i) == 0)) {
            ++i;
            continue;
        }
        const std::size_t sofEdge = i;
        const std::size_t sofCenter = sofEdge + static_cast<std::size_t>(std::llround(0.5 * bitSamples));
        if (sofCenter >= n || lineBit(sofCenter) != 0) {
            ++i;
            continue;
        }

        double bitStart = static_cast<double>(sofEdge);
        auto nextRawBit = [&](std::size_t& sampledAt) -> std::optional<uint8_t> {
            const double expectedBoundary = bitStart + bitSamples;
            const auto lo = static_cast<std::size_t>(std::max(1.0, std::floor(expectedBoundary - 0.35 * bitSamples)));
            const auto hi = static_cast<std::size_t>(std::min(static_cast<double>(n - 1), std::ceil(expectedBoundary + 0.35 * bitSamples)));
            double boundary = expectedBoundary;
            for (std::size_t s = lo; s <= hi; ++s) {
                if (lineBit(s - 1) == 1 && lineBit(s) == 0) {
                    boundary = static_cast<double>(s);
                    break;
                }
            }
            bitStart = boundary;
            sampledAt = static_cast<std::size_t>(std::llround(bitStart + 0.5 * bitSamples));
            if (sampledAt >= n)
                return std::nullopt;
            return lineBit(sampledAt);
        };

        std::vector<uint8_t> destuffed;
        destuffed.push_back(0);  // SOF
        uint8_t lastRaw = 0;
        int sameCount = 1;
        std::size_t targetDestuffed = 19;  // SOF + 11 ID + RTR + IDE + r0 + 4 DLC (until IDE is known)
        bool headerParsed = false;
        bool ide = false;
        bool rtr = false;
        uint32_t canId = 0;
        uint8_t dlc = 0;
        uint8_t dataLen = 0;
        bool stuffError = false;
        std::size_t lastSampled = sofCenter;

        auto readBits = [&](const std::vector<uint8_t>& v, std::size_t pos, std::size_t len) -> uint32_t {
            uint32_t val = 0;
            for (std::size_t k = 0; k < len; ++k)
                val = (val << 1) | (v[pos + k] & 1u);
            return val;
        };

        while (destuffed.size() < targetDestuffed) {
            std::size_t sAt = 0;
            const auto raw = nextRawBit(sAt);
            if (!raw)
                break;
            lastSampled = sAt;
            if (sameCount == 5) {
                if (*raw == lastRaw) {
                    stuffError = true;
                    break;
                }
                lastRaw = *raw;
                sameCount = 1;
                continue;  // discard stuff bit
            }
            if (*raw == lastRaw) {
                ++sameCount;
            } else {
                lastRaw = *raw;
                sameCount = 1;
            }
            destuffed.push_back(*raw);

            if (!headerParsed && destuffed.size() == 14) {
                ide = destuffed[13] != 0;
                if (ide)
                    targetDestuffed = 39;  // SOF(1)+base(11)+SRR(1)+IDE(1)+ext(18)+RTR(1)+r1(1)+r0(1)+DLC(4)
            }
            if (!headerParsed && destuffed.size() == (ide ? 39u : 19u)) {
                headerParsed = true;
                const uint32_t baseId = readBits(destuffed, 1, 11);
                if (!ide) {
                    rtr = destuffed[12] != 0;
                    canId = baseId;
                    dlc = static_cast<uint8_t>(readBits(destuffed, 15, 4));
                } else {
                    const uint32_t extId = readBits(destuffed, 14, 18);
                    rtr = destuffed[32] != 0;
                    canId = (baseId << 18) | extId;
                    dlc = static_cast<uint8_t>(readBits(destuffed, 35, 4));
                }
                dataLen = rtr ? 0 : std::min<uint8_t>(dlc, 8);
                targetDestuffed = (ide ? 39u : 19u) + static_cast<std::size_t>(dataLen) * 8u + 15u;
            }
        }

        DecodedFrame frame;
        frame.startSample = sampleOffset + sofEdge;
        frame.startTimeUs = buf.timeOfSample(sofEdge) * 1e6;
        frame.segment = segment;

        if (stuffError) {
            frame.endSample = sampleOffset + lastSampled;
            frame.endTimeUs = buf.timeOfSample(lastSampled) * 1e6;
            frame.kind = "error";
            frame.errors.push_back("stuff_error");
            frame.summary = "CAN [STUFF ERR]";
            out.errors.push_back({sampleOffset + lastSampled, buf.timeOfSample(lastSampled) * 1e6, segment,
                                  "can_stuff_error", "CAN bit-stuffing rule violated (6 identical consecutive bits)"});
            ++out.errorCount;
            out.frames.push_back(std::move(frame));
            ++out.frameCount;
            i = lastSampled + 1;
            while (i < n && lineBit(i) == 0)
                ++i;
            continue;
        }
        if (destuffed.size() < targetDestuffed)
            break;

        const std::size_t dataStart = ide ? 39u : 19u;
        for (uint8_t b = 0; b < dataLen; ++b)
            frame.data.push_back(readBits(destuffed, dataStart + static_cast<std::size_t>(b) * 8u, 8));

        const std::size_t crcStart = dataStart + static_cast<std::size_t>(dataLen) * 8u;
        const uint16_t rxCrc = static_cast<uint16_t>(readBits(destuffed, crcStart, 15));
        const uint16_t calcCrc = canCrc15(destuffed, crcStart);

        frame.address = canId;
        frame.extendedId = ide;
        frame.remoteFrame = rtr;
        frame.kind = rtr ? "remote_frame" : "data_frame";

        if (rxCrc != calcCrc) {
            frame.errors.push_back("crc_error");
            out.errors.push_back({sampleOffset + lastSampled, buf.timeOfSample(lastSampled) * 1e6, segment,
                                  "can_crc_error",
                                  "CAN CRC-15 mismatch on ID " + hexByte(canId, ide ? 8 : 3) + " (got " +
                                      hexByte(rxCrc, 4) + ", expected " + hexByte(calcCrc, 4) + ")"});
            ++out.errorCount;
        }

        // Unstuffed trailer: CRC delimiter (1), ACK slot (0 = ACK, 1 = NACK), ACK delimiter (1).
        std::size_t sCrcDelim = 0, sAckSlot = 0, sAckDelim = 0;
        const auto crcDelim = nextRawBit(sCrcDelim);
        const auto ackSlot = crcDelim ? nextRawBit(sAckSlot) : std::nullopt;
        const auto ackDelim = ackSlot ? nextRawBit(sAckDelim) : std::nullopt;
        if (crcDelim)
            lastSampled = sCrcDelim;
        if (ackSlot) {
            lastSampled = sAckSlot;
            const bool acked = (*ackSlot == 0);
            frame.ack = acked;
            if (!acked) {
                frame.errors.push_back("ack_error");
                out.errors.push_back({sampleOffset + sAckSlot, buf.timeOfSample(sAckSlot) * 1e6, segment,
                                      "can_ack_error", "CAN frame ID " + hexByte(canId, ide ? 8 : 3) + " was not ACKed"});
                ++out.errorCount;
                ++out.nackCount;
            }
        }
        if (ackDelim)
            lastSampled = sAckDelim;
        if ((crcDelim && *crcDelim == 0) || (ackDelim && *ackDelim == 0)) {
            frame.errors.push_back("form_error");
            out.errors.push_back({sampleOffset + lastSampled, buf.timeOfSample(lastSampled) * 1e6, segment,
                                  "can_form_error", "CAN delimiter bit was dominant (0)"});
            ++out.errorCount;
        }

        const std::size_t frameEnd = std::min<std::size_t>(
            n - 1, lastSampled + static_cast<std::size_t>(std::llround(7.0 * bitSamples)));
        frame.endSample = sampleOffset + frameEnd;
        frame.endTimeUs = buf.timeOfSample(frameEnd) * 1e6;

        std::ostringstream sum;
        sum << "ID " << hexByte(canId, ide ? 8 : 3) << (ide ? " (ext)" : "") << " [" << static_cast<int>(dlc) << "]";
        if (rtr) {
            sum << " RTR";
        } else {
            for (uint32_t d : frame.data)
                sum << " " << std::uppercase << std::hex << std::setw(2) << std::setfill('0') << d;
            sum << std::dec;
        }
        if (frame.ack)
            sum << (*frame.ack ? " [ACK]" : " [NACK]");
        if (!frame.errors.empty())
            sum << " [ERR]";
        frame.summary = sum.str();

        out.frames.push_back(std::move(frame));
        ++out.frameCount;

        i = frameEnd + 1;
        while (i < n && lineBit(i) == 0)
            ++i;
    }
}

void decodeIntoWindow(const CaptureBuffer& buf, const DecoderSpec& spec, std::size_t sampleOffset,
                      std::optional<uint32_t> segment, DecodedBus& out)
{
    switch (spec.kind) {
    case ProtocolKind::Uart: decodeUartWindow(buf, spec, sampleOffset, segment, out); break;
    case ProtocolKind::Spi: decodeSpiWindow(buf, spec, sampleOffset, segment, out); break;
    case ProtocolKind::I2c: decodeI2cWindow(buf, spec, sampleOffset, segment, out); break;
    case ProtocolKind::Can: decodeCanWindow(buf, spec, sampleOffset, segment, out); break;
    }
}

}  // namespace

std::string_view toString(ProtocolKind kind) noexcept
{
    switch (kind) {
    case ProtocolKind::Uart: return "uart";
    case ProtocolKind::Spi: return "spi";
    case ProtocolKind::I2c: return "i2c";
    case ProtocolKind::Can: return "can";
    }
    return "uart";
}

std::optional<std::vector<DecoderSpec>> parseDecoderSpecs(std::string_view text, std::string* error)
{
    std::vector<DecoderSpec> out;
    for (const auto& part : splitTokens(text, ';')) {
        auto spec = parseOneSpec(part, error);
        if (!spec)
            return std::nullopt;
        out.push_back(std::move(*spec));
    }
    if (out.empty()) {
        if (error)
            *error = "empty --decode specification";
        return std::nullopt;
    }
    return out;
}

bool validateDecoderChannels(const std::vector<DecoderSpec>& specs, const std::vector<ChannelInfo>& channels,
                             std::string* error)
{
    auto check = [&](const std::string& token, const std::string& proto) -> bool {
        if (token.empty())
            return true;
        if (!findChannelIndex(channels, token)) {
            if (error)
                *error = proto + " decoder channel '" + token + "' is not in the capture";
            return false;
        }
        return true;
    };
    for (const auto& s : specs) {
        const std::string p(toString(s.kind));
        if (!check(s.rxOrSdaOrMosi, p) || !check(s.clkOrScl, p) || !check(s.miso, p) || !check(s.cs, p))
            return false;
    }
    return true;
}

std::vector<DecodedBus> decodeProtocols(const CaptureBuffer& buffer, const std::vector<DecoderSpec>& specs,
                                        const std::optional<SegmentLayout>& segments)
{
    std::vector<DecodedBus> results;
    results.reserve(specs.size());
    const bool segmented = segments && segments->count > 1 && buffer.size() == segments->totalSamples();
    std::vector<CaptureBuffer> windows;
    if (segmented)
        windows = splitSegments(buffer, *segments);

    for (const auto& spec : specs) {
        DecodedBus bus;
        bus.spec = spec.raw;
        bus.protocol = std::string(toString(spec.kind));
        const std::string& primaryToken = !spec.rxOrSdaOrMosi.empty() ? spec.rxOrSdaOrMosi
                                          : !spec.miso.empty()        ? spec.miso
                                                                      : spec.clkOrScl;
        bus.channelIndex = findChannelIndex(buffer.channels(), primaryToken);
        if (segmented && windows.size() == segments->count) {
            for (uint32_t w = 0; w < segments->count; ++w) {
                const bool hasSyntheticBaseline = (w > 0 && segments->triggerChannel.has_value());
                const CaptureBuffer win =
                    hasSyntheticBaseline ? windows[w].slice(1, segments->postSamples) : windows[w];
                const std::size_t offset =
                    w == 0 ? 0 : (segments->preSamples + static_cast<std::size_t>(w) * segments->postSamples);
                decodeIntoWindow(win, spec, offset, w + 1, bus);
            }
        } else {
            decodeIntoWindow(buffer, spec, 0, std::nullopt, bus);
        }
        results.push_back(std::move(bus));
    }
    return results;
}

nlohmann::json toJson(const std::vector<DecodedBus>& buses)
{
    auto arr = nlohmann::json::array();
    for (const auto& b : buses) {
        auto frames = nlohmann::json::array();
        for (const auto& f : b.frames) {
            nlohmann::json jf = {{"start_sample", f.startSample},
                                 {"end_sample", f.endSample},
                                 {"start_us", f.startTimeUs},
                                 {"end_us", f.endTimeUs},
                                 {"kind", f.kind},
                                 {"summary", f.summary},
                                 {"data", f.data},
                                 {"errors", f.errors}};
            if (f.segment)
                jf["segment"] = *f.segment;
            if (f.address)
                jf["address"] = *f.address;
            if (f.extendedId)
                jf["extended_id"] = *f.extendedId;
            if (f.remoteFrame)
                jf["remote_frame"] = *f.remoteFrame;
            if (f.read)
                jf["read"] = *f.read;
            if (f.ack)
                jf["ack"] = *f.ack;
            if (!f.misoData.empty())
                jf["miso_data"] = f.misoData;
            frames.push_back(std::move(jf));
        }
        auto errs = nlohmann::json::array();
        for (const auto& e : b.errors) {
            nlohmann::json je = {{"sample", e.sample},
                                 {"time_us", e.timeUs},
                                 {"kind", e.kind},
                                 {"detail", e.detail}};
            if (e.segment)
                je["segment"] = *e.segment;
            errs.push_back(std::move(je));
        }
        arr.push_back({{"spec", b.spec},
                       {"protocol", b.protocol},
                       {"frames", b.frameCount},
                       {"errors", b.errorCount},
                       {"nacks", b.nackCount},
                       {"transactions", std::move(frames)},
                       {"error_events", std::move(errs)}});
    }
    return arr;
}

std::string decodedMarkdown(const std::vector<DecodedBus>& buses)
{
    if (buses.empty())
        return {};
    std::ostringstream ss;
    ss << "\n### Decoded bus protocols\n";
    for (const auto& b : buses) {
        ss << "\n- **" << b.protocol << "** (`" << b.spec << "`): " << b.frameCount << " frame(s), " << b.errorCount
           << " error(s)";
        if (b.nackCount > 0)
            ss << ", " << b.nackCount << " NACK(s)";
        ss << "\n";
        if (!b.frames.empty()) {
            const std::size_t shown = std::min<std::size_t>(b.frames.size(), 32);
            ss << "\n| # | Start | End | Summary |\n|---|---|---|---|\n";
            for (std::size_t i = 0; i < shown; ++i) {
                const auto& f = b.frames[i];
                ss << "| " << (i + 1) << " | " << formatTime(f.startTimeUs * 1e-6);
                if (f.segment)
                    ss << " (W" << *f.segment << ")";
                ss << " | " << formatTime(f.endTimeUs * 1e-6) << " | `" << f.summary << "` |\n";
            }
        }
    }
    return ss.str();
}

}  // namespace lt
