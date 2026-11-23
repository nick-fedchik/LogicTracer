// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include <QString>
#include <nlohmann/json.hpp>

#include <cstdint>
#include <functional>
#include <string>

namespace lt::events {

inline constexpr char format[] = "logictracer-events-v1";
inline constexpr int maxEvents = 10000;
inline constexpr int maxDurationMs = 3600000;

enum class Source { Can, Gpio };
struct Request {
    Source source = Source::Can;
    std::string device;
    uint32_t line = 0;
    bool hte = false;
    int durationMs = 1000;
    int limit = 1000;
};

enum class Status { Ok, Error, Timeout, Cancelled };
struct Result {
    Status status = Status::Error;
    std::string message;
    nlohmann::json recording;
    std::string kind = "InputError";
};
using Cancel = std::function<bool()>;

std::string validate(const Request& request);
Result record(const Request& request, const Cancel& cancel = {});
bool load(const QString& path, nlohmann::json& recording, QString& error);
bool save(const QString& path, const nlohmann::json& recording, QString& error);
std::string describe(const nlohmann::json& recording);

}  // namespace lt::events
