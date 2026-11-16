// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/WaveformWidget.hpp"

#include "core/DeviceProtocol.hpp"
#include "core/Format.hpp"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QHelpEvent>
#include <QToolTip>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QPushButton>
#include <QVBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
#include <limits>
#include <cmath>
#include <vector>

namespace lt::gui {

namespace {

const QColor kBackground(24, 26, 31);
const QColor kRowAlt(30, 33, 39);
const QColor kGrid(58, 62, 72);
const QColor kText(205, 210, 220);
const QColor kTrigger(230, 80, 80);
const QColor kCursorA(255, 170, 40);
const QColor kCursorB(60, 200, 235);

QColor channelColor(std::size_t index)
{
    static const QColor palette[] = {{90, 200, 120},  {240, 200, 80},  {110, 160, 245}, {235, 110, 110},
                                     {200, 130, 235}, {90, 210, 200},  {240, 150, 90},  {170, 200, 90}};
    return palette[index % (sizeof palette / sizeof *palette)];
}

constexpr int kMinRowHeight = 18;
constexpr int kMaxRowHeight = 48;
constexpr double kDragThreshold = 3.0;

// 1-2-5 progression for ruler ticks.
double niceStep(double rough)
{
    const double exp10 = std::pow(10.0, std::floor(std::log10(rough)));
    const double f = rough / exp10;
    return (f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10) * exp10;
}

}  // namespace

WaveformWidget::WaveformWidget(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setFocusPolicy(Qt::StrongFocus);
    setMinimumWidth(kGutterWidth + 200);
    setAutoFillBackground(false);

    // Guided empty state: what to do first, with one-click access to the demo and to saved captures.
    m_emptyCard = new QWidget(this);
    m_emptyCard->setObjectName("emptyCard");
    auto* card = new QVBoxLayout(m_emptyCard);
    auto* title = new QLabel(tr("No capture yet"));
    QFont big = title->font();
    big.setBold(true);
    big.setPointSizeF(big.pointSizeF() * 1.3);
    title->setFont(big);
    title->setAlignment(Qt::AlignCenter);
    auto* steps = new QLabel(tr("1. Connect  \u2192  2. Probe lines  \u2192  3. Set Trigger  \u2192  4. Capture"));
    steps->setAlignment(Qt::AlignCenter);
    auto* demo = new QPushButton(tr("Load Demo Capture"));
    demo->setObjectName("demoButton");
    demo->setToolTip(tr("Explore the viewer with a synthetic capture (Ctrl+D); no hardware needed"));
    auto* open = new QPushButton(tr("Open Capture..."));
    open->setObjectName("openButton");
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(demo);
    buttons->addWidget(open);
    buttons->addStretch(1);
    card->addWidget(title);
    card->addWidget(steps);
    card->addLayout(buttons);
    m_emptyCard->setStyleSheet("QLabel { color: #cdd2dc; }");
    connect(demo, &QPushButton::clicked, this, &WaveformWidget::demoRequested);
    connect(open, &QPushButton::clicked, this, &WaveformWidget::openRequested);
    updateEmptyCard();
}

void WaveformWidget::updateEmptyCard()
{
    m_emptyCard->setVisible(!hasCapture());
    m_emptyCard->adjustSize();
    m_emptyCard->move((width() - m_emptyCard->width()) / 2, (height() - m_emptyCard->height()) / 2);
}

QSize WaveformWidget::minimumSizeHint() const
{
    const int rows = std::max<int>(1, static_cast<int>(m_rows.size()));
    return {kGutterWidth + 200, kPlotTop + rows * kMinRowHeight};
}

QRectF WaveformWidget::plotRect() const
{
    return QRectF(kGutterWidth, kPlotTop, std::max(1, width() - kGutterWidth), std::max(1, height() - kPlotTop));
}

double WaveformWidget::maxScrollY() const
{
    return std::max(0.0, rowHeight() * static_cast<double>(m_rows.size()) - plotRect().height());
}

void WaveformWidget::clampScroll()
{
    m_scrollY = std::clamp(m_scrollY, 0.0, maxScrollY());
}

void WaveformWidget::setScrollY(double px)
{
    const double before = m_scrollY;
    m_scrollY = px;
    clampScroll();
    if (m_scrollY != before)
        update();
}

void WaveformWidget::setRowHeight(double px)
{
    m_rowHeightPx = px <= 0 ? 0.0 : std::clamp(px, 12.0, 160.0);
    clampScroll();
    update();
}

void WaveformWidget::zoomRows(double factor)
{
    const double current = rowHeight();
    // Keep the row under the top edge of the plot fixed while scaling.
    const double topRow = (m_scrollY) / current;
    setRowHeight(current * factor);
    m_scrollY = topRow * rowHeight();
    clampScroll();
    update();
}

double WaveformWidget::rowHeight() const
{
    if (m_rowHeightPx > 0)
        return m_rowHeightPx;
    const auto n = std::max<std::size_t>(1, m_rows.size());
    return std::clamp(plotRect().height() / static_cast<double>(n), static_cast<double>(kMinRowHeight),
                      static_cast<double>(kMaxRowHeight));
}

void WaveformWidget::updateViewWidth()
{
    m_view.setWidth(plotRect().width());
}

void WaveformWidget::setCapture(const lt::CaptureBuffer& buffer)
{
    m_buffer = buffer;
    rebuildChannelState();
    updateEmptyCard();
    m_scrollY = 0;
    m_cursorA.reset();
    m_cursorB.reset();
    m_annotations.clear();  // annotation times are relative to the trigger of the capture they were made on
    m_anomalies.clear();  // the owner supplies them (setAnomalies) once the analysis options are known
    m_view.setTotalSamples(m_buffer.size());
    updateViewWidth();
    m_view.fitAll();
    updateGeometry();
    update();
    Q_EMIT captureChanged();
    Q_EMIT viewChanged();
    Q_EMIT cursorsChanged();
}

void WaveformWidget::setBoard(std::string board)
{
    m_board = std::move(board);
    update();
}

void WaveformWidget::setHideInactive(bool hide)
{
    if (m_hideInactive == hide)
        return;
    m_hideInactive = hide;
    rebuildChannelState();
    updateGeometry();
    update();
}

void WaveformWidget::rebuildChannelState()
{
    const std::size_t nCh = m_buffer.channels().size();
    m_active.assign(nCh, false);
    const auto& samples = m_buffer.samples();
    if (!samples.empty()) {
        uint32_t changed = 0;
        const uint32_t first = samples.front();
        for (const uint32_t v : samples)
            changed |= v ^ first;
        for (std::size_t ch = 0; ch < nCh && ch < 32; ++ch)
            m_active[ch] = (changed >> ch) & 1u;
    }
    m_rows.clear();
    const bool any = std::find(m_active.begin(), m_active.end(), true) != m_active.end();
    for (std::size_t ch = 0; ch < nCh; ++ch)
        if (!m_hideInactive || !any || m_active[ch])  // never hide everything
            m_rows.push_back(ch);
    m_densityWidth = 0;
    clampScroll();
}

void WaveformWidget::setDisplayState(DisplayState state, const QString& banner)
{
    if (m_displayState == state && m_banner == banner)
        return;
    m_displayState = state;
    m_banner = banner;
    update();
}

void WaveformWidget::clearCapture()
{
    setCapture(lt::CaptureBuffer{});
}

lt::ReportOptions WaveformWidget::reportOptions() const
{
    lt::ReportOptions o;
    o.first = m_view.firstVisible();
    o.last = m_view.endVisible();
    o.cursorA = m_cursorA;
    o.cursorB = m_cursorB;
    o.annotations = m_annotations;
    return o;
}

void WaveformWidget::fitAll()
{
    m_view.fitAll();
    update();
    Q_EMIT viewChanged();
}

void WaveformWidget::zoomIn()
{
    m_view.zoomAt(1.5, plotRect().width() / 2);
    update();
    Q_EMIT viewChanged();
}

void WaveformWidget::zoomOut()
{
    m_view.zoomAt(1.0 / 1.5, plotRect().width() / 2);
    update();
    Q_EMIT viewChanged();
}

void WaveformWidget::zoomToCursors()
{
    if (!m_cursorA || !m_cursorB || !hasCapture())
        return;
    const double lo = static_cast<double>(std::min(*m_cursorA, *m_cursorB));
    const double hi = static_cast<double>(std::max(*m_cursorA, *m_cursorB) + 1);
    const double pad = std::max(4.0, (hi - lo) * 0.1);
    m_view.showRange(std::max(0.0, lo - pad), std::min(static_cast<double>(m_buffer.size()), hi + pad));
    update();
    Q_EMIT viewChanged();
}

void WaveformWidget::centerOn(std::size_t sample)
{
    if (!hasCapture())
        return;
    const double half = std::max(1.0, static_cast<double>(m_view.endVisible() - m_view.firstVisible()) / 2.0);
    const double centre = static_cast<double>(std::min(sample, m_buffer.size() - 1)) + 0.5;
    m_view.showRange(std::max(0.0, centre - half), std::min(static_cast<double>(m_buffer.size()), centre + half));
    update();
    Q_EMIT viewChanged();
}

void WaveformWidget::showSampleRange(double first, double end)
{
    if (!hasCapture())
        return;
    m_view.showRange(first, end);
    update();
    Q_EMIT viewChanged();
}

void WaveformWidget::setAnomalies(std::vector<lt::AnomalyIncident> anomalies)
{
    m_anomalies = std::move(anomalies);
    update();
}

void WaveformWidget::setShowAnomalyMarkers(bool show)
{
    if (m_showAnomalies == show)
        return;
    m_showAnomalies = show;
    update();
}

void WaveformWidget::drawAnomalyMarkers(QPainter& p, const QRectF& plot) const
{
    if (!m_showAnomalies || m_anomalies.empty())
        return;
    p.save();
    p.setClipRect(QRectF(plot.left(), kPlotTop - 8, plot.width(), 10));
    p.setPen(Qt::NoPen);
    for (const auto& a : m_anomalies) {
        const double x = xOfSample(static_cast<double>(a.sample));
        if (x < plot.left() - 6 || x > plot.right() + 6)
            continue;
        p.setBrush(a.severity >= 3 ? QColor(235, 80, 70) : a.severity == 2 ? QColor(240, 170, 50) : QColor(230, 215, 90));
        const QPointF tri[3] = {{x - 4, kPlotTop - 8}, {x + 4, kPlotTop - 8}, {x, kPlotTop - 1}};
        p.drawPolygon(tri, 3);
    }
    p.restore();
}

void WaveformWidget::setSnapToEdges(bool enabled)
{
    m_snapToEdges = enabled;
}

void WaveformWidget::setCursorA(std::size_t sample)
{
    m_cursorA = std::min(sample, m_buffer.size() ? m_buffer.size() - 1 : 0);
    update();
    Q_EMIT cursorsChanged();
}

void WaveformWidget::setCursorB(std::size_t sample)
{
    m_cursorB = std::min(sample, m_buffer.size() ? m_buffer.size() - 1 : 0);
    update();
    Q_EMIT cursorsChanged();
}

uint64_t WaveformWidget::addAnnotation(lt::Annotation annotation)
{
    constexpr std::size_t kMaxAnnotations = 500;
    if (m_annotations.size() >= kMaxAnnotations)
        m_annotations.erase(m_annotations.begin());
    annotation.id = m_nextAnnotationId++;
    m_annotations.push_back(std::move(annotation));
    update();
    Q_EMIT annotationsChanged();
    return m_annotations.back().id;
}

void WaveformWidget::setAnnotations(std::vector<lt::Annotation> annotations)
{
    m_annotations = std::move(annotations);
    uint64_t maxId = 0;
    for (auto& a : m_annotations) {
        if (a.id == 0)
            a.id = ++maxId;
        else
            maxId = std::max(maxId, a.id);
    }
    m_nextAnnotationId = maxId + 1;
    update();
    Q_EMIT annotationsChanged();
}

bool WaveformWidget::removeAnnotation(uint64_t id)
{
    const auto it = std::find_if(m_annotations.begin(), m_annotations.end(), [id](const auto& a) { return a.id == id; });
    if (it == m_annotations.end())
        return false;
    m_annotations.erase(it);
    update();
    Q_EMIT annotationsChanged();
    return true;
}

void WaveformWidget::clearAnnotations()
{
    m_annotations.clear();
    update();
    Q_EMIT annotationsChanged();
}

void WaveformWidget::clearCursors()
{
    m_cursorA.reset();
    m_cursorB.reset();
    update();
    Q_EMIT cursorsChanged();
}

std::size_t WaveformWidget::sampleAtX(double x) const
{
    const double s = std::floor(m_view.sampleAt(x - kGutterWidth));
    return static_cast<std::size_t>(std::clamp(s, 0.0, static_cast<double>(m_buffer.size() ? m_buffer.size() - 1 : 0)));
}

std::size_t WaveformWidget::sampleAtPosition(const QPointF& pos, bool allowSnap) const
{
    const std::size_t raw = sampleAtX(pos.x());
    if (!allowSnap || !m_snapToEdges || m_buffer.size() < 2 || m_buffer.channels().empty())
        return raw;

    constexpr double kRowSnapPx = 8.0;
    constexpr double kAnySnapPx = 6.0;
    constexpr std::size_t kMaxScanRadius = 2048;

    const double clickX = pos.x() - kGutterWidth;
    const double rawLo = std::max(1.0, std::floor(m_view.sampleAt(clickX - kRowSnapPx)));
    const double rawHi = std::min(static_cast<double>(m_buffer.size() - 1), std::ceil(m_view.sampleAt(clickX + kRowSnapPx)));
    if (rawHi < rawLo)
        return raw;

    std::size_t sLo = static_cast<std::size_t>(rawLo);
    std::size_t sHi = static_cast<std::size_t>(rawHi);
    if (sHi - sLo > kMaxScanRadius * 2) {
        sLo = raw > kMaxScanRadius ? std::max<std::size_t>(1, raw - kMaxScanRadius) : 1;
        sHi = std::min<std::size_t>(m_buffer.size() - 1, raw + kMaxScanRadius);
    }

    const auto clickedRow = rowAtY(pos.y());
    const auto clickedCh = clickedRow ? m_rows[*clickedRow] : m_buffer.channels().size();

    if (clickedCh < m_buffer.channels().size()) {
        double bestPx = kRowSnapPx + 1e-6;
        std::optional<std::size_t> bestSample;
        for (std::size_t i = sLo; i <= sHi; ++i) {
            if (m_buffer.bit(i, clickedCh) != m_buffer.bit(i - 1, clickedCh)) {
                const double dist = std::abs(m_view.xAt(static_cast<double>(i)) - clickX);
                if (dist <= bestPx) {
                    bestPx = dist;
                    bestSample = i;
                }
            }
        }
        if (bestSample)
            return *bestSample;
    }

    const std::size_t nCh = m_buffer.channels().size();
    const uint32_t mask = nCh >= 32 ? 0xFFFFFFFFu : ((1u << nCh) - 1u);
    double bestPx = kAnySnapPx + 1e-6;
    std::optional<std::size_t> bestSample;
    for (std::size_t i = sLo; i <= sHi; ++i) {
        if (((m_buffer.samples()[i] ^ m_buffer.samples()[i - 1]) & mask) != 0) {
            const double dist = std::abs(m_view.xAt(static_cast<double>(i)) - clickX);
            if (dist <= bestPx) {
                bestPx = dist;
                bestSample = i;
            }
        }
    }
    return bestSample.value_or(raw);
}

std::optional<std::size_t> WaveformWidget::rowAtY(double y) const
{
    if (y < kPlotTop)
        return std::nullopt;
    const auto r = static_cast<std::size_t>((y - kPlotTop + m_scrollY) / rowHeight());
    return r < m_rows.size() ? std::optional<std::size_t>(r) : std::nullopt;
}

double WaveformWidget::xOfSample(double sample) const
{
    // One convention for every sample-anchored mark: sample i starts at its left cell boundary, which is
    // where an edge into sample i, the ruler tick for its time, the trigger and annotations are drawn.
    return kGutterWidth + m_view.xAt(sample);
}

uint64_t WaveformWidget::annotationNear(double x) const
{
    uint64_t best = 0;
    double bestDist = kGrabPx + 1e-6;
    const double rate = m_buffer.frequency();
    for (const auto& a : m_annotations) {
        const double sample = static_cast<double>(m_buffer.triggerIndex()) + a.timeSeconds * rate;
        const double dist = std::abs(kGutterWidth + m_view.xAt(sample) - x);
        if (dist <= bestDist) {
            bestDist = dist;
            best = a.id;
        }
    }
    return best;
}

int WaveformWidget::cursorNear(double x) const
{
    double bestDist = kGrabPx + 1e-6;
    int best = 0;
    if (m_cursorA && std::abs(xOfSample(static_cast<double>(*m_cursorA)) - x) <= bestDist) {
        bestDist = std::abs(xOfSample(static_cast<double>(*m_cursorA)) - x);
        best = 1;
    }
    if (m_cursorB && std::abs(xOfSample(static_cast<double>(*m_cursorB)) - x) <= bestDist)
        best = 2;
    return best;
}

QRectF WaveformWidget::overviewRect() const
{
    return QRectF(kGutterWidth, 0, std::max(1, width() - kGutterWidth), kOverviewHeight);
}

double WaveformWidget::overviewX(double sample) const
{
    const auto r = overviewRect();
    return r.left() + sample / static_cast<double>(std::max<std::size_t>(1, m_buffer.size())) * r.width();
}

void WaveformWidget::jumpFromOverview(double x)
{
    const auto r = overviewRect();
    const double frac = std::clamp((x - r.left()) / r.width(), 0.0, 1.0);
    centerOn(static_cast<std::size_t>(frac * static_cast<double>(m_buffer.size() - 1)));
}

void WaveformWidget::drawOverview(QPainter& p) const
{
    const QRectF r = overviewRect();
    p.fillRect(QRectF(0, 0, width(), kOverviewHeight), QColor(20, 22, 27));
    p.setPen(kText);
    p.drawText(QRectF(6, 0, kGutterWidth - 12, kOverviewHeight), Qt::AlignVCenter | Qt::AlignLeft, tr("Overview"));

    // Edge density: transitions per column over the whole capture, on every channel that is drawn.
    const int cols = static_cast<int>(r.width());
    if (m_densityWidth != cols) {
        m_density.assign(static_cast<std::size_t>(cols), 0);
        uint32_t mask = 0;
        for (const auto ch : m_rows)
            if (ch < 32)
                mask |= 1u << ch;
        const auto& samples = m_buffer.samples();
        for (std::size_t i = 1; i < samples.size(); ++i)
            if ((samples[i] ^ samples[i - 1]) & mask)
                ++m_density[std::min<std::size_t>(static_cast<std::size_t>(cols) - 1,
                                                        i * static_cast<std::size_t>(cols) / samples.size())];
        m_densityWidth = cols;
    }
    const uint32_t peak = m_density.empty() ? 0 : *std::max_element(m_density.begin(), m_density.end());
    if (peak > 0) {
        p.setPen(QColor(90, 120, 170));
        for (int c = 0; c < cols; ++c) {
            const uint32_t d = m_density[static_cast<std::size_t>(c)];
            if (!d)
                continue;
            const double h = std::max(2.0, (kOverviewHeight - 4) * std::sqrt(static_cast<double>(d) / peak));
            p.drawLine(QPointF(r.left() + c + 0.5, kOverviewHeight - 2), QPointF(r.left() + c + 0.5, kOverviewHeight - 2 - h));
        }
    }

    const double trigger = static_cast<double>(m_buffer.triggerIndex());
    if (trigger < static_cast<double>(m_buffer.size())) {
        p.setPen(QPen(kTrigger, 1));
        p.drawLine(QPointF(overviewX(trigger), 0), QPointF(overviewX(trigger), kOverviewHeight));
        p.drawText(QPointF(overviewX(trigger) + 3, 11), "T");
    }
    if (m_cursorA) {
        p.setPen(QPen(kCursorA, 1.5));
        p.drawLine(QPointF(overviewX(static_cast<double>(*m_cursorA)), 0), QPointF(overviewX(static_cast<double>(*m_cursorA)), kOverviewHeight));
    }
    if (m_cursorB) {
        p.setPen(QPen(kCursorB, 1.5));
        p.drawLine(QPointF(overviewX(static_cast<double>(*m_cursorB)), 0), QPointF(overviewX(static_cast<double>(*m_cursorB)), kOverviewHeight));
    }
    const double x0 = overviewX(m_view.start());
    const double x1 = overviewX(m_view.start() + m_view.visibleSamples());
    const QRectF box(x0, 0.5, std::max(2.0, x1 - x0), kOverviewHeight - 1.0);
    p.fillRect(box, QColor(255, 255, 255, 36));
    p.setPen(QPen(QColor(220, 225, 235), 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(box);
    p.setPen(kGrid);
    p.drawLine(QPointF(0, kOverviewHeight - 0.5), QPointF(width(), kOverviewHeight - 0.5));
}

// ---------------------------------------------------------------------------------------------
// Painting

void WaveformWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    const QRectF plot = plotRect();

    if (!hasCapture()) {
        return;  // the empty-state card (child widget) explains what to do
    }

    const double rh = rowHeight();
    p.save();
    p.setClipRect(QRectF(0, kPlotTop, width(), height() - kPlotTop));  // rows scroll under the pinned header
    for (std::size_t r = 0; r < m_rows.size(); ++r) {
        const QRectF row(0, kPlotTop + static_cast<double>(r) * rh - m_scrollY, width(), rh);
        if (row.top() >= height())
            break;
        if (row.bottom() <= kPlotTop)
            continue;
        if (r % 2)
            p.fillRect(row, kRowAlt);
        drawChannel(p, plot, m_rows[r], row);
    }
    p.restore();
    if (const double maxScroll = maxScrollY(); maxScroll > 0) {  // scroll indicator
        const double track = plot.height();
        const double thumb = std::max(16.0, track * track / (track + maxScroll));
        const double top = kPlotTop + (track - thumb) * (m_scrollY / maxScroll);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(200, 205, 215, 110));
        p.drawRoundedRect(QRectF(width() - 6, top, 4, thumb), 2, 2);
    }
    drawOverview(p);
    drawRuler(p, plot);
    drawAnnotations(p, plot);
    drawAnomalyMarkers(p, plot);
    drawCursors(p, plot);

    if (m_displayState != DisplayState::Live) {
        if (m_displayState == DisplayState::Armed) {
            QColor veil = kBackground;
            veil.setAlpha(153);  // the old waveform stays visible at ~40%
            p.fillRect(QRectF(0, kPlotTop, width(), height() - kPlotTop), veil);
        }
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        const QString text = m_banner.isEmpty() ? (m_displayState == DisplayState::Armed ? tr("ARMED") : tr("STALE"))
                                                : m_banner;
        const QRectF box = QRectF(plot.left() + 8, kPlotTop + 6, QFontMetrics(bold).horizontalAdvance(text) + 20, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(m_displayState == DisplayState::Armed ? QColor(230, 160, 40) : QColor(200, 70, 60));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(20, 20, 20));
        p.drawText(box, Qt::AlignCenter, text);
    }
}

void WaveformWidget::drawChannel(QPainter& p, const QRectF& plot, std::size_t ch, const QRectF& row) const
{
    // Gutter: "CHx (GPx) Name" plus an activity dot (green = the line toggles in this capture).
    const auto& info = m_buffer.channels()[ch];
    QString title = QString("CH%1").arg(info.displayNumber());
    if (const auto gp = lt::gpioForChannel(m_board, info.number))
        title += QString(" (GP%1)").arg(static_cast<int>(*gp));
    const QString label = QString::fromStdString(info.label());
    if (!label.isEmpty() && label != QString("CH%1").arg(info.displayNumber()) &&
        label != QString::number(info.displayNumber()))
        title += " " + label;
    const bool active = channelActive(ch);
    p.setPen(active ? kText : QColor(130, 135, 145));
    p.drawText(QRectF(6, row.top(), kGutterWidth - 28, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(font()).elidedText(title, Qt::ElideRight, kGutterWidth - 28));
    p.setPen(Qt::NoPen);
    p.setBrush(active ? QColor(90, 200, 120) : QColor(80, 84, 94));
    p.drawEllipse(QPointF(kGutterWidth - 12, row.center().y()), 4.0, 4.0);

    const double pad = std::min(5.0, row.height() * 0.2);
    const double yHigh = row.top() + pad, yLow = row.bottom() - pad;
    p.setPen(QPen(kGrid, 1, Qt::DotLine));
    p.drawLine(QPointF(plot.left(), yLow), QPointF(plot.right(), yLow));

    const std::size_t first = m_view.firstVisible(), end = m_view.endVisible();
    if (end <= first)
        return;

    p.save();
    p.setClipRect(plot.adjusted(0, 0, 0, 0).intersected(QRectF(plot.left(), row.top(), plot.width(), row.height())));
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(channelColor(ch), 1.5));
    const auto level = [&](std::size_t i) { return m_buffer.bit(i, ch); };
    const auto yOf = [&](bool high) { return high ? yHigh : yLow; };

    if (m_view.samplesPerPixel() <= 1.0) {
        // Zoomed in: exact steps, one segment per sample run.
        QPainterPath path;
        bool prev = level(first);
        double x = plot.left() + m_view.xAt(static_cast<double>(first));
        path.moveTo(x, yOf(prev));
        for (std::size_t i = first + 1; i < end; ++i) {
            const bool cur = level(i);
            if (cur != prev) {
                const double xe = plot.left() + m_view.xAt(static_cast<double>(i));
                path.lineTo(xe, yOf(prev));
                path.lineTo(xe, yOf(cur));
                prev = cur;
            }
        }
        path.lineTo(plot.left() + m_view.xAt(static_cast<double>(end)), yOf(prev));
        p.drawPath(path);
    } else {
        // Zoomed out: per pixel column, remember whether the line was high, low or both.
        const int cols = static_cast<int>(plot.width());
        std::vector<uint8_t> state(static_cast<std::size_t>(cols), 0);  // bit0 low, bit1 high
        for (std::size_t i = first; i < end; ++i) {
            const int c = static_cast<int>(m_view.xAt(static_cast<double>(i)));
            if (c >= 0 && c < cols)
                state[static_cast<std::size_t>(c)] |= level(i) ? 2 : 1;
        }
        int prevLevel = -1;
        for (int c = 0; c < cols; ++c) {
            const uint8_t s = state[static_cast<std::size_t>(c)];
            if (!s)
                continue;
            const double x = plot.left() + c + 0.5;
            if (s == 3) {
                p.drawLine(QPointF(x, yHigh), QPointF(x, yLow));
                prevLevel = -1;
                continue;
            }
            const bool high = s == 2;
            p.drawLine(QPointF(x - 0.5, yOf(high)), QPointF(x + 0.5, yOf(high)));
            if (prevLevel >= 0 && prevLevel != static_cast<int>(high))
                p.drawLine(QPointF(x - 0.5, yHigh), QPointF(x - 0.5, yLow));
            prevLevel = high;
        }
    }
    p.restore();
}

void WaveformWidget::drawRuler(QPainter& p, const QRectF& plot) const
{
    p.fillRect(QRectF(0, kOverviewHeight, width(), kRulerHeight), kBackground);
    p.setPen(kGrid);
    p.drawLine(QPointF(0, kPlotTop - 0.5), QPointF(width(), kPlotTop - 0.5));

    const double period = m_buffer.samplePeriodSeconds();
    if (period <= 0)
        return;
    const double secondsPerPixel = m_view.samplesPerPixel() * period;
    const double step = niceStep(secondsPerPixel * 90.0);  // aim for ~90 px between labelled ticks
    const double t0 = m_buffer.timeOfSample(0) + m_view.start() * period;
    const double t1 = t0 + plot.width() * secondsPerPixel;

    p.save();
    p.setClipRect(QRectF(plot.left(), kOverviewHeight, plot.width(), kRulerHeight));
    int ticks = 0;  // bounded: with extreme offsets t + step may equal t and never advance
    for (double t = std::ceil(t0 / step) * step; t <= t1 && ticks < 4096; t += step, ++ticks) {
        if (t + step == t)
            break;
        const double sample = t / period + static_cast<double>(m_buffer.triggerIndex());
        const double x = plot.left() + m_view.xAt(sample);
        p.setPen(kGrid);
        p.drawLine(QPointF(x, kPlotTop - 8), QPointF(x, height()));
        p.setPen(kText);
        p.drawText(QPointF(x + 3, kPlotTop - 12), QString::fromStdString(lt::formatTime(std::abs(t) < step * 1e-6 ? 0.0 : t)));
    }
    p.restore();
}

void WaveformWidget::drawCursors(QPainter& p, const QRectF& plot) const
{
    p.save();
    p.setClipRect(QRectF(plot.left() - 1, kOverviewHeight, plot.width() + 2, height() - kOverviewHeight));

    const auto xOfSample = [&](double s) { return plot.left() + m_view.xAt(s); };

    // Trigger marker (t = 0).
    const double trigger = static_cast<double>(m_buffer.triggerIndex());
    if (trigger >= 0 && trigger < static_cast<double>(m_buffer.size())) {
        const double x = plot.left() + m_view.xAt(trigger);
        p.setPen(QPen(kTrigger, 1, Qt::DashLine));
        p.drawLine(QPointF(x, kPlotTop), QPointF(x, height()));
        p.drawText(QPointF(x + 3, kPlotTop + 12), "T");
    }

    auto line = [&](std::optional<std::size_t> c, const QColor& color, const char* label) {
        if (!c)
            return;
        const double x = xOfSample(static_cast<double>(*c));
        p.setPen(QPen(color, 1.5));
        p.drawLine(QPointF(x, kOverviewHeight), QPointF(x, height()));
        p.drawText(QPointF(x + 3, kOverviewHeight + 12), label);
    };
    line(m_cursorA, kCursorA, "A");
    line(m_cursorB, kCursorB, "B");

    if (m_cursorA && m_cursorB) {
        const double xa = xOfSample(static_cast<double>(*m_cursorA)), xb = xOfSample(static_cast<double>(*m_cursorB));
        const double dt = m_buffer.timeOfSample(*m_cursorB) - m_buffer.timeOfSample(*m_cursorA);
        QString text = QString("Δ %1").arg(QString::fromStdString(lt::formatTime(std::abs(dt))));
        if (dt != 0.0)
            text += QString("  (%1)").arg(QString::fromStdString(lt::formatHz(1.0 / std::abs(dt))));
        // Measurement bar just below the ruler, on a backing so it stays readable over the waveforms.
        const double y = kPlotTop + 26;
        const QFontMetrics fm(p.font());
        const QRectF label(std::min(xa, xb) + 6, y - fm.ascent() - 3, fm.horizontalAdvance(text) + 8, fm.height() + 4);
        p.fillRect(label, QColor(24, 26, 31, 220));
        p.setPen(QPen(kText, 1));
        p.drawLine(QPointF(xa, y), QPointF(xb, y));
        p.drawText(label.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    p.restore();
}

void WaveformWidget::drawAnnotations(QPainter& p, const QRectF& plot) const
{
    if (m_annotations.empty() || m_buffer.size() == 0)
        return;
    p.save();
    p.setClipRect(QRectF(plot.left(), kPlotTop, plot.width(), height() - kPlotTop));
    const QFontMetrics fm(p.font());
    const double rate = m_buffer.frequency();
    for (std::size_t i = 0; i < m_annotations.size(); ++i) {
        const auto& a = m_annotations[i];
        QColor color(QString::fromStdString(a.color));
        if (!color.isValid())
            color = QColor(Qt::red);
        const double sample = static_cast<double>(m_buffer.triggerIndex()) + a.timeSeconds * rate;
        const double x = plot.left() + m_view.xAt(sample);
        if (a.durationSeconds > 0) {
            const double x2 = plot.left() + m_view.xAt(sample + a.durationSeconds * rate);
            QColor span = color;
            span.setAlpha(40);
            p.fillRect(QRectF(x, kPlotTop, std::max(1.0, x2 - x), height() - kPlotTop), span);
        }
        p.setPen(QPen(color, 1.5, Qt::DashDotLine));
        p.drawLine(QPointF(x, kPlotTop), QPointF(x, height()));

        // Tags stagger over three lines so neighbouring markers stay readable.
        const QString text = QString("#%1 %2").arg(a.id).arg(QString::fromStdString(a.label));
        const double ty = kPlotTop + 44 + static_cast<double>(i % 3) * (fm.height() + 3);
        const QRectF tag(x + 3, ty, fm.horizontalAdvance(text) + 8, fm.height() + 2);
        QColor back = color.darker(300);
        back.setAlpha(230);
        p.fillRect(tag, back);
        p.setPen(Qt::white);
        p.drawText(tag.adjusted(4, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, text);
    }
    p.restore();
}

// ---------------------------------------------------------------------------------------------
// Interaction

void WaveformWidget::resizeEvent(QResizeEvent* event)
{
    QWidget::resizeEvent(event);
    updateEmptyCard();
    clampScroll();
    const bool wasFit = m_view.isFit();
    const double firstSample = m_view.start(), visible = m_view.visibleSamples();
    updateViewWidth();
    if (wasFit)
        m_view.fitAll();
    else
        m_view.showRange(firstSample, firstSample + visible);  // keep the same window of samples
    Q_EMIT viewChanged();
}

void WaveformWidget::wheelEvent(QWheelEvent* event)
{
    if (!hasCapture()) {
        event->ignore();
        return;
    }
    const auto delta = event->angleDelta();
    const double steps = (delta.y() != 0 ? delta.y() : delta.x()) / 120.0;  // Shift may swap the axis
    if (steps == 0.0) {
        event->ignore();
        return;
    }
    if (event->modifiers() & Qt::ShiftModifier) {
        if (event->modifiers() & Qt::ControlModifier)
            zoomRows(std::pow(1.15, steps));
        else
            setScrollY(m_scrollY - steps * rowHeight());
        Q_EMIT viewChanged();
        event->accept();
        return;
    }
    if (event->modifiers() & Qt::ControlModifier) {
        const double anchor = std::clamp(event->position().x() - kGutterWidth, 0.0, plotRect().width());
        m_view.zoomAt(std::pow(1.25, steps), anchor);
    } else {
        m_view.panByPixels(steps * plotRect().width() * 0.1);  // plain wheel scrolls horizontally
    }
    update();
    Q_EMIT viewChanged();
    event->accept();
}

void WaveformWidget::mousePressEvent(QMouseEvent* event)
{
    setFocus();
    const QPointF pos = event->position();
    if (event->button() == Qt::RightButton) {
        if (hasCapture()) {
            Q_EMIT contextMenuRequested(event->globalPosition().toPoint(), sampleAtX(pos.x()),
                                        plotRect().contains(pos) ? annotationNear(pos.x()) : 0);
            event->accept();
        } else {
            QWidget::mousePressEvent(event);
        }
        return;
    }
    if (!hasCapture() || event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressed = true;
    m_dragging = false;
    m_dragCursor = 0;
    m_overviewDrag = false;
    m_pressButton = event->button();
    m_pressModifiers = event->modifiers();
    m_pressPos = m_lastPos = pos;
    if (pos.y() < kOverviewHeight && pos.x() >= kGutterWidth) {
        m_overviewDrag = true;
        jumpFromOverview(pos.x());
    } else if (plotRect().contains(pos) && !(event->modifiers() & Qt::ShiftModifier)) {
        m_dragCursor = cursorNear(pos.x());  // grabbing a cursor line must not pan
    }
}

void WaveformWidget::mouseMoveEvent(QMouseEvent* event)
{
    const QPointF pos = event->position();
    if (m_pressed && m_pressButton == Qt::LeftButton) {
        if (m_overviewDrag) {
            jumpFromOverview(pos.x());
            return;
        }
        if (!m_dragging && std::abs(pos.x() - m_pressPos.x()) > kDragThreshold) {
            m_dragging = true;
            setCursor(m_dragCursor ? Qt::SizeHorCursor : Qt::ClosedHandCursor);
        }
        if (m_dragging) {
            if (m_dragCursor) {
                const auto s = sampleAtPosition(pos, !(event->modifiers() & Qt::AltModifier));
                if (m_dragCursor == 1)
                    setCursorA(s);
                else
                    setCursorB(s);
            } else {
                m_view.panByPixels(pos.x() - m_lastPos.x());
                update();
                Q_EMIT viewChanged();
            }
            m_lastPos = pos;
            return;
        }
    }
    m_lastPos = pos;

    const QRectF plot = plotRect();
    if (!m_pressed) {
        if (hasCapture() && plot.contains(pos) && cursorNear(pos.x()))
            setCursor(Qt::SizeHorCursor);
        else
            unsetCursor();
    }
    if (hasCapture() && plot.contains(pos)) {
        const auto s = sampleAtX(pos.x());
        m_hoverSample = s;
        if (const auto row = rowAtY(pos.y())) {
            const auto ch = m_rows[*row];
            Q_EMIT hoverChanged(true, m_buffer.timeOfSample(s), m_buffer.channels()[ch].displayNumber(), m_buffer.bit(s, ch));
        } else {
            Q_EMIT hoverChanged(true, m_buffer.timeOfSample(s), 0, -1);
        }
    } else {
        Q_EMIT hoverChanged(false, 0.0, 0, -1);
    }
}

void WaveformWidget::mouseReleaseEvent(QMouseEvent* event)
{
    if (!m_pressed) {
        QWidget::mouseReleaseEvent(event);
        return;
    }
    const bool wasDrag = m_dragging || m_overviewDrag;
    m_pressed = m_dragging = m_overviewDrag = false;
    m_dragCursor = 0;
    unsetCursor();
    if (wasDrag || !plotRect().contains(event->position()))
        return;
    // A click places a cursor: left = A, Shift+left = B. Hold Alt to bypass edge snap.
    const bool allowSnap = !(m_pressModifiers & Qt::AltModifier);
    const auto s = sampleAtPosition(event->position(), allowSnap);
    if (m_pressModifiers & Qt::ShiftModifier)
        setCursorB(s);
    else
        setCursorA(s);
}

void WaveformWidget::mouseDoubleClickEvent(QMouseEvent* event)
{
    if (event->button() == Qt::LeftButton && hasCapture())
        fitAll();
}

void WaveformWidget::contextMenuEvent(QContextMenuEvent* event)
{
    if (event->reason() == QContextMenuEvent::Keyboard) {
        Q_EMIT contextMenuRequested(event->globalPos(), m_hoverSample, 0);
        event->accept();
        return;
    }
    event->ignore();
}

QString WaveformWidget::hintAt(const QPointF& pos) const
{
    if (!hasCapture())
        return {};
    if (pos.y() < kOverviewHeight)
        return tr("Overview of the whole capture.\nClick or drag to jump; the box is the visible window.");
    if (pos.x() < kGutterWidth)
        return tr("CHx (GPx) name; green dot = the line toggles in this capture.\nH hides inactive channels.");
    if (cursorNear(pos.x()))
        return tr("Drag to move this cursor (hold Alt to bypass edge snap).");
    if (pos.y() < kPlotTop)
        return tr("Time ruler relative to the trigger.\nCtrl+wheel zooms around the pointer.\n"
                  "Triangles mark automatic anomalies (F3 jumps to the next one).");
    return tr("Ctrl+wheel: zoom at pointer  |  Wheel: scroll  |  Drag: pan\n"
              "Shift+wheel: scroll channels  |  Ctrl+Shift+wheel: row height\n"
              "Click: Cursor A  |  Shift+click: Cursor B  |  Drag a cursor line: move it\n"
              "Right-click: menu  |  Double-click / Home: fit all  |  +/-: zoom  |  H: hide inactive");
}

bool WaveformWidget::event(QEvent* e)
{
    if (e->type() == QEvent::ToolTip) {
        const auto* help = static_cast<QHelpEvent*>(e);
        const QString text = hintAt(help->pos());
        if (text.isEmpty())
            QToolTip::hideText();
        else
            QToolTip::showText(help->globalPos(), text, this);
        return true;
    }
    return QWidget::event(e);
}

void WaveformWidget::leaveEvent(QEvent*)
{
    Q_EMIT hoverChanged(false, 0.0, 0, -1);
}

void WaveformWidget::keyPressEvent(QKeyEvent* event)
{
    switch (event->key()) {
    case Qt::Key_Plus:
    case Qt::Key_Equal: zoomIn(); break;
    case Qt::Key_Minus: zoomOut(); break;
    case Qt::Key_Home: fitAll(); break;
    case Qt::Key_Left:
        m_view.panByPixels(plotRect().width() * 0.1);
        update();
        Q_EMIT viewChanged();
        break;
    case Qt::Key_Right:
        m_view.panByPixels(-plotRect().width() * 0.1);
        update();
        Q_EMIT viewChanged();
        break;
    default: QWidget::keyPressEvent(event); return;
    }
    event->accept();
}

}  // namespace lt::gui
