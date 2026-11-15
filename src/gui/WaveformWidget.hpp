// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/AiReport.hpp"
#include "core/CaptureBuffer.hpp"
#include "gui/ViewTransform.hpp"

#include <QWidget>

#include <optional>

namespace lt::gui {

// Digital waveform view: one row per channel, time ruler relative to the trigger, two measurement
// cursors (A/B), Ctrl+wheel zoom and drag-to-pan. Everything the user sees (visible range, cursor
// positions) is exposed through reportOptions() so the AI report describes exactly this view.
class WaveformWidget : public QWidget {
    Q_OBJECT

public:
    explicit WaveformWidget(QWidget* parent = nullptr);

    // Live: the capture on screen is current. Armed: a new capture is waiting for its trigger, so the
    // old waveform is dimmed. Stale: the last attempt failed; the waveform on screen is old data.
    enum class DisplayState { Live, Armed, Stale };
    void setDisplayState(DisplayState state, const QString& banner = {});
    [[nodiscard]] DisplayState displayState() const noexcept { return m_displayState; }

    void setCapture(const lt::CaptureBuffer& buffer);
    void clearCapture();
    [[nodiscard]] const lt::CaptureBuffer& capture() const noexcept { return m_buffer; }
    [[nodiscard]] bool hasCapture() const noexcept { return m_buffer.size() > 0; }

    [[nodiscard]] std::size_t visibleFirst() const { return m_view.firstVisible(); }
    [[nodiscard]] std::size_t visibleEnd() const { return m_view.endVisible(); }
    [[nodiscard]] std::optional<std::size_t> cursorA() const noexcept { return m_cursorA; }
    [[nodiscard]] std::optional<std::size_t> cursorB() const noexcept { return m_cursorB; }
    [[nodiscard]] bool snapToEdges() const noexcept { return m_snapToEdges; }
    [[nodiscard]] const ViewTransform& view() const noexcept { return m_view; }

    // Timeline markers (human or AI). Ids are assigned here; the returned id identifies the marker.
    uint64_t addAnnotation(lt::Annotation annotation);
    void setAnnotations(std::vector<lt::Annotation> annotations);
    [[nodiscard]] const std::vector<lt::Annotation>& annotations() const noexcept { return m_annotations; }

    // Visible range + cursors + annotations, ready for lt::copyForAi().
    [[nodiscard]] lt::ReportOptions reportOptions() const;

    [[nodiscard]] QSize sizeHint() const override { return {900, 420}; }
    [[nodiscard]] QSize minimumSizeHint() const override;

    static constexpr int kGutterWidth = 120;
    static constexpr int kRulerHeight = 30;

public Q_SLOTS:
    void fitAll();
    void zoomIn();
    void zoomOut();
    void zoomToCursors();
    void setCursorA(std::size_t sample);
    void setCursorB(std::size_t sample);
    void setSnapToEdges(bool enabled);
    void clearCursors();
    void clearAnnotations();

Q_SIGNALS:
    void viewChanged();
    void captureChanged();
    void cursorsChanged();
    void annotationsChanged();
    void contextMenuRequested(const QPoint& globalPos);
    // Time of the sample under the pointer, relative to the trigger; hasSample is false outside the plot.
    void hoverChanged(bool hasSample, double seconds, int channelDisplayNumber, int level);

protected:
    void paintEvent(QPaintEvent* event) override;
    void resizeEvent(QResizeEvent* event) override;
    void wheelEvent(QWheelEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;
    void mouseDoubleClickEvent(QMouseEvent* event) override;
    void contextMenuEvent(QContextMenuEvent* event) override;
    void leaveEvent(QEvent* event) override;
    void keyPressEvent(QKeyEvent* event) override;

private:
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] double rowHeight() const;
    [[nodiscard]] std::size_t sampleAtX(double x) const;
    [[nodiscard]] std::size_t sampleAtPosition(const QPointF& pos, bool allowSnap) const;
    void drawRuler(QPainter& p, const QRectF& plot) const;
    void drawChannel(QPainter& p, const QRectF& plot, std::size_t channel, const QRectF& row) const;
    void drawCursors(QPainter& p, const QRectF& plot) const;
    void drawAnnotations(QPainter& p, const QRectF& plot) const;
    void updateViewWidth();

    lt::CaptureBuffer m_buffer;
    ViewTransform m_view;
    std::optional<std::size_t> m_cursorA, m_cursorB;
    std::vector<lt::Annotation> m_annotations;
    uint64_t m_nextAnnotationId = 1;
    DisplayState m_displayState = DisplayState::Live;
    QString m_banner;

    bool m_snapToEdges = true;
    bool m_pressed = false;
    bool m_dragging = false;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    Qt::KeyboardModifiers m_pressModifiers;
    QPointF m_pressPos, m_lastPos;
};

}  // namespace lt::gui
