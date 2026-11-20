// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "core/Exporters.hpp"

#include "core/ArgParsing.hpp"
#include "core/Format.hpp"
#include "core/Paths.hpp"
#include "core/Segments.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <numeric>
#include <sstream>
#include <streambuf>

#include <cerrno>
#include <random>
#include <stdlib.h>
#if defined(_WIN32)
#include <cstdio>
#include <io.h>
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace lt {

namespace {

// RFC 4648 Base64 decoder for legacy .lac sample arrays. Like QByteArray::fromBase64 it skips
// characters outside the alphabet (line breaks, whitespace) and stops at the first '=' padding.
std::vector<uint8_t> decodeBase64(std::string_view text)
{
    auto value = [](char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '+' || ch == '-') return 62;
        if (ch == '/' || ch == '_') return 63;
        return -1;
    };
    std::vector<uint8_t> out;
    out.reserve(text.size() / 4 * 3);
    uint32_t acc = 0;
    int bits = 0;
    for (const char ch : text) {
        if (ch == '=')
            break;
        const int v = value(ch);
        if (v < 0)
            continue;
        acc = (acc << 6) | static_cast<uint32_t>(v);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<uint8_t>((acc >> bits) & 0xFFu));
        }
    }
    return out;
}

constexpr double kUs = 1e6;

// Reads a JSON number as an integer in [lo, hi] without ever converting an out-of-range or fractional
// value (a float-to-integer cast of such a value is undefined behaviour).
std::optional<int64_t> integerIn(const nlohmann::json& v, int64_t lo, int64_t hi)
{
    int64_t x = 0;
    if (v.is_number_unsigned()) {
        const auto u = v.get<uint64_t>();
        if (u > static_cast<uint64_t>(INT64_MAX))
            return std::nullopt;
        x = static_cast<int64_t>(u);
    } else if (v.is_number_integer()) {
        x = v.get<int64_t>();
    } else if (v.is_number_float()) {
        const double d = v.get<double>();
        if (!std::isfinite(d) || d != std::floor(d) || d < -9.2e18 || d > 9.2e18)
            return std::nullopt;
        x = static_cast<int64_t>(d);
    } else {
        return std::nullopt;
    }
    if (x < lo || x > hi)
        return std::nullopt;
    return x;
}

std::string utcNowIso8601()
{
    const auto now = std::chrono::system_clock::now();
    const std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#if defined(_WIN32)
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream ss;
    ss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return ss.str();
}

nlohmann::json widthJson(const std::optional<PulseWidth>& w, const ChannelStats& st)
{
    if (!w)
        return nullptr;
    return {{"min", st.samplesToSeconds(static_cast<double>(w->minSamples)) * kUs},
            {"max", st.samplesToSeconds(static_cast<double>(w->maxSamples)) * kUs},
            {"mean", st.samplesToSeconds(w->meanSamples) * kUs}};
}

template <typename T>
nlohmann::json optJson(const std::optional<T>& v, double scale = 1.0)
{
    return v ? nlohmann::json(*v * scale) : nlohmann::json(nullptr);
}

std::string vcdName(const ChannelInfo& ch)
{
    std::string s = ch.label();
    for (auto& c : s) {
        const auto u = static_cast<unsigned char>(c);
        if (u <= 0x20 || u == 0x7F || c == '$')
            c = '_';
    }
    return s;
}

std::string vcdId(std::size_t index)
{
    std::string id;
    do {
        id.push_back(static_cast<char>('!' + index % 94));
        index /= 94;
    } while (index > 0);
    return id;
}


#if !defined(_WIN32)
// Unbuffered-on-error stream buffer over a file descriptor we already own (from mkstemp), so the data is
// written through the very descriptor that was created exclusively instead of reopening a pathname.
class FdStreamBuf : public std::streambuf {
public:
    explicit FdStreamBuf(int fd) : m_fd(fd) { setp(m_buf, m_buf + sizeof m_buf); }
    [[nodiscard]] bool failed() const { return m_failed; }

protected:
    int_type overflow(int_type ch) override
    {
        if (!flushBuffer())
            return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }
    int sync() override { return flushBuffer() ? 0 : -1; }

private:
    bool flushBuffer()
    {
        const char* p = pbase();
        std::ptrdiff_t left = pptr() - pbase();
        while (left > 0 && !m_failed) {
            const ssize_t n = ::write(m_fd, p, static_cast<std::size_t>(left));
            if (n < 0 && errno == EINTR)
                continue;
            if (n <= 0) {
                m_failed = true;
                break;
            }
            p += n;
            left -= n;
        }
        setp(m_buf, m_buf + sizeof m_buf);
        return !m_failed;
    }

    int m_fd;
    bool m_failed = false;
    char m_buf[1 << 16];
};

// The process umask without the racy umask(0) round trip where the OS exposes it (Linux: /proc/self/status).
mode_t currentUmask()
{
#if defined(__linux__)
    std::ifstream status("/proc/self/status");
    for (std::string line; std::getline(status, line);) {
        if (line.rfind("Umask:", 0) == 0)
            return static_cast<mode_t>(std::strtoul(line.c_str() + 6, nullptr, 8));
    }
    return 022;
#else
    static std::mutex lock;  // only guards against our own threads; there is no read-only query
    const std::lock_guard guard(lock);
    const mode_t mask = ::umask(022);
    ::umask(mask);
    return mask;
#endif
}

