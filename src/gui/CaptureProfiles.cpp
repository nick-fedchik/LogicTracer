// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/CaptureProfiles.hpp"

#include "core/ArgParsing.hpp"
#include "core/Exporters.hpp"
#include "core/Paths.hpp"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTextBoundaryFinder>

#include <algorithm>
#include <filesystem>
#include <limits>
#include <utility>
#include <vector>

namespace lt::gui::profiles {
namespace {

constexpr QLatin1StringView suffix(".ltprofile");
constexpr std::size_t listLimit = 100;
constexpr qint64 maxProfileBytes = 1 << 20; // room for four JSON-escaped QLineEdit values

bool fileName(const QString& name)
{
    return !name.isEmpty() && name != "." && name != ".." && !QDir::fromNativeSeparators(name).contains('/')
#ifdef Q_OS_WIN
        && !name.contains(':') // drives and alternate data streams
#endif
        && !name.contains(QChar::Null);
}

// Do not create through (or later delete through) a redirected application/profiles directory.
bool checkDirectory(bool create, QString& error)
{
    QDir parent = QDir::home();
    for (const auto& part : {QStringLiteral(".logictracer"), QStringLiteral("profiles")}) {
        const QFileInfo info(parent.filePath(part));
        if (create && !info.exists() && !info.isSymLink())
            parent.mkdir(part);
        const QFileInfo current(info.filePath());
        if (!current.isDir() || current.isSymLink() || current.isJunction()) {
            error = QObject::tr("Not an accessible profile directory: %1").arg(info.filePath());
            return false;
        }
        parent.setPath(info.filePath());
    }
    return true;
}

bool validFields(const CaptureFields& f, QString& error)
{
    // Check syntax against the protocol, not the current board. Device-specific limits are checked
    // by the ordinary setup preflight after selecting a profile, without losing the requested values.
    for (const QString* s : {&f.channels, &f.rate, &f.samples, &f.trigger}) {
        if (s->size() > 32767 || s->contains(QChar::Null) || s->contains('\n') || s->contains('\r')) {
            error = QObject::tr("Invalid single-line capture field");
            return false;
        }
    }
    std::string why;
    if (!cli::parseChannelList(f.channels.toStdString(), 32, &why)
        || !cli::parseTrigger(f.trigger.toStdString(), 33, &why)) {
        error = QString::fromStdString(why);
        return false;
    }
    const auto rate = cli::parseQuantity(f.rate.toStdString());
    const auto samples = cli::parseQuantity(f.samples.toStdString());
    if (!rate || *rate == 0 || *rate > std::numeric_limits<uint32_t>::max())
        error = QObject::tr("Invalid Rate");
    else if (!samples || *samples < 4 || *samples > std::numeric_limits<uint32_t>::max())
        error = QObject::tr("Invalid Samples");
    else if (f.windows < 1 || f.windows > 65536)
        error = QObject::tr("Windows must be an integer from 1 to 65536");
    else if (f.timeout < 0 || f.timeout > 600000)
        error = QObject::tr("Timeout must be an integer from 0 to 600000 ms");
    else
        return true;
    return false;
}

}  // namespace

QString directory()
{
    return QDir::home().filePath(QStringLiteral(".logictracer/profiles"));
}

QString savePath(const QString& name, QString& error)
{
    const QString stem = name.endsWith(suffix) ? name.chopped(suffix.size()) : name;
    if (!fileName(stem) || stem.trimmed().isEmpty()
        || std::any_of(stem.cbegin(), stem.cend(), [](QChar c) {
               return c.category() == QChar::Other_Control || QStringLiteral("<>:\"\\|?*").contains(c);
           })) {
        error = QObject::tr("Enter a profile name, not a path or a filename with reserved characters.");
        return {};
    }
    return QDir(directory()).filePath(stem + suffix);
}

bool save(const QString& name, const CaptureFields& f, QString& error)
{
    const QString path = savePath(name, error);
    if (path.isEmpty() || !validFields(f, error) || !checkDirectory(true, error))
        return false;
    const QFileInfo target(path);
    if (target.isSymLink() || (target.exists() && !target.isFile())) {
        error = QObject::tr("Refusing to replace a link or non-file: %1").arg(path);
        return false;
    }
    const nlohmann::json doc{{"schema_version", 1}, {"channels", f.channels.toStdString()},
                             {"rate", f.rate.toStdString()}, {"samples", f.samples.toStdString()},
                             {"trigger", f.trigger.toStdString()}, {"windows", f.windows}, {"timeout_ms", f.timeout}};
    std::string why;
    // Resolve the home itself (which can legitimately be a symlink), not the managed subdirectories.
    const QDir home(QDir::home().canonicalPath());
    const QString confinedPath = home.filePath(".logictracer/profiles/" + target.fileName());
    if (!writeJsonFile(confinedPath.toStdString(), doc, &why, true)) {
        error = QString::fromStdString(why);
        return false;
    }
    return true;
}

std::optional<Profile> load(const QString& nameOrPath, QString& error)
{
    const QString arg = QDir::fromNativeSeparators(nameOrPath);
    const bool explicitPath = arg.contains('/') || (!arg.startsWith(':') && QDir::isAbsolutePath(arg));
    QString path;
    if (explicitPath && !arg.contains(QChar::Null)) {
        path = QFileInfo(arg).absoluteFilePath();
    } else if (fileName(arg)) {
        path = directory() + '/' + (arg.endsWith(suffix) ? arg : arg + suffix);
    }
    const auto fail = [&](const QString& why) -> std::optional<Profile> {
        error = QObject::tr("Cannot load profile '%1': %2").arg(path.isEmpty() ? nameOrPath : path, why);
        return std::nullopt;
    };
    if (path.isEmpty())
        return fail(QObject::tr("invalid profile name"));
    if (!explicitPath) {
        QString why;
        if (!checkDirectory(false, why))
            return fail(why);
    }
    const QFileInfo info(path);
    if (!info.exists())
        return fail(QObject::tr("file does not exist"));
    if (!info.isFile() || (!explicitPath && info.isSymLink()))
        return fail(QObject::tr("not a regular profile file"));
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return fail(file.errorString());
    const QByteArray bytes = file.read(maxProfileBytes + 1);
    if (file.error() != QFileDevice::NoError)
        return fail(file.errorString());
    if (bytes.size() > maxProfileBytes)
        return fail(QObject::tr("profile file is too large"));
    const auto doc = nlohmann::json::parse(bytes.cbegin(), bytes.cend(), nullptr, false);
    if (!doc.is_object() || !doc.contains("schema_version") || !doc["schema_version"].is_number_integer()
        || doc["schema_version"] != 1)
        return fail(QObject::tr("expected a version 1 profile object"));
    for (const auto* key : {"channels", "rate", "samples", "trigger"})
        if (!doc.contains(key) || !doc[key].is_string())
            return fail(QObject::tr("expected a string for %1").arg(QString::fromLatin1(key)));
    const auto integerIn = [&](const char* key, int low, int high) {
        return doc.contains(key) && doc[key].is_number_integer() && doc[key] >= low && doc[key] <= high;
    };
    if (!integerIn("windows", 1, 65536) || !integerIn("timeout_ms", 0, 600000))
        return fail(QObject::tr("expected integer windows (1–65536) and timeout_ms (0–600000)"));
    const auto text = [&](const char* key) { return QString::fromStdString(doc[key].get<std::string>()); };
    CaptureFields f{text("channels"), text("rate"), text("samples"), text("trigger"),
                    doc["windows"].get<int>(), doc["timeout_ms"].get<int>()};
    QString why;
    if (!validFields(f, why))
        return fail(why);
    return Profile{path, std::move(f)};
}

bool remove(const QString& filename, QString& error)
{
    if (!fileName(filename) || !filename.endsWith(suffix)) {
        error = QObject::tr("Not a stored profile filename: %1").arg(filename);
        return false;
    }
    if (!checkDirectory(false, error))
        return false;
    QFile file(directory() + '/' + filename);
    const QFileInfo info(file);
    if (info.isSymLink() || !info.isFile()) {
        error = QObject::tr("Not a regular profile file: %1").arg(file.fileName());
        return false;
    }
    if (!file.remove()) {
        error = QObject::tr("%1: %2").arg(file.fileName(), file.errorString());
        return false;
    }
    return true;
}

QStringList recentFiles()
{
    QString why;
    if (!checkDirectory(false, why))
        return {};
    namespace fs = std::filesystem;
    struct Entry {
        fs::file_time_type mtime;
        QString name;
    };
    const auto newer = [](const Entry& a, const Entry& b) {
        return a.mtime != b.mtime ? a.mtime > b.mtime : a.name < b.name;
    };
    // A heap with the worst retained entry at the front. No file bodies or full-directory lists.
    std::vector<Entry> kept;
    kept.reserve(listLimit);
    std::error_code ec;
    for (fs::directory_iterator it(pathFromUtf8(directory().toStdString()), ec), end;
         !ec && it != end; it.increment(ec)) {
        const auto filename = utf8FromPath(it->path().filename());
        const QString name = QString::fromStdString(filename);
        if (!name.endsWith(suffix) || !fileName(name) || name.toStdString() != filename)
            continue;
        std::error_code statError;
        if (!fs::is_regular_file(it->symlink_status(statError)))
            continue;
        Entry entry{it->last_write_time(statError), name};
        if (statError)
            continue;
        if (kept.size() == listLimit) {
            if (!newer(entry, kept.front()))
                continue;
            std::pop_heap(kept.begin(), kept.end(), newer);
            kept.pop_back();
        }
        kept.push_back(std::move(entry));
        std::push_heap(kept.begin(), kept.end(), newer);
    }
    std::sort_heap(kept.begin(), kept.end(), newer);
    QStringList files;
    for (const auto& entry : kept)
        files.push_back(entry.name);
    return files;
}

QString shortName(const QString& filename)
{
    const QString stem = filename.endsWith(suffix) ? filename.chopped(suffix.size()) : filename;
    QTextBoundaryFinder grapheme(QTextBoundaryFinder::Grapheme, stem);
    qsizetype cutoff = 0;
    for (int i = 1; i <= 32; ++i) {
        const auto pos = grapheme.toNextBoundary();
        if (pos < 0 || pos == stem.size())
            return stem;
        if (i == 31)
            cutoff = pos;
    }
    return stem.left(cutoff) + QChar(0x2026);
}

}  // namespace lt::gui::profiles
