// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/ProfileSelector.hpp"

#include "gui/CaptureProfiles.hpp"

#include <QComboBox>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QInputDialog>
#include <QKeyEvent>
#include <QLabel>
#include <QListView>
#include <QMessageBox>
#include <QProxyStyle>
#include <QPushButton>
#include <QSignalBlocker>
#include <QtConcurrentRun>
#include <utility>

namespace lt::gui {
namespace {

// Some native menu-style combos ignore maxVisibleItems and use arrows instead of a scrollbar.
class ListPopupStyle : public QProxyStyle {
    int styleHint(StyleHint hint, const QStyleOption* option, const QWidget* widget,
                  QStyleHintReturn* result) const override
    {
        if (hint == SH_ComboBox_Popup || hint == SH_ComboBox_UseNativePopup)
            return 0;
        return QProxyStyle::styleHint(hint, option, widget, result);
    }
};

QString storedFilename(const QString& path)
{
    if (path.isEmpty())
        return {};
    const QFileInfo file(path);
    return file.absolutePath() == profiles::directory() && file.fileName().endsWith(".ltprofile")
        ? file.fileName() : QString{};
}

void confirm(QWidget* parent, const QString& title, const QString& message, std::function<void()> accepted)
{
    auto* box = new QMessageBox(QMessageBox::Question, title, message, QMessageBox::Yes | QMessageBox::Cancel, parent);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextFormat(Qt::PlainText);
    box->setDefaultButton(QMessageBox::Cancel);
    QObject::connect(box, &QDialog::finished, parent, [accepted = std::move(accepted)](int result) {
        if (result == QMessageBox::Yes)
            accepted();
    });
    box->open();
}

}  // namespace

class ProfileCombo : public QComboBox {
public:
    explicit ProfileCombo(std::function<void()> refresh, QWidget* parent)
        : QComboBox(parent), m_refresh(std::move(refresh))
    {
        auto* listStyle = new ListPopupStyle;
        listStyle->setParent(this);
        setStyle(listStyle);
        setView(new QListView);
        view()->setVerticalScrollBarPolicy(Qt::ScrollBarAsNeeded);
        setMaxVisibleItems(5);
    }

    void showPopup() override
    {
        m_requested = true;
        m_refresh();
    }

    void hidePopup() override
    {
        m_requested = false;
        QComboBox::hidePopup();
    }

    void showWhenReady()
    {
        if (std::exchange(m_requested, false) && isVisible() && isEnabled())
            QComboBox::showPopup();
    }

protected:
    void focusOutEvent(QFocusEvent* event) override
    {
        m_requested = false;
        QComboBox::focusOutEvent(event);
    }

    void keyPressEvent(QKeyEvent* event) override
    {
        if (m_requested && event->key() == Qt::Key_Escape) {
            m_requested = false;
            event->accept();
            return;
        }
        QComboBox::keyPressEvent(event);
    }

    void hideEvent(QHideEvent* event) override
    {
        m_requested = false;
        QComboBox::hideEvent(event);
    }

private:
    std::function<void()> m_refresh;
    bool m_requested = false;
};

ProfileSelector::ProfileSelector(std::function<CaptureFields()> read, std::function<void(const CaptureFields&)> apply,
                                 QWidget* parent)
    : QWidget(parent), m_read(std::move(read)), m_apply(std::move(apply)),
      m_combo(new ProfileCombo([this] { refresh(); }, this)), m_delete(new QPushButton(tr("Delete profile"), this))
{
    auto* row = new QHBoxLayout(this);
    row->setContentsMargins(0, 0, 0, 0);
    auto* label = new QLabel(tr("Profile"));
    label->setBuddy(m_combo);
    m_combo->setObjectName("profileSelector");
    m_combo->setAccessibleName(tr("Profile"));
    m_combo->setMinimumContentsLength(12);
    m_combo->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    auto* load = new QPushButton(tr("Load…"));
    load->setToolTip(tr("Load any profile by name or path, including one outside this list"));
    load->setObjectName("loadProfile");
    auto* save = new QPushButton(tr("Save as…"));
    save->setObjectName("saveProfile");
    m_delete->setObjectName("deleteProfile");
    row->addWidget(label);
    row->addWidget(m_combo, 1);
    row->addWidget(m_delete);
    row->addWidget(load);
    row->addWidget(save);
    applySelection();

    connect(m_combo, &QComboBox::activated, this, [this](int index) {
        const QString filename = m_combo->itemData(index).toString();
        if (!filename.isEmpty()) {
            setSelectedPath(profiles::directory() + '/' + filename);
            loadProfile(filename);
        }
    });
    connect(save, &QPushButton::clicked, this, [this] { askName(true); });
    connect(load, &QPushButton::clicked, this, [this] { askName(false); });
    connect(m_delete, &QPushButton::clicked, this, &ProfileSelector::deleteSelected);
    connect(&m_scan, &QFutureWatcher<QStringList>::finished, this, [this] {
        m_scanning = false;
        if (std::exchange(m_rescan, false)) {
            refresh();
            return;
        }
        {
            const QSignalBlocker block(m_combo);
            const bool wasListed = m_combo->currentIndex() >= 0;
            m_combo->clear();
            const QString root = profiles::directory() + '/';
            for (const auto& filename : m_scan.result()) {
                m_combo->addItem(profiles::shortName(filename), filename);
                m_combo->setItemData(m_combo->count() - 1, root + filename, Qt::ToolTipRole);
                m_combo->setItemData(m_combo->count() - 1, filename, Qt::AccessibleTextRole);
            }
            if (wasListed && m_combo->findData(storedFilename(m_path)) < 0)
                m_path.clear();
            applySelection();
        }
        m_combo->showWhenReady();
    });
}

void ProfileSelector::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    refresh();
}

