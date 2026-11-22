// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "gui/CaptureFields.hpp"

#include <QStringList>
#include <optional>

namespace lt::gui::profiles {

struct Profile {
    QString path;
    CaptureFields fields;
};

[[nodiscard]] QString directory();
// A bare name (with or without .ltprofile) is relative to directory(); an explicit path is literal.
[[nodiscard]] std::optional<Profile> load(const QString& nameOrPath, QString& error);
[[nodiscard]] QString savePath(const QString& name, QString& error);
[[nodiscard]] bool save(const QString& name, const CaptureFields& fields, QString& error);
[[nodiscard]] bool remove(const QString& filename, QString& error);

// Metadata only. May scan the whole directory; call on a worker, not the GUI thread.
[[nodiscard]] QStringList recentFiles();
[[nodiscard]] QString shortName(const QString& filename);

}  // namespace lt::gui::profiles
