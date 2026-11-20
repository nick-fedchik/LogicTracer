// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

// Deterministic bus protocol decoders over a CaptureBuffer (pure C++20, no Qt):
//   uart:<rx>[:baud=<N>][:bits=5..9][:parity=none|even|odd][:stop=1|2][:invert]
//   spi:clk=<ch>,mosi=<ch>[,miso=<ch>][,cs=<ch>][:mode=0..3][:bits=4..32][:cs_high][:lsb]
//   i2c:scl=<ch>,sda=<ch>
//   can:<rx>[:bitrate=<N>][:invert]

#include "core/CaptureBuffer.hpp"
#include "core/CaptureTypes.hpp"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lt {

enum class ProtocolKind { Uart, Spi, I2c, Can };

[[nodiscard]] std::string_view toString(ProtocolKind kind) noexcept;

struct DecoderSpec {
    std::string raw;
    ProtocolKind kind = ProtocolKind::Uart;
    std::string rxOrSdaOrMosi;   // UART rx, CAN rx, I2C sda, SPI mosi
    std::string clkOrScl;        // I2C scl, SPI clk
    std::string miso;            // SPI miso (optional)
    std::string cs;              // SPI cs (optional)
    uint32_t baudOrBitrate = 0;  // UART default 115200, CAN default 500000
    uint8_t bits = 8;            // UART 5..9, SPI 4..32
    uint8_t parity = 0;          // UART: 0 = none, 1 = even, 2 = odd
    uint8_t stopBits = 1;        // UART: 1 or 2
    uint8_t spiMode = 0;         // SPI: 0..3
    bool invert = false;         // UART / CAN polarity inversion
    bool csHigh = false;         // SPI active-high CS
    bool lsbFirst = false;       // SPI LSB-first (default MSB-first)
};

struct DecodedFrame {
    std::size_t startSample = 0;
    std::size_t endSample = 0;
    double startTimeUs = 0.0;
    double endTimeUs = 0.0;
    std::optional<uint32_t> segment;  // 1-based window when segmented
    std::string kind;                 // "byte", "transfer", "write", "read", "data_frame", "remote_frame"
    std::string summary;
    std::optional<uint32_t> address;  // I2C 7-bit address or CAN 11/29-bit ID
    std::optional<bool> extendedId;   // CAN 29-bit IDE
    std::optional<bool> remoteFrame;  // CAN RTR
    std::optional<bool> read;         // I2C R/W
    std::optional<bool> ack;          // I2C / CAN ACK
    std::vector<uint32_t> data;
    std::vector<uint32_t> misoData;
    std::vector<std::string> errors;
};

struct DecodedError {
    std::size_t sample = 0;
    double timeUs = 0.0;
    std::optional<uint32_t> segment;
    std::string kind;
    std::string detail;
};

struct DecodedBus {
    std::string spec;
    std::string protocol;  // "uart", "spi", "i2c", "can"
    std::optional<std::size_t> channelIndex;  // primary channel index in CaptureBuffer::channels()
    std::size_t frameCount = 0;
    std::size_t errorCount = 0;
    std::size_t nackCount = 0;
    std::vector<DecodedFrame> frames;
    std::vector<DecodedError> errors;
};

// Parses one or more ';'-separated decoder specifications.
[[nodiscard]] std::optional<std::vector<DecoderSpec>> parseDecoderSpecs(std::string_view text, std::string* error);

// Verifies that every channel referenced in `specs` exists in `channels`.
[[nodiscard]] bool validateDecoderChannels(const std::vector<DecoderSpec>& specs,
                                           const std::vector<ChannelInfo>& channels, std::string* error);

// Runs `specs` on `buffer` (or window-by-window when `segments` is present and has count > 1).
[[nodiscard]] std::vector<DecodedBus> decodeProtocols(const CaptureBuffer& buffer,
                                                      const std::vector<DecoderSpec>& specs,
                                                      const std::optional<SegmentLayout>& segments = std::nullopt);

[[nodiscard]] nlohmann::json toJson(const std::vector<DecodedBus>& buses);
[[nodiscard]] std::string decodedMarkdown(const std::vector<DecodedBus>& buses);

}  // namespace lt
