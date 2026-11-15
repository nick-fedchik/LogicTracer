// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "gui/WaveformWidget.hpp"

#include "core/Format.hpp"

#include <QContextMenuEvent>
#include <QFontMetrics>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QWheelEvent>

#include <algorithm>
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
}

QSize WaveformWidget::minimumSizeHint() const
{
    const int rows = std::max<int>(1, static_cast<int>(m_buffer.channels().size()));
    return {kGutterWidth + 200, kRulerHeight + rows * kMinRowHeight};
}

QRectF WaveformWidget::plotRect() const
{
    return QRectF(kGutterWidth, kRulerHeight, std::max(1, width() - kGutterWidth), std::max(1, height() - kRulerHeight));
}

double WaveformWidget::rowHeight() const
{
    const auto n = std::max<std::size_t>(1, m_buffer.channels().size());
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
    m_cursorA.reset();
    m_cursorB.reset();
    m_annotations.clear();  // annotation times are relative to the trigger of the capture they were made on
    m_view.setTotalSamples(m_buffer.size());
    updateViewWidth();
    m_view.fitAll();
    updateGeometry();
    update();
    Q_EMIT captureChanged();
    Q_EMIT viewChanged();
    Q_EMIT cursorsChanged();
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

    const auto clickedCh = pos.y() >= kRulerHeight
                               ? static_cast<std::size_t>((pos.y() - kRulerHeight) / rowHeight())
                               : m_buffer.channels().size();

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

// ---------------------------------------------------------------------------------------------
// Painting

void WaveformWidget::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    p.fillRect(rect(), kBackground);
    const QRectF plot = plotRect();

    if (!hasCapture()) {
        p.setPen(kText);
        p.drawText(rect(), Qt::AlignCenter, tr("No capture.\nConnect a device and press Capture."));
        return;
    }

    const double rh = rowHeight();
    for (std::size_t ch = 0; ch < m_buffer.channels().size(); ++ch) {
        const QRectF row(0, kRulerHeight + static_cast<double>(ch) * rh, width(), rh);
        if (row.top() >= height())
            break;
        if (ch % 2)
            p.fillRect(row, kRowAlt);
        drawChannel(p, plot, ch, row);
    }
    drawRuler(p, plot);
    drawAnnotations(p, plot);
    drawCursors(p, plot);

    if (m_displayState != DisplayState::Live) {
        if (m_displayState == DisplayState::Armed) {
            QColor veil = kBackground;
            veil.setAlpha(153);  // the old waveform stays visible at ~40%
            p.fillRect(QRectF(0, kRulerHeight, width(), height() - kRulerHeight), veil);
        }
        QFont bold = font();
        bold.setBold(true);
        p.setFont(bold);
        const QString text = m_banner.isEmpty() ? (m_displayState == DisplayState::Armed ? tr("ARMED") : tr("STALE"))
                                                : m_banner;
        const QRectF box = QRectF(plot.left() + 8, kRulerHeight + 6, QFontMetrics(bold).horizontalAdvance(text) + 20, 24);
        p.setPen(Qt::NoPen);
        p.setBrush(m_displayState == DisplayState::Armed ? QColor(230, 160, 40) : QColor(200, 70, 60));
        p.drawRoundedRect(box, 4, 4);
        p.setPen(QColor(20, 20, 20));
        p.drawText(box, Qt::AlignCenter, text);
    }
}

void WaveformWidget::drawChannel(QPainter& p, const QRectF& plot, std::size_t ch, const QRectF& row) const
{
    // Gutter: channel number and name.
    p.setPen(kText);
    p.drawText(QRectF(6, row.top(), kGutterWidth - 12, row.height()), Qt::AlignVCenter | Qt::AlignLeft,
               QFontMetrics(font()).elidedText(
                   QString("%1  %2").arg(m_buffer.channels()[ch].displayNumber()).arg(QString::fromStdString(m_buffer.channels()[ch].label())),
                   Qt::ElideRight, kGutterWidth - 12));

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
    p.fillRect(QRectF(0, 0, width(), kRulerHeight), kBackground);
    p.setPen(kGrid);
    p.drawLine(QPointF(0, kRulerHeight - 0.5), QPointF(width(), kRulerHeight - 0.5));

    const double period = m_buffer.samplePeriodSeconds();
    if (period <= 0)
        return;
    const double secondsPerPixel = m_view.samplesPerPixel() * period;
    const double step = niceStep(secondsPerPixel * 90.0);  // aim for ~90 px between labelled ticks
    const double t0 = m_buffer.timeOfSample(0) + m_view.start() * period;
    const double t1 = t0 + plot.width() * secondsPerPixel;

    p.save();
    p.setClipRect(QRectF(plot.left(), 0, plot.width(), kRulerHeight));
    for (double t = std::ceil(t0 / step) * step; t <= t1; t += step) {
        const double sample = t / period + static_cast<double>(m_buffer.triggerIndex());
        const double x = plot.left() + m_view.xAt(sample);
        p.setPen(kGrid);
        p.drawLine(QPointF(x, kRulerHeight - 8), QPointF(x, height()));
        p.setPen(kText);
        p.drawText(QPointF(x + 3, kRulerHeight - 12), QString::fromStdString(lt::formatTime(std::abs(t) < step * 1e-6 ? 0.0 : t)));
    }
    p.restore();
}

