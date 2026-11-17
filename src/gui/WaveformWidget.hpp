// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#pragma once

#include "core/AiReport.hpp"
#include "core/CaptureBuffer.hpp"
#include "gui/ViewTransform.hpp"

#include <QWidget>

#include <optional>
#include <string>
#include <vector>

namespace lt::gui {

// Digital waveform view: one row per channel, an overview strip, a time ruler relative to the trigger,
// two measurement cursors (A/B), Ctrl+wheel zoom and drag-to-pan. Mouse model: left click = cursor A,
// Shift+left click = cursor B, dragging a cursor line moves it, dragging elsewhere pans, right click
// opens the context menu. Everything the user sees (visible range, cursor
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

    // Board name (e.g. "PICO") used to show the GPIO behind each channel in the gutter.
    void setBoard(std::string board);

    void setHideInactive(bool hide);
    [[nodiscard]] bool hideInactive() const noexcept { return m_hideInactive; }
    // Channel indices (into the CaptureBuffer) that are drawn; every channel unless hiding inactive ones.
    [[nodiscard]] const std::vector<std::size_t>& visibleChannels() const noexcept { return m_rows; }
    [[nodiscard]] bool channelActive(std::size_t channel) const noexcept
    {
        return channel < m_active.size() && m_active[channel];
    }

    // Vertical layout: row height in px (0 = auto-fit to the widget) and scroll offset of the channel rows.
    // The overview and ruler stay pinned while the rows scroll.
    void setRowHeight(double px);
    void zoomRows(double factor);
    [[nodiscard]] double rowHeightSetting() const noexcept { return m_rowHeightPx; }
    [[nodiscard]] double effectiveRowHeight() const { return rowHeight(); }
    void setScrollY(double px);
    [[nodiscard]] double scrollY() const noexcept { return m_scrollY; }
    [[nodiscard]] double maxScrollY() const;
    [[nodiscard]] std::optional<std::size_t> rowAt(double y) const { return rowAtY(y); }

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
    // Automatic anomaly markers (computed from the capture, independent of annotations()).
    [[nodiscard]] bool showAnomalyMarkers() const noexcept { return m_showAnomalies; }
    [[nodiscard]] const std::vector<lt::AnomalyIncident>& anomalies() const noexcept { return m_anomalies; }
    [[nodiscard]] const std::vector<std::size_t>& segmentStarts() const noexcept { return m_segmentStarts; }

    [[nodiscard]] QSize sizeHint() const override { return {900, 420}; }
    [[nodiscard]] QSize minimumSizeHint() const override;

    static constexpr int kGutterWidth = 180;
    static constexpr int kOverviewHeight = 18;
    static constexpr int kRulerHeight = 30;
    static constexpr int kPlotTop = kOverviewHeight + kRulerHeight;  // y of the first channel row
    static constexpr double kGrabPx = 6.0;                           // cursor / annotation hit distance

    // Widget x of the start of a sample (cursors, annotations, ruler, trigger and edges share it), and the
    // annotation / cursor within grab distance of an x (0 = none; cursor 1 = A, 2 = B).
    [[nodiscard]] double xOfSample(double sample) const;
    [[nodiscard]] uint64_t annotationNear(double x) const;
    [[nodiscard]] int cursorNear(double x) const;

public Q_SLOTS:
    void fitAll();
    void zoomIn();
    void zoomOut();
    void zoomToCursors();
    // Scrolls so `sample` is centred, keeping the current zoom level.
    void centerOn(std::size_t sample);
    // Shows samples [first, end) (clamped to the capture).
    void showSampleRange(double first, double end);
    // Incidents over the whole capture, drawn as ruler markers; does not touch annotations().
    void setAnomalies(std::vector<lt::AnomalyIncident> anomalies);
    void setShowAnomalyMarkers(bool show);
    // Segmented capture: first stored sample of every trigger window (drawn as seams; empty = continuous).
    void setSegmentStarts(std::vector<std::size_t> starts);
    void setCursorA(std::size_t sample);
    void setCursorB(std::size_t sample);
    void setSnapToEdges(bool enabled);
    void clearCursors();
    bool removeAnnotation(uint64_t id);
    void clearAnnotations();

Q_SIGNALS:
    void viewChanged();
    void captureChanged();
    void cursorsChanged();
    void annotationsChanged();
    // Buttons of the empty-state card.
    void demoRequested();
    void openRequested();
    // Right click (or the menu key): the sample under the pointer and the id of an annotation within
    // kGrabPx of it (0 = none).
    void contextMenuRequested(const QPoint& globalPos, std::size_t sample, uint64_t annotationId);
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
    bool event(QEvent* event) override;

private:
    [[nodiscard]] QRectF plotRect() const;
    [[nodiscard]] double rowHeight() const;
    [[nodiscard]] std::size_t sampleAtX(double x) const;
    [[nodiscard]] std::size_t sampleAtPosition(const QPointF& pos, bool allowSnap) const;
    [[nodiscard]] std::optional<std::size_t> rowAtY(double y) const;  // index into m_rows
    [[nodiscard]] QString hintAt(const QPointF& pos) const;
    [[nodiscard]] QRectF overviewRect() const;
    [[nodiscard]] double overviewX(double sample) const;
    void jumpFromOverview(double x);
    void rebuildChannelState();
    void updateEmptyCard();
    void clampScroll();
    void drawOverview(QPainter& p) const;
    void drawRuler(QPainter& p, const QRectF& plot) const;
    void drawChannel(QPainter& p, const QRectF& plot, std::size_t channel, const QRectF& row) const;
    void drawCursors(QPainter& p, const QRectF& plot) const;
    void drawAnnotations(QPainter& p, const QRectF& plot) const;
    void drawAnomalyMarkers(QPainter& p, const QRectF& plot) const;
    void drawSegmentSeams(QPainter& p, const QRectF& plot) const;
    void updateViewWidth();

    lt::CaptureBuffer m_buffer;
    ViewTransform m_view;
    std::optional<std::size_t> m_cursorA, m_cursorB;
    std::vector<lt::Annotation> m_annotations;
    uint64_t m_nextAnnotationId = 1;
    DisplayState m_displayState = DisplayState::Live;
    QString m_banner;

    QWidget* m_emptyCard = nullptr;
    std::vector<lt::AnomalyIncident> m_anomalies;
    std::vector<std::size_t> m_segmentStarts;
    bool m_showAnomalies = true;
    double m_rowHeightPx = 0;  // 0 = auto
    double m_scrollY = 0;
    std::string m_board;
    bool m_hideInactive = false;
    std::vector<bool> m_active;           // per channel: has at least one transition
    std::vector<std::size_t> m_rows;      // channels currently drawn
    mutable std::vector<uint32_t> m_density;  // overview cache: transitions per column
    mutable int m_densityWidth = 0;
    int m_dragCursor = 0;                 // 1 = A, 2 = B while a cursor line is being dragged
    bool m_overviewDrag = false;
    std::size_t m_hoverSample = 0;

    bool m_snapToEdges = true;
    bool m_pressed = false;
    bool m_dragging = false;
    Qt::MouseButton m_pressButton = Qt::NoButton;
    Qt::KeyboardModifiers m_pressModifiers;
    QPointF m_pressPos, m_lastPos;
};

}  // namespace lt::gui