// Opens the directory that will receive the output. In confined mode (paths validated by
// resolveSafeOutputPath for the session/MCP) every component is opened with O_NOFOLLOW, so a component that
// was swapped for a symlink after validation makes the write fail instead of escaping the sandbox.
int openOutputDirectory(const std::filesystem::path& dir, bool confined)
{
    if (!confined)
        return ::open(dir.empty() ? "." : dir.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (!dir.is_absolute())
        return -1;
    int fd = ::open("/", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    for (const auto& part : dir.relative_path()) {
        if (fd < 0)
            break;
        const std::string name = part.string();
        if (name.empty() || name == ".")
            continue;
        const int next = ::openat(fd, name.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
        ::close(fd);
        fd = next;
    }
    return fd;
}

// Writes through a random, exclusively created sibling file and renames it over the target, so a failed or
// partial write (disk full, quota) never destroys the previous file and a pre-planted file or symlink with a
// guessable name cannot be followed. Every step is relative to one directory descriptor (openat/renameat).
// The result keeps the mode of the file it replaces, or follows the umask. Special files (pipes, devices)
// are written in place.
template <typename Fn>
bool writeFileChecked(const std::string& path, std::ios::openmode mode, std::string* error, Fn&& write,
                      bool confined = false)
{
    (void)mode;  // text and binary are identical on POSIX
    namespace fs = std::filesystem;
    auto failWith = [&](const std::string& msg) {
        if (error)
            *error = msg + " " + path;
        return false;
    };
    const fs::path target = pathFromUtf8(path);
    const std::string leaf = utf8FromPath(target.filename());
    if (leaf.empty() || leaf == "." || leaf == "..")
        return failWith("not a file name:");
    const int dirFd = openOutputDirectory(target.parent_path(), confined);
    if (dirFd < 0)
        return failWith(confined ? "output directory changed or is not reachable without symlinks:" : "cannot write");
    struct DirCloser {
        int fd;
        ~DirCloser() { ::close(fd); }
    } dirCloser{dirFd};

    struct stat existing{};
    const bool exists = ::fstatat(dirFd, leaf.c_str(), &existing, AT_SYMLINK_NOFOLLOW) == 0;
    if (exists && !S_ISREG(existing.st_mode) && !S_ISLNK(existing.st_mode)) {
        if (confined)
            return failWith("refusing to write to a special file:");
        const int fd = ::openat(dirFd, leaf.c_str(), O_WRONLY | O_TRUNC | O_CLOEXEC);
        if (fd < 0)
            return failWith("cannot write");
        FdStreamBuf buf(fd);
        std::ostream out(&buf);
        write(out);
        out.flush();
        const bool ok = out.good() && !buf.failed();
        return ::close(fd) == 0 && ok ? true : failWith("failed while writing");
    }
    const mode_t perms = exists && S_ISREG(existing.st_mode) ? (existing.st_mode & 07777)
                                                              : static_cast<mode_t>(0666 & ~currentUmask());

    // ".lt-<random>": short (independent of the file name length), hidden, created exclusively.
    std::random_device rd;
    std::string tmp;
    int fd = -1;
    for (int attempt = 0; attempt < 16 && fd < 0; ++attempt) {
        char name[32];
        std::snprintf(name, sizeof name, ".lt-%08x%08x", rd(), rd());
        tmp = name;
        fd = ::openat(dirFd, tmp.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        if (fd < 0 && errno != EEXIST)
            break;
    }
    if (fd < 0)
        return failWith("cannot write");
    ::fchmod(fd, perms);

    bool ok = false;
    try {
        FdStreamBuf buf(fd);
        std::ostream out(&buf);
        write(out);
        out.flush();
        ok = out.good() && !buf.failed() && ::fsync(fd) == 0;
    } catch (...) {
        ::close(fd);
        ::unlinkat(dirFd, tmp.c_str(), 0);
        throw;
    }
    const bool closed = ::close(fd) == 0;
    if (!ok || !closed) {
        ::unlinkat(dirFd, tmp.c_str(), 0);
        return failWith("failed while writing");
    }
    if (::renameat(dirFd, tmp.c_str(), dirFd, leaf.c_str()) != 0) {
        ::unlinkat(dirFd, tmp.c_str(), 0);
        return failWith("cannot replace");
    }
    return true;
}

#else
// Windows: the same contract with Win32 primitives. The temporary sibling is created exclusively ("x"),
// written in binary (LF line ends on every platform), flushed to disk and moved over the target with
// MoveFileExW(MOVEFILE_REPLACE_EXISTING). In confined mode no component of the directory and not the target
// itself may be a symlink or junction (checked just before writing; Windows has no openat/O_NOFOLLOW walk).
class FileStreamBuf : public std::streambuf {
public:
    explicit FileStreamBuf(std::FILE* f) : m_file(f) { setp(m_buf, m_buf + sizeof m_buf); }
    [[nodiscard]] bool failed() const { return m_failed; }

protected:
    int_type overflow(int_type ch) override
    {
        if (!flushBuffer())
            return traits_type::eof();
        if (!traits_type::eq_int_type(ch, traits_type::eof())) {
            *pptr() = traits_type::to_char_type(ch);
            pbump(1);
        }
        return traits_type::not_eof(ch);
    }
    int sync() override { return flushBuffer() ? 0 : -1; }

private:
    bool flushBuffer()
    {
        const auto n = static_cast<std::size_t>(pptr() - pbase());
        if (n > 0 && !m_failed && std::fwrite(pbase(), 1, n, m_file) != n)
            m_failed = true;
        setp(m_buf, m_buf + sizeof m_buf);
        return !m_failed;
    }

    std::FILE* m_file;
    bool m_failed = false;
    char m_buf[1 << 16];
};

bool isPlainDirectoryChain(const std::filesystem::path& dir)
{
    namespace fs = std::filesystem;
    std::error_code ec;
    for (fs::path p = dir; !p.empty(); p = p.parent_path()) {
        const auto st = fs::symlink_status(p, ec);
        if (ec || st.type() != fs::file_type::directory)
            return false;
        if (p == p.root_path() || p == p.parent_path())
            break;
    }
    return true;
}

template <typename Fn>
bool writeFileChecked(const std::string& path, std::ios::openmode mode, std::string* error, Fn&& write,
                      bool confined = false)
{
    (void)mode;  // always binary: identical bytes on every platform
    namespace fs = std::filesystem;
    auto failWith = [&](const std::string& msg) {
        if (error)
            *error = msg + " " + path;
        return false;
    };
    const fs::path target = pathFromUtf8(path);
    const std::string leaf = utf8FromPath(target.filename());
    if (leaf.empty() || leaf == "." || leaf == "..")
        return failWith("not a file name:");
    const fs::path dir = target.parent_path().empty() ? fs::path(".") : target.parent_path();
    std::error_code ec;
    if (confined && (!dir.is_absolute() || !isPlainDirectoryChain(dir)))
        return failWith("output directory changed or is not reachable without symlinks:");
    const auto existing = fs::symlink_status(target, ec);
    const bool exists = !ec && existing.type() != fs::file_type::not_found;
    if (exists && confined && existing.type() != fs::file_type::regular)
        return failWith("refusing to write to a special file or link:");
    if (exists && existing.type() != fs::file_type::regular && existing.type() != fs::file_type::symlink) {
        std::FILE* f = _wfopen(target.c_str(), L"wb");  // a device such as NUL: write in place
        if (!f)
            return failWith("cannot write");
        FileStreamBuf buf(f);
        std::ostream out(&buf);
        write(out);
        out.flush();
        const bool ok = out.good() && !buf.failed();
        return std::fclose(f) == 0 && ok ? true : failWith("failed while writing");
    }

    std::random_device rd;
    fs::path tmp;
    std::FILE* f = nullptr;
    for (int attempt = 0; attempt < 16 && !f; ++attempt) {
        char name[32];
        std::snprintf(name, sizeof name, ".lt-%08x%08x", rd(), rd());
        tmp = dir / name;
        f = _wfopen(tmp.c_str(), L"wbx");
        if (!f && errno != EEXIST)
            break;
    }
    if (!f)
        return failWith("cannot write");

    bool ok = false;
    try {
        FileStreamBuf buf(f);
        std::ostream out(&buf);
        write(out);
        out.flush();
        ok = out.good() && !buf.failed() && std::fflush(f) == 0 && _commit(_fileno(f)) == 0;
    } catch (...) {
        std::fclose(f);
        fs::remove(tmp, ec);
        throw;
    }
    const bool closed = std::fclose(f) == 0;
    if (!ok || !closed) {
        fs::remove(tmp, ec);
        return failWith("failed while writing");
    }
    if (!MoveFileExW(tmp.c_str(), target.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        fs::remove(tmp, ec);
        return failWith("cannot replace");
    }
    return true;
}

#endif

}  // namespace

nlohmann::json toJson(const DeviceInfo& info)
{
    nlohmann::json j = {{"port", info.port},
                        {"serial_number", info.serialNumber},
                        {"version", info.version.raw},
                        {"board", info.version.board},
                        {"firmware_family", std::string(toString(info.version.family))},
                        {"firmware_version", info.version.versionString()},
                        {"protocol", info.generation == ProtocolGeneration::V6 ? "v6" : "v5"},
                        {"max_frequency_hz", info.maxFrequency},
                        {"min_frequency_hz", info.minFrequency()},
                        {"blast_frequency_hz", info.blastFrequency},
                        {"buffer_bytes", info.bufferSize},
                        {"channels", info.channels},
                        {"edge_sensitive_trigger", info.supportsEdgeTrigger()},
                        {"segment_trigger_times", info.supportsEdgeBurstTimestamps()},
                        {"clock_domain", info.dutClock ? "dut" : "async"},
                        {"max_samples",
                         {{"8ch", info.limits(CaptureMode::Channels8).maxTotalSamples},
                          {"16ch", info.limits(CaptureMode::Channels16).maxTotalSamples},
                          {"24ch", info.limits(CaptureMode::Channels24).maxTotalSamples}}},
                        // Immediate captures always reserve the firmware's 2 mandatory pre-trigger samples.
                        {"max_immediate_samples",
                         {{"8ch", info.limits(CaptureMode::Channels8).maxPostSamples},
                          {"16ch", info.limits(CaptureMode::Channels16).maxPostSamples},
                          {"24ch", info.limits(CaptureMode::Channels24).maxPostSamples}}}};

    if (!info.version.board.empty() && gpioForChannel(info.version.board, 0)) {
        nlohmann::json gpios = nlohmann::json::object();
        for (uint8_t ch = 0; ch < info.channels; ++ch) {
            if (auto gp = gpioForChannel(info.version.board, ch))
                gpios[std::to_string(ch + 1)] = *gp;
        }
        if (auto ext = gpioForChannel(info.version.board, info.channels))
            gpios["ext_trigger"] = *ext;
        j["gpio_map"] = std::move(gpios);
    }
    return j;
}

nlohmann::json toJson(const ChannelStats& st, bool includeEdges, std::string_view board)
{
    std::optional<double> minW, maxW;
    for (const auto& w : {st.highWidth, st.lowWidth}) {
        if (!w)
            continue;
        const double mn = st.samplesToSeconds(static_cast<double>(w->minSamples)) * kUs;
        const double mx = st.samplesToSeconds(static_cast<double>(w->maxSamples)) * kUs;
        minW = minW ? std::min(*minW, mn) : mn;
        maxW = maxW ? std::max(*maxW, mx) : mx;
    }

    nlohmann::json j = {
        {"channel", st.channel.displayNumber()},
        {"name", st.channel.label()},
        {"samples", st.sampleCount},
        {"idle_level", st.idleLevel},
        {"final_level", st.finalLevel},
        {"transitions", st.transitions},
        {"rising_edges", st.risingEdges},
        {"falling_edges", st.fallingEdges},
        {"pulse_count_high", st.pulseCountHigh},
        {"pulse_count_low", st.pulseCountLow},
        {"min_pulse_width_us", optJson(minW)},
        {"max_pulse_width_us", optJson(maxW)},
        {"high_pulse_width_us", widthJson(st.highWidth, st)},
        {"low_pulse_width_us", widthJson(st.lowWidth, st)},
        {"mean_frequency_hz", optJson(st.meanFrequencyHz)},
        {"duty_cycle_percent", optJson(st.dutyCyclePercent)},
        {"high_time_percent", st.highTimePercent},
        {"trailing_level_us", st.samplesToSeconds(static_cast<double>(st.trailingSamples)) * kUs},
        {"glitch_detection", {{"detected", st.glitchCount > 0}, {"count", st.glitchCount}}},
        {"first_edge_us", optJson(st.firstEdgeTimeS, kUs)},
        {"last_edge_us", optJson(st.lastEdgeTimeS, kUs)},
    };
    if (st.period) {
        const auto& p = *st.period;
        const double ns = st.sampleRateHz > 0 ? 1e9 / st.sampleRateHz : 0.0;
        j["periodicity"] = {
            {"kind", std::string(toString(p.kind))},
            {"edge", p.fromRising ? "rising" : "falling"},
            {"periods", p.count},
            {"min_samples", p.minSamples},
            {"median_samples", p.medianSamples},
            {"max_samples", p.maxSamples},
            {"median_period_ns", p.medianSamples * ns},
            {"cv", p.cv},
            {"bursts", p.burstCount},
            {"burst_gap_threshold_samples", p.burstGapSamples},
            {"min_gap_samples", p.minGapSamples},
            {"max_gap_samples", p.maxGapSamples},
            {"irregular_periods", p.irregularCount},
            {"irregular_edge_samples", p.irregularSamples},
            {"note", "frequency_hz and duty_cycle_percent use intra-burst periods only"},
        };
    }
    if (!st.glitchSamples.empty())
        j["glitch_detection"]["start_samples"] = st.glitchSamples;
    if (!board.empty()) {
        if (auto gp = gpioForChannel(board, st.channel.number))
            j["gpio"] = *gp;
    }
    if (includeEdges) {
        // Level alternates on every edge, starting from idle_level.
        auto times = nlohmann::json::array();
        for (uint32_t idx : st.edges)
            times.push_back(st.timeOfSample(idx) * kUs);
        j["edges_note"] = "level toggles at each edge, starting from idle_level";
        j["edge_samples"] = st.edges;
        j["edge_times_us"] = std::move(times);
    }
    return j;
}

nlohmann::json toJson(const LatencyStats& lat, bool includeEvents)
{
    nlohmann::json j = {
        {"stimulus",
         {{"channel", lat.stimChannel.displayNumber()},
          {"name", lat.stimChannel.label()},
          {"edge", lat.stimFalling ? "falling" : "rising"}}},
        {"reaction",
         {{"channel", lat.reactChannel.displayNumber()},
          {"name", lat.reactChannel.label()},
          {"edge", lat.reactFalling ? "falling" : "rising"}}},
        {"sample_rate_hz", lat.sampleRateHz},
        {"quantization_step_ns", lat.quantizationStepNs},
        {"count", lat.count},
        {"unmatched_stimulus_count", lat.unmatchedStimCount},
        {"min_ns", lat.count ? nlohmann::json(lat.minNs) : nlohmann::json(nullptr)},
        {"max_ns", lat.count ? nlohmann::json(lat.maxNs) : nlohmann::json(nullptr)},
        {"mean_ns", lat.count ? nlohmann::json(lat.meanNs) : nlohmann::json(nullptr)},
        {"stdev_ns", lat.count ? nlohmann::json(lat.stdevNs) : nlohmann::json(nullptr)},
        {"p50_ns", lat.count ? nlohmann::json(lat.p50Ns) : nlohmann::json(nullptr)},
        {"p90_ns", lat.count ? nlohmann::json(lat.p90Ns) : nlohmann::json(nullptr)},
        {"p95_ns", lat.count ? nlohmann::json(lat.p95Ns) : nlohmann::json(nullptr)},
        {"p99_ns", lat.count ? nlohmann::json(lat.p99Ns) : nlohmann::json(nullptr)},
        {"censored_stimulus_count", lat.censoredStimCount},
        {"censored_reaction_count", lat.censoredReactCount},
        {"spurious_reaction_count", lat.spuriousReactCount},
        {"max_reactions_per_stimulus", lat.maxReactsPerStim},
        {"coincident_count", lat.coincidentCount},
        {"censor_window_samples", lat.censorWindowSamples},
        {"percentile_method", "nearest_rank"},
        {"exact_timing", lat.exact},
        {"uncertainty_note", lat.exact ? "in-design capture: a delay of d samples is exactly d*T (DUT clock cycles)"
                                       : "true delay of a pair measured as d samples lies in ((d-1)*T, (d+1)*T)"},
        {"outlier_count", lat.outlierCount},
    };
    if (lat.count) {
        auto hist = nlohmann::json::array();
        for (const auto& [d, c] : lat.histogram)
            hist.push_back({{"delta_samples", d}, {"count", c}});
        j["histogram"] = std::move(hist);
        j["samples"] = {{"min", lat.minSamples}, {"p50", lat.p50Samples}, {"p90", lat.p90Samples},
                        {"p95", lat.p95Samples}, {"p99", lat.p99Samples}, {"max", lat.maxSamples}};
        j["bounds_ns"] = {{"min_lb", lat.lowerBoundNs(lat.minSamples)}, {"min_ub", lat.upperBoundNs(lat.minSamples)},
                          {"p50_ub", lat.upperBoundNs(lat.p50Samples)}, {"p99_ub", lat.upperBoundNs(lat.p99Samples)},
                          {"max_lb", lat.lowerBoundNs(lat.maxSamples)}, {"max_ub", lat.upperBoundNs(lat.maxSamples)}};
        auto outl = nlohmann::json::array();
        j["constant_delay_hypothesis"] = {
            {"consistent", lat.constantDelayConsistent()},
            {"interval_ns",
             lat.constantDelayConsistent()
                 ? nlohmann::json::array({lat.constantDelayLowerNs(), lat.constantDelayUpperNs()})
                 : nlohmann::json(nullptr)},
            {"note", lat.exact ? (lat.constantDelayConsistent()
                                      ? "exact timing: every pair has this same delay"
                                      : "exact timing: the delay differs between pairs (real variation)")
                               : (lat.constantDelayConsistent()
                                      ? "all pairs are explained by one fixed delay in this open interval; histogram "
                                        "spread is sampling phase, not jitter"
                                      : "spread exceeds 1 sample: the true delay varies (real jitter)")}};
        for (const auto& ev : lat.outliers) {
            nlohmann::json o = {{"event_id", ev.eventId}, {"stim_sample", ev.stimSample},
                                {"react_sample", ev.reactSample}, {"delta_samples", ev.deltaSamples},
                                {"delta_ns", ev.deltaNs}};
            if (ev.segment)
                o["segment"] = *ev.segment;
            outl.push_back(std::move(o));
        }
        j["outliers"] = std::move(outl);
    }
    if (lat.reversed)
        j["direction_warning"] = {{"likely_swapped", true},
                                  {"stim_period_samples", lat.reversed->stimPeriodSamples},
                                  {"reverse_p50_samples", lat.reversed->reverseP50Samples},
                                  {"reverse_pairs", lat.reversed->reverseCount},
                                  {"message", SignalAnalyzer::reversedHint(lat)}};
    j["drop_stim_samples"] = lat.dropSamples;
    j["spurious_react_samples"] = lat.spuriousSamples;
    if (includeEvents) {
        auto evs = nlohmann::json::array();
        for (const auto& ev : lat.events) {
            evs.push_back({{"event_id", ev.eventId},
                           {"stim_sample", ev.stimSample},
                           {"stim_time_ns", ev.stimTimeNs},
                           {"react_sample", ev.reactSample},
                           {"react_time_ns", ev.reactTimeNs},
                           {"delta_samples", ev.deltaSamples},
                           {"delta_ns", ev.deltaNs}});
            if (ev.segment)
                evs.back()["segment"] = *ev.segment;
        }
        j["events"] = std::move(evs);
    }
    return j;
}

nlohmann::json captureToJson(const CaptureBuffer& buffer, const std::vector<ChannelStats>& stats, bool includeEdges,
                             std::string_view board)
{
    auto channels = nlohmann::json::array();
    for (const auto& st : stats)
        channels.push_back(toJson(st, includeEdges, board));
    const double duration = static_cast<double>(buffer.size()) * buffer.samplePeriodSeconds();
    nlohmann::json j = {{"schema_version", kCaptureSchemaVersion},
            {"format", "logictracer-capture-v1"},
            {"captured_at_utc", utcNowIso8601()},
            {"sample_rate_hz", buffer.frequency()},
            {"sample_period_us", buffer.samplePeriodSeconds() * kUs},
            {"sample_period_ns", buffer.samplePeriodSeconds() * 1e9},
            {"samples", buffer.size()},
            {"pre_trigger_samples", buffer.preSamples()},
            {"trigger_sample", buffer.triggerIndex()},
            {"duration_us", duration * kUs},
            {"time_origin", "trigger (t=0 at sample index trigger_sample)"},
            {"channels", std::move(channels)}};
    if (buffer.exactTiming())
        j["exact_timing"] = true;  // in-design capture: sample instants are the DUT clock edges
    return j;
}

void writeVcd(std::ostream& out, const CaptureBuffer& buffer)
{
    const uint32_t f = buffer.frequency();
    const bool ns = f > 0 && 1'000'000'000u % f == 0;
    const double unitsPerSample = f == 0 ? 1.0 : (ns ? 1e9 : 1e12) / f;
    auto timeOf = [&](std::size_t i) { return static_cast<uint64_t>(std::llround(i * unitsPerSample)); };

    out << "$comment logictracer-cli capture: " << f << " Hz, " << buffer.size() << " samples, trigger at #"
        << timeOf(buffer.preSamples()) << " $end\n";
    out << "$comment logictracer-channels";  // hardware channel numbers (1-based) in declaration order
    for (std::size_t i = 0; i < buffer.channels().size(); ++i)
        out << (i == 0 ? " " : ",") << buffer.channels()[i].displayNumber();
    out << " $end\n";
    out << "$timescale 1 " << (ns ? "ns" : "ps") << " $end\n";
    out << "$scope module logic $end\n";
    const auto& chans = buffer.channels();
    for (std::size_t i = 0; i < chans.size(); ++i)
        out << "$var wire 1 " << vcdId(i) << ' ' << vcdName(chans[i]) << " $end\n";
    out << "$upscope $end\n$enddefinitions $end\n";
    if (buffer.size() == 0)
        return;

    out << "#0\n$dumpvars\n";
    for (std::size_t c = 0; c < chans.size(); ++c)
        out << (buffer.bit(0, c) ? '1' : '0') << vcdId(c) << '\n';
    out << "$end\n";

    const auto& s = buffer.samples();
    for (std::size_t i = 1; i < s.size(); ++i) {
        const uint32_t diff = s[i] ^ s[i - 1];
        if (!diff)
            continue;
        out << '#' << timeOf(i) << '\n';
        for (std::size_t c = 0; c < chans.size(); ++c)
            if ((diff >> c) & 1u)
                out << (((s[i] >> c) & 1u) ? '1' : '0') << vcdId(c) << '\n';
    }
    out << '#' << timeOf(s.size()) << '\n';
}

bool writeVcdFile(const std::string& path, const CaptureBuffer& buffer, std::string* error, bool confined)
{
    return writeFileChecked(path, std::ios::binary, error, [&](std::ostream& f) { writeVcd(f, buffer); }, confined);
}

bool writeJsonFile(const std::string& path, const nlohmann::json& doc, std::string* error, bool confined)
{
    return writeFileChecked(path, {}, error, [&](std::ostream& f) { f << doc.dump(2) << '\n'; }, confined);
}

void writeCsv(std::ostream& out, const CaptureBuffer& buffer)
{
    out << "SampleIndex,TimeRelativeNs";
    for (const auto& ch : buffer.channels())
        out << ',' << vcdName(ch);
    out << '\n';

    const double dtNs = buffer.samplePeriodSeconds() * 1e9;
    const int64_t trig = buffer.triggerIndex();
    for (std::size_t i = 0; i < buffer.size(); ++i) {
        const auto relNs = static_cast<int64_t>(std::llround((static_cast<double>(i) - static_cast<double>(trig)) * dtNs));
        out << i << ',' << relNs;
        for (std::size_t c = 0; c < buffer.channels().size(); ++c)
            out << ',' << (buffer.bit(i, c) ? '1' : '0');
        out << '\n';
    }
}

bool writeCsvFile(const std::string& path, const CaptureBuffer& buffer, std::string* error, bool confined)
{
    return writeFileChecked(path, {}, error, [&](std::ostream& f) { writeCsv(f, buffer); }, confined);
}

void writeEventsCsv(std::ostream& out, const std::vector<LatencyEvent>& events)
{
    // Segmented captures add the 1-based window; samples and times are then relative to that window.
    const bool segmented = !events.empty() && events.front().segment.has_value();
    out << "event_id,stim_sample,stim_time_ns,react_sample,react_time_ns,delta_samples,delta_ns"
        << (segmented ? ",segment\n" : "\n");
    out << std::fixed << std::setprecision(3);
    for (const auto& ev : events) {
        out << ev.eventId << ',' << ev.stimSample << ',' << ev.stimTimeNs << ',' << ev.reactSample << ','
            << ev.reactTimeNs << ',' << ev.deltaSamples << ',' << ev.deltaNs;
        if (segmented)
            out << ',' << ev.segment.value_or(0);
        out << '\n';
    }
}

bool writeEventsCsvFile(const std::string& path, const std::vector<LatencyEvent>& events, std::string* error,
                        bool confined)
{
    return writeFileChecked(path, {}, error, [&](std::ostream& f) { writeEventsCsv(f, events); }, confined);
}

namespace {

// Unsigned sample indices beyond this cannot come from a real capture and break time-axis arithmetic.
constexpr int64_t kMaxTriggerOffset = int64_t{UINT32_MAX};

bool readCaptureJsonImpl(const nlohmann::json& doc, LoadedCapture& out, std::string* error)
{
    auto fail = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
        return false;
    };
    if (!doc.is_object())
        return fail("JSON document is not an object");

    // Support legacy C# .lac session files (Settings.CaptureChannels[].Samples Base64 byte arrays).
    if (doc.contains("Settings") && doc["Settings"].is_object()) {
        const auto& s = doc["Settings"];
        if (!s.contains("Frequency") || !s.contains("CaptureChannels") || !s["CaptureChannels"].is_array())
            return fail("invalid .lac Settings object");
        const auto freqValue = integerIn(s["Frequency"], 1, UINT32_MAX);
        if (!freqValue)
            return fail("invalid Frequency in .lac file");
        const uint64_t freq = static_cast<uint64_t>(*freqValue);
        const auto preValue = s.contains("PreTriggerSamples") ? integerIn(s["PreTriggerSamples"], -kMaxTriggerOffset, kMaxTriggerOffset)
                                                              : std::optional<int64_t>(0);
        if (!preValue)
            return fail("PreTriggerSamples is out of range in .lac file");
        const int64_t pre = *preValue;
        const auto& chArr = s["CaptureChannels"];
        if (freq == 0 || freq > UINT32_MAX || chArr.empty() || chArr.size() > 32)
            return fail("invalid frequency or channel count in .lac file");

        std::vector<ChannelInfo> channels;
        std::vector<std::vector<uint8_t>> decoded(chArr.size());
        std::size_t sampleCount = 0;
        for (std::size_t c = 0; c < chArr.size(); ++c) {
            const auto& cj = chArr[c];
            int64_t hw = static_cast<int64_t>(c);
            if (cj.is_object() && cj.contains("ChannelNumber")) {
                const auto n = integerIn(cj["ChannelNumber"], -1'000'000, 1'000'000);
                if (!n)
                    return fail("invalid ChannelNumber in .lac channel");
                hw = *n;
            }
            std::string name = cj.is_object() && cj.contains("ChannelName") && cj["ChannelName"].is_string()
                                   ? cj["ChannelName"].get<std::string>()
                                   : std::string{};
            channels.push_back({static_cast<uint8_t>(std::clamp<int64_t>(hw, 0, 31)), std::move(name)});
            if (!cj.contains("Samples") || !cj["Samples"].is_string())
                return fail("missing Base64 Samples in .lac channel");
            const auto& b64 = cj["Samples"].get_ref<const std::string&>();
            decoded[c] = decodeBase64(b64);
            const auto len = static_cast<std::size_t>(decoded[c].size());
            sampleCount = (c == 0) ? len : std::min(sampleCount, len);
        }
        if (sampleCount > 16'777'216)
            return fail(".lac sample count exceeds 16M limit");

        std::vector<uint32_t> samples(sampleCount, 0u);
        for (std::size_t c = 0; c < chArr.size(); ++c) {
            const uint8_t* raw = decoded[c].data();
            const uint32_t mask = 1u << c;
            for (std::size_t i = 0; i < sampleCount; ++i) {
                if (raw[i] != 0)
                    samples[i] |= mask;
            }
        }
        out = {};
        out.buffer = CaptureBuffer(std::move(channels), static_cast<uint32_t>(freq), 0, std::move(samples));
        out.buffer.setTriggerIndex(pre);
        const auto trigType = s.contains("TriggerType") ? integerIn(s["TriggerType"], 0, 1000) : std::optional<int64_t>(0);
        const auto trigChannel = s.contains("TriggerChannel") ? integerIn(s["TriggerChannel"], 0, 1'000'000) : std::nullopt;
        if (trigType == 0 && trigChannel) {
            out.triggerDescription = "CH" + std::to_string(*trigChannel + 1) + " " +
                                     (s.contains("TriggerInverted") && s["TriggerInverted"].is_boolean() &&
                                              s["TriggerInverted"].get<bool>()
                                          ? "falling"
                                          : "rising");
        }
        return true;
    }

    const nlohmann::json& cap =
        (doc.contains("capture") && doc["capture"].is_object()) ? doc["capture"] : doc;
    if (!cap.contains("sample_rate_hz") || !cap["sample_rate_hz"].is_number() ||
        !cap.contains("samples") || !cap["samples"].is_number_unsigned() ||
        !cap.contains("channels") || !cap["channels"].is_array()) {
        return fail("JSON does not contain a valid capture object (missing sample_rate_hz/samples/channels)");
    }

    const auto rateValue = integerIn(cap["sample_rate_hz"], 1, UINT32_MAX);
    if (!rateValue)
        return fail("invalid sample_rate_hz in capture JSON");
    const uint64_t rate = static_cast<uint64_t>(*rateValue);
    const std::size_t sampleCount = cap["samples"].get<std::size_t>();
    if (sampleCount > 16'777'216)
        return fail("capture sample count exceeds 16M limit");
    if (cap["channels"].empty() || cap["channels"].size() > 32)
        return fail("capture must contain 1..32 channels");

    int64_t triggerSample = 0;
    if (cap.contains("trigger_sample") && cap["trigger_sample"].is_number_integer()) {
        const auto t = integerIn(cap["trigger_sample"], INT64_MIN, INT64_MAX);
        if (!t)
            return fail("trigger_sample is out of range in capture JSON");
        triggerSample = *t;
    } else if (cap.contains("pre_trigger_samples")) {
        const auto t = integerIn(cap["pre_trigger_samples"], 0, INT64_MAX);
        if (!t)
            return fail("pre_trigger_samples is out of range in capture JSON");
        triggerSample = *t;
    }

    std::vector<ChannelInfo> channels;
    std::vector<uint32_t> samples(sampleCount, 0u);
    for (std::size_t c = 0; c < cap["channels"].size(); ++c) {
        const auto& cj = cap["channels"][c];
        if (!cj.is_object() || !cj.contains("idle_level") || !cj.contains("edge_samples") ||
            !cj["edge_samples"].is_array()) {
            return fail("channel entry missing idle_level or lossless edge_samples[] (re-save with --out-json)");
        }
        int64_t chNum1 = static_cast<int64_t>(c + 1);
        if (cj.contains("channel")) {
            const auto n = integerIn(cj["channel"], -1'000'000, 1'000'000);
            if (!n)
                return fail("invalid channel number in capture JSON");
            chNum1 = *n;
        }
        const uint8_t hwNum = static_cast<uint8_t>(std::clamp<int64_t>(chNum1 - 1, 0, 31));
        std::string name = cj.value("name", std::string{});
        if (name == "CH" + std::to_string(hwNum + 1))
            name.clear();
        channels.push_back({hwNum, std::move(name)});

        std::vector<uint32_t> edges;
        edges.reserve(cj["edge_samples"].size());
        for (const auto& e : cj["edge_samples"]) {
            if (!e.is_number_unsigned())
                return fail("edge_samples must contain unsigned sample indices");
            const uint64_t idx = e.get<uint64_t>();
            if (idx > 0 && idx < sampleCount)
                edges.push_back(static_cast<uint32_t>(idx));
        }
        std::sort(edges.begin(), edges.end());

        const auto idle = cj["idle_level"].is_boolean() ? std::optional<int64_t>(cj["idle_level"].get<bool>() ? 1 : 0)
                                                        : integerIn(cj["idle_level"], 0, 1);
        if (!idle)
            return fail("idle_level must be 0 or 1");
        bool level = *idle != 0;
        std::size_t ePos = 0;
        const uint32_t bitMask = 1u << c;
        for (std::size_t i = 0; i < sampleCount; ++i) {
            while (ePos < edges.size() && edges[ePos] == i) {
                level = !level;
                ++ePos;
            }
            if (level)
                samples[i] |= bitMask;
        }
    }

    if (triggerSample < -kMaxTriggerOffset || triggerSample > kMaxTriggerOffset)
        return fail("trigger_sample is out of range in capture JSON");
    out = {};
    out.buffer = CaptureBuffer(std::move(channels), static_cast<uint32_t>(rate), 0, std::move(samples));
    out.buffer.setTriggerIndex(triggerSample);
    out.buffer.setExactTiming(cap.contains("exact_timing") && cap["exact_timing"].is_boolean() &&
                              cap["exact_timing"].get<bool>());

    if (doc.contains("trigger_description") && doc["trigger_description"].is_string()) {
        out.triggerDescription = doc["trigger_description"].get<std::string>();
    } else if (doc.contains("config") && doc["config"].is_object() && doc["config"].contains("trigger")) {
        const auto& tr = doc["config"]["trigger"];
        if (tr.is_object() && tr.value("type", "") == "edge") {
            const auto trChannel = tr.contains("channel") ? integerIn(tr["channel"], 1, 1'000'000) : std::optional<int64_t>(1);
            out.triggerDescription = "CH" + std::to_string(trChannel.value_or(1)) + " " +
                                     tr.value("edge", std::string("rising"));
        } else if (tr.is_object() && tr.value("type", "") == "pattern") {
            const auto trChannel = tr.contains("channel") ? integerIn(tr["channel"], 1, 16) : std::optional<int64_t>(1);
            const auto bitCount = tr.contains("bit_count") ? integerIn(tr["bit_count"], 1, 16) : std::optional<int64_t>(1);
            std::string desc = "CH" + std::to_string(trChannel.value_or(1));
            if (bitCount.value_or(1) > 1)
                desc += "-CH" + std::to_string(trChannel.value_or(1) + bitCount.value_or(1) - 1);
            desc += "==" + tr.value("pattern", std::string("1"));
            out.triggerDescription = std::move(desc);
        }
    }

    if (doc.contains("device") && doc["device"].is_object()) {  // provenance written by the CLI and the GUI
        const auto& dev = doc["device"];
        if (dev.contains("version") && dev["version"].is_string())
            out.deviceVersion = dev["version"].get<std::string>();
        if (dev.contains("board") && dev["board"].is_string())
            out.board = dev["board"].get<std::string>();
    }

    if (doc.contains("segments") && doc["segments"].is_object() && doc["segments"].contains("layout")) {
        std::string err;
        const auto layout = segmentLayoutFromJson(doc["segments"]["layout"], &err);
        if (!layout)
            out.warnings.push_back("segment layout ignored: " + err);
        else if (layout->totalSamples() != out.buffer.size())
            out.warnings.push_back("segment layout ignored: it describes " + std::to_string(layout->totalSamples()) +
                                   " samples, the capture has " + std::to_string(out.buffer.size()));
        else
            out.segments = layout;
    }
    if (doc.contains("setup"))
        out.setup = setupFromJson(doc["setup"]);
    else if (cap.contains("setup"))
        out.setup = setupFromJson(cap["setup"]);

    const nlohmann::json* notes = doc.contains("annotations") && doc["annotations"].is_array()
                                      ? &doc["annotations"]
                                      : (cap.contains("annotations") && cap["annotations"].is_array()
                                             ? &cap["annotations"]
                                             : nullptr);
    if (notes) {
        for (const auto& n : *notes) {
            if (!n.is_object())
                continue;
            Annotation a;
            a.id = n.contains("id") ? static_cast<uint64_t>(integerIn(n["id"], 0, INT64_MAX).value_or(0)) : 0;
            a.timeSeconds = n.value("time_us", 0.0) * 1e-6;
            a.durationSeconds = n.value("duration_us", 0.0) * 1e-6;
            a.label = n.value("label", std::string{});
            a.color = n.value("color", std::string("red"));
            a.author = n.value("author", std::string("ai"));
            if (!a.label.empty())
                out.annotations.push_back(std::move(a));
        }
    }
    return true;
}

}  // namespace

bool readCaptureJson(const nlohmann::json& doc, LoadedCapture& out, std::string* error)
{
    try {
        return readCaptureJsonImpl(doc, out, error);
    } catch (const nlohmann::json::exception& e) {
        out = {};
        if (error)
            *error = std::string("malformed capture JSON: ") + e.what();
        return false;
    }
}

nlohmann::json toJson(const SetupInfo& setup)
{
    nlohmann::json j = nlohmann::json::object();
    for (const auto& [k, v] : setup)
        j[k] = v;
    return j;
}

SetupInfo setupFromJson(const nlohmann::json& object)
{
    SetupInfo out;
    if (!object.is_object())
        return out;
    for (auto it = object.begin(); it != object.end(); ++it) {
        if (it.value().is_string() && cli::isValidSetupEntry(it.key(), it.value().get_ref<const std::string&>()))
            out.emplace_back(it.key(), it.value().get<std::string>());
    }
    return out;
}

bool readVcd(std::istream& in, LoadedCapture& out, std::string* error)
{
    auto fail = [&](std::string msg) {
        if (error)
            *error = std::move(msg);
        return false;
    };

    double timescaleSeconds = 1e-9;
    std::optional<uint32_t> commentFreq;
    std::optional<std::size_t> commentSamples;
    std::optional<uint64_t> commentTriggerTime;
    std::vector<uint8_t> commentChannels;  // hardware numbers written by logictracer, in $var order

    struct Var {
        std::string id;
        ChannelInfo ch;
    };
    std::vector<Var> vars;
    std::map<std::string, std::size_t> idToIndex;

    // Tokenize up to $enddefinitions $end.
    std::string tok;
    bool inDefinitions = true;
    while (inDefinitions && (in >> tok)) {
        if (tok == "$comment") {
            std::string comment;
            while (in >> tok && tok != "$end") {
                if (!comment.empty())
                    comment += ' ';
                comment += tok;
            }
            if (comment.starts_with("logictracer-channels ")) {
                std::string_view list = std::string_view(comment).substr(21);
                while (!list.empty()) {
                    int n = 0;
                    const auto [ptr, ec] = std::from_chars(list.data(), list.data() + list.size(), n);
                    if (ec != std::errc{} || n < 1 || n > 32)
                        break;
                    commentChannels.push_back(static_cast<uint8_t>(n - 1));
                    list.remove_prefix(static_cast<std::size_t>(ptr - list.data()));
                    if (!list.empty() && list.front() == ',')
                        list.remove_prefix(1);
                }
                continue;
            }
            // Match our own comment: "logictracer-cli capture: <f> Hz, <N> samples, trigger at #<T>"
            const auto hzPos = comment.find(" Hz, ");
            const auto smpPos = comment.find(" samples, trigger at #");
            if (hzPos != std::string::npos && smpPos != std::string::npos) {
                const auto colonPos = comment.rfind(':', hzPos);
                if (colonPos != std::string::npos) {
                    uint32_t f = 0;
                    const auto fStr = comment.substr(colonPos + 2, hzPos - (colonPos + 2));
                    std::from_chars(fStr.data(), fStr.data() + fStr.size(), f);
                    if (f > 0)
                        commentFreq = f;
                }
                std::size_t n = 0;
                const auto nStr = comment.substr(hzPos + 5, smpPos - (hzPos + 5));
                std::from_chars(nStr.data(), nStr.data() + nStr.size(), n);
                commentSamples = n;
                uint64_t tr = 0;
                const auto tStr = comment.substr(smpPos + 22);
                std::from_chars(tStr.data(), tStr.data() + tStr.size(), tr);
                commentTriggerTime = tr;
            }
        } else if (tok == "$timescale") {
            std::string ts;
            while (in >> tok && tok != "$end")
                ts += tok;
            double mult = 0.0;
            const char* tsEnd = ts.data() + ts.size();
            const auto [p, ec] = std::from_chars(ts.data(), tsEnd, mult);
            const std::string unit(p, tsEnd);
            double scale = 0.0;
            if (unit == "fs") scale = 1e-15;
            else if (unit == "ps") scale = 1e-12;
            else if (unit == "ns") scale = 1e-9;
            else if (unit == "us") scale = 1e-6;
            else if (unit == "ms") scale = 1e-3;
            else if (unit == "s") scale = 1.0;
            if (ec != std::errc{} || !std::isfinite(mult) || mult <= 0 || scale == 0.0)
                return fail("invalid $timescale '" + ts + "' in VCD header");
            timescaleSeconds = mult * scale;
        } else if (tok == "$var") {
            std::string type, widthStr, id, name;
            if (!(in >> type >> widthStr >> id >> name))
                return fail("truncated $var in VCD header");
            while (in >> tok && tok != "$end") {
                // ignore bit-select suffix like [0]
            }
            if (widthStr == "1" && vars.size() < 32 && !idToIndex.contains(id)) {
                const auto idx = vars.size();
                idToIndex[id] = idx;
                uint8_t chNum = idx < commentChannels.size() ? commentChannels[idx] : static_cast<uint8_t>(idx);
                if (idx < commentChannels.size() && name == "CH" + std::to_string(chNum + 1))
                    name.clear();  // the default label of that channel
                else if (idx >= commentChannels.size() && name.size() > 2 && (name.starts_with("CH") || name.starts_with("ch"))) {
                    int parsed = 0;
                    const auto [ptr, ec] = std::from_chars(name.data() + 2, name.data() + name.size(), parsed);
                    if (ec == std::errc{} && ptr == name.data() + name.size() && parsed >= 1 && parsed <= 32) {
                        chNum = static_cast<uint8_t>(parsed - 1);
                        name.clear();
                    }
                }
                vars.push_back({id, {chNum, name}});
            }
        } else if (tok == "$enddefinitions") {
            while (in >> tok && tok != "$end") {}
            inDefinitions = false;
        }
    }

    if (inDefinitions || vars.empty())
        return fail("VCD contains no 1-bit wire/reg signals");

    struct Change {
        uint64_t time = 0;
        std::size_t ch = 0;
        bool val = false;
        bool known = true;  // false for x/z: the level is unknown and must not be guessed
    };
    std::vector<Change> changes;
    std::size_t unknownLevels = 0;
    uint64_t currentTime = 0;
    uint64_t maxTime = 0;

    while (in >> tok) {
        if (tok.starts_with('$')) {
            if (tok == "$comment") {
                while (in >> tok && tok != "$end") {}
            }
            continue;
        }
        if (tok.front() == '#') {
            uint64_t t = 0;
            std::from_chars(tok.data() + 1, tok.data() + tok.size(), t);
            currentTime = t;
            maxTime = std::max(maxTime, t);
            continue;
        }
        const char v = tok.front();
        if (v == '0' || v == '1' || v == 'x' || v == 'X' || v == 'z' || v == 'Z') {
            const std::string id = tok.substr(1);
            if (auto it = idToIndex.find(id); it != idToIndex.end())
{
                const bool known = v == '0' || v == '1';
                unknownLevels += !known;
                changes.push_back({currentTime, it->second, v == '1', known});
            }
        } else if (v == 'b' || v == 'B' || v == 'r' || v == 'R') {
            std::string ignoredId;
            in >> ignoredId;
        }
    }

    uint32_t frequency = 0;
    std::size_t totalSamples = 0;
    int64_t triggerSample = 0;
    double unitsPerSample = 1.0;  // VCD time units per output sample
    std::vector<std::string> warnings;

    if (commentFreq && commentSamples) {
        frequency = *commentFreq;
        totalSamples = std::min<std::size_t>(*commentSamples, 16'777'216);
        unitsPerSample = 1.0 / (static_cast<double>(frequency) * timescaleSeconds);
        if (commentTriggerTime && unitsPerSample > 0) {
            const double position = static_cast<double>(*commentTriggerTime) / unitsPerSample;
            if (position > static_cast<double>(kMaxTriggerOffset))
                warnings.push_back("the trigger position in the VCD comment is out of range and was ignored");
            else
                triggerSample = static_cast<int64_t>(std::llround(position));
        }
    } else {
        uint64_t stepUnits = 0;
        uint64_t prevT = 0;
        for (const auto& c : changes) {
            if (c.time > prevT) {
                stepUnits = stepUnits == 0 ? (c.time - prevT) : std::gcd(stepUnits, c.time - prevT);
                prevT = c.time;
            }
        }
        if (stepUnits == 0)
            stepUnits = maxTime > 0 ? maxTime : 1;
        const uint64_t exactStep = stepUnits;
        while (maxTime / stepUnits > 1'048'576)
            stepUnits *= 2;
        if (stepUnits != exactStep)
            warnings.push_back("VCD is longer than 1M time steps: the time base was coarsened " +
                               std::to_string(stepUnits / exactStep) + "x, so short pulses may be lost");
        const double periodS = static_cast<double>(stepUnits) * timescaleSeconds;
        if (periodS <= 0)
            return fail("VCD has no usable $timescale");
        const double rate = 1.0 / periodS;
        if (rate > 2e9)
            return fail("VCD time resolution is too fine (" + formatNumber(rate, 3) +
                        " Hz > 2 GHz): re-export it with a coarser timescale");
        if (rate < 1.0)
            return fail("VCD time base is slower than 1 Hz (" + formatNumber(rate, 3) +
                        " Hz): re-export it with a finer timescale");
        frequency = static_cast<uint32_t>(std::round(rate));
        unitsPerSample = static_cast<double>(stepUnits);
        // The last change at #maxTime is itself a sample.
        totalSamples = changes.empty() ? 0 : static_cast<std::size_t>(maxTime / stepUnits) + 1;
    }

    std::vector<ChannelInfo> channels;
    channels.reserve(vars.size());
    for (const auto& v : vars)
        channels.push_back(v.ch);

    std::vector<uint32_t> samples(totalSamples, 0u);
    if (totalSamples > 0) {
        uint32_t currentWord = 0;
        std::size_t cIdx = 0;
        for (std::size_t s = 0; s < totalSamples; ++s) {
            const auto sampleTime = static_cast<uint64_t>(std::llround(static_cast<double>(s) * unitsPerSample));
            while (cIdx < changes.size() && changes[cIdx].time <= sampleTime) {
                const uint32_t mask = 1u << changes[cIdx].ch;
                if (!changes[cIdx].known) {
                    ++cIdx;  // keep the previous level
                    continue;
                }
                if (changes[cIdx].val)
                    currentWord |= mask;
                else
                    currentWord &= ~mask;
                ++cIdx;
            }
            samples[s] = currentWord;
        }
    }

    if (unknownLevels > 0)
        warnings.push_back(std::to_string(unknownLevels) +
                           " x/z value change(s) in the VCD were treated as 'level unchanged' (initial unknown = 0)");
    out = {};
    out.buffer = CaptureBuffer(std::move(channels), frequency, 0, std::move(samples));
    out.buffer.setTriggerIndex(triggerSample);
    out.warnings = std::move(warnings);
    return true;
}

bool loadCaptureFile(const std::string& path, LoadedCapture& out, std::string* error)
{
    std::ifstream f(pathFromUtf8(path), std::ios::binary);
    if (!f) {
        if (error)
            *error = "cannot open " + path;
        return false;
    }

    // Peek the first non-whitespace byte: '{' => JSON, otherwise VCD.
    char first = '\0';
    while (f.get(first) && std::isspace(static_cast<unsigned char>(first))) {}
    f.clear();
    f.seekg(0);

    if (first == '{') {
        const auto doc = nlohmann::json::parse(f, nullptr, false);
        if (doc.is_discarded()) {
            if (error)
                *error = "invalid JSON in " + path;
            return false;
        }
        return readCaptureJson(doc, out, error);
    }
    return readVcd(f, out, error);
}

std::optional<std::string> resolveSafeOutputPath(const std::string& rawPath,
                                                 std::span<const std::string_view> allowedExtensions,
                                                 const std::string& sandboxDir,
                                                 std::string* error)
{
    namespace fs = std::filesystem;
    auto fail = [&](std::string msg) -> std::optional<std::string> {
        if (error)
            *error = std::move(msg);
        return std::nullopt;
    };
    if (rawPath.empty())
        return std::string{};

    const fs::path input = pathFromUtf8(rawPath);
    for (const auto& part : input) {
        const std::string p = utf8FromPath(part);
        if (p == "..")
            return fail("output path must not contain '..': " + rawPath);
        if (p.size() > 1 && p.front() == '.')
            return fail("output path must not target hidden files or dot-directories: " + rawPath);
    }

    std::string ext = utf8FromPath(input.extension());
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) { return std::tolower(c); });
    bool extOk = allowedExtensions.empty();
    for (std::string_view allowed : allowedExtensions) {
        if (ext == allowed) {
            extOk = true;
            break;
        }
    }
    if (!extOk)
        return fail("disallowed output file extension in '" + rawPath + "'");

    std::error_code ec;
    const fs::path cwd = fs::weakly_canonical(fs::current_path(ec), ec);
    std::vector<fs::path> roots;
    if (!sandboxDir.empty()) {
        roots.push_back(fs::weakly_canonical(pathFromUtf8(sandboxDir), ec));
    } else {
        roots.push_back(cwd);
        roots.push_back(fs::weakly_canonical(fs::temp_directory_path(ec), ec));
    }

    const fs::path base = !sandboxDir.empty() ? roots.front() : cwd;
    const fs::path candidate = input.is_absolute() ? input : (base / input);
    if (fs::is_symlink(candidate, ec))
        return fail("refusing to overwrite symlink: " + rawPath);

    const fs::path resolved = fs::weakly_canonical(candidate, ec);
    if (ec || resolved.empty())
        return fail("cannot resolve output path: " + rawPath);

    auto isInside = [](const fs::path& root, const fs::path& target) {
        if (root.empty() || target == root)
            return false;
        const auto [rootEnd, _] = std::mismatch(root.begin(), root.end(), target.begin(), target.end());
        return rootEnd == root.end();
    };

    bool allowed = false;
    for (const auto& r : roots) {
        if (isInside(r, resolved)) {
            allowed = true;
            break;
        }
    }
    if (!allowed)
        return fail("output path '" + rawPath + "' is outside the allowed output directory");
    if (!fs::exists(resolved.parent_path(), ec))
        return fail("parent directory does not exist for '" + rawPath + "'");

    return utf8FromPath(resolved);
}

}  // namespace lt