void WaveformWidget::drawCursors(QPainter& p, const QRectF& plot) const
{
    p.save();
    p.setClipRect(QRectF(plot.left() - 1, 0, plot.width() + 2, height()));

    const auto xOfSample = [&](double s) { return plot.left() + m_view.xAt(s + 0.5); };

    // Trigger marker (t = 0).
    const double trigger = static_cast<double>(m_buffer.triggerIndex());
    if (trigger >= 0 && trigger < static_cast<double>(m_buffer.size())) {
        const double x = plot.left() + m_view.xAt(trigger);
        p.setPen(QPen(kTrigger, 1, Qt::DashLine));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, height()));
        p.drawText(QPointF(x + 3, kRulerHeight + 12), "T");
    }

    auto line = [&](std::optional<std::size_t> c, const QColor& color, const char* label) {
        if (!c)
            return;
        const double x = xOfSample(static_cast<double>(*c));
        p.setPen(QPen(color, 1.5));
        p.drawLine(QPointF(x, 0), QPointF(x, height()));
        p.drawText(QPointF(x + 3, 12), label);
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
        const double y = kRulerHeight + 26;
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
    p.setClipRect(QRectF(plot.left(), kRulerHeight, plot.width(), height() - kRulerHeight));
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
            p.fillRect(QRectF(x, kRulerHeight, std::max(1.0, x2 - x), height() - kRulerHeight), span);
        }
        p.setPen(QPen(color, 1.5, Qt::DashDotLine));
        p.drawLine(QPointF(x, kRulerHeight), QPointF(x, height()));

        // Tags stagger over three lines so neighbouring markers stay readable.
        const QString text = QString("#%1 %2").arg(a.id).arg(QString::fromStdString(a.label));
        const double ty = kRulerHeight + 44 + static_cast<double>(i % 3) * (fm.height() + 3);
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
    const double steps = event->angleDelta().y() / 120.0;
    if (steps == 0.0) {
        event->ignore();
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
    if (event->button() == Qt::RightButton &&
        ((event->modifiers() & Qt::ControlModifier) || !plotRect().contains(event->position()))) {
        Q_EMIT contextMenuRequested(event->globalPosition().toPoint());
        event->accept();
        return;
    }
    if (!hasCapture() || (event->button() != Qt::LeftButton && event->button() != Qt::RightButton)) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressed = true;
    m_dragging = false;
    m_pressButton = event->button();
    m_pressModifiers = event->modifiers();
    m_pressPos = m_lastPos = event->position();
}

void WaveformWidget::mouseMoveEvent(QMouseEvent* event)
{
    if (m_pressed && m_pressButton == Qt::LeftButton) {
        if (!m_dragging && std::abs(event->position().x() - m_pressPos.x()) > kDragThreshold) {
            m_dragging = true;
            setCursor(Qt::ClosedHandCursor);
        }
        if (m_dragging) {
            m_view.panByPixels(event->position().x() - m_lastPos.x());
            m_lastPos = event->position();
            update();
            Q_EMIT viewChanged();
            return;
        }
    }
    m_lastPos = event->position();

    const QRectF plot = plotRect();
    if (hasCapture() && plot.contains(event->position())) {
        const auto s = sampleAtX(event->position().x());
        const auto ch = static_cast<std::size_t>((event->position().y() - kRulerHeight) / rowHeight());
        if (ch < m_buffer.channels().size())
            Q_EMIT hoverChanged(true, m_buffer.timeOfSample(s), m_buffer.channels()[ch].displayNumber(), m_buffer.bit(s, ch));
        else
            Q_EMIT hoverChanged(true, m_buffer.timeOfSample(s), 0, -1);
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
    const bool wasDrag = m_dragging;
    m_pressed = m_dragging = false;
    unsetCursor();
    if (wasDrag || !plotRect().contains(event->position()))
        return;
    // A click places a cursor: left = A, right or Shift+left = B. Hold Alt to bypass edge snap.
    const bool allowSnap = !(m_pressModifiers & Qt::AltModifier);
    const auto s = sampleAtPosition(event->position(), allowSnap);
    if (m_pressButton == Qt::RightButton || (m_pressModifiers & Qt::ShiftModifier))
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
        Q_EMIT contextMenuRequested(event->globalPos());
        event->accept();
        return;
    }
    event->ignore();
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