void ProfileSelector::refresh()
{
    if (m_scanning) {
        m_rescan = true;
        return;
    }
    m_scanning = true;
    m_scan.setFuture(QtConcurrent::run(profiles::recentFiles));
}

void ProfileSelector::setSelectedPath(const QString& path)
{
    m_path = path;
    applySelection();
}

void ProfileSelector::applySelection()
{
    const QString filename = storedFilename(m_path);
    m_combo->setCurrentIndex(filename.isEmpty() ? -1 : m_combo->findData(filename));
    m_combo->setPlaceholderText(m_path.isEmpty() ? tr("Select a profile") : profiles::shortName(QFileInfo(m_path).fileName()));
    m_combo->setToolTip(m_path);
    m_delete->setEnabled(!filename.isEmpty());
    m_delete->setToolTip(!m_path.isEmpty() && filename.isEmpty()
                             ? tr("Only files in the profiles directory can be deleted here. External files are left untouched.")
                             : tr("Delete the selected profile file after confirmation"));
}

void ProfileSelector::loadProfile(const QString& nameOrPath)
{
    QString why;
    if (const auto p = profiles::load(nameOrPath, why)) {
        setSelectedPath(p->path);
        m_apply(p->fields);
    } else {
        showError(why);
    }
}

void ProfileSelector::askName(bool save)
{
    auto* dialog = new QInputDialog(this);
    dialog->setAttribute(Qt::WA_DeleteOnClose);
    dialog->setWindowTitle(save ? tr("Save capture profile") : tr("Load capture profile"));
    dialog->setLabelText(save ? tr("Profile name:") : tr("Profile name or file path:"));
    const auto fields = m_read();
    if (save && !m_path.isEmpty())
        dialog->setTextValue(QFileInfo(m_path).fileName());
    connect(dialog, &QInputDialog::textValueSelected, this, [this, save, fields](const QString& text) {
        if (!save) {
            loadProfile(text);
            return;
        }
        QString why;
        const QString path = profiles::savePath(text, why);
        if (path.isEmpty()) {
            showError(why);
            return;
        }
        const auto write = [this, text, path, fields] {
            QString error;
            if (!profiles::save(text, fields, error)) {
                showError(error);
                return;
            }
            setSelectedPath(path);
            refresh();
        };
        if (QFileInfo::exists(path))
            confirm(this, tr("Overwrite profile"), tr("Replace this saved profile?\n%1").arg(path), write);
        else
            write();
    });
    dialog->open();
}

void ProfileSelector::deleteSelected()
{
    const QString filename = storedFilename(m_path);
    if (filename.isEmpty())
        return;
    const QString path = profiles::directory() + '/' + filename;
    confirm(this, tr("Delete profile"), tr("Delete profile “%1”?\n%2").arg(QFileInfo(filename).completeBaseName(), path),
            [this, filename, path] {
                QString why;
                if (!profiles::remove(filename, why))
                    showError(why);
                else if (m_path == path)
                    setSelectedPath({});
                refresh();
            });
}

void ProfileSelector::showError(const QString& error)
{
    auto* box = new QMessageBox(QMessageBox::Warning, tr("Capture profile"), error, QMessageBox::Ok, this);
    box->setAttribute(Qt::WA_DeleteOnClose);
    box->setTextFormat(Qt::PlainText);
    box->open();
}

}  // namespace lt::gui
