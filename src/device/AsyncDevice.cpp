// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Mykola Fedchyk

#include "device/AsyncDevice.hpp"

#include <QMetaObject>
#include <QThread>

#include <memory>
#include <mutex>

namespace lt {

// Lives on the worker thread. Every slot runs there, so the QSerialPort inside PicoConnection is
// created, used and destroyed on a single thread.
class AsyncWorker : public QObject {
    Q_OBJECT

public:
    void requestCancel()
    {
        std::lock_guard lock(m_mutex);
        if (m_conn)
            m_conn->requestCancel();
    }

    std::optional<std::chrono::milliseconds> pollLatencyOverride;

    // Run on the worker thread; AsyncDevice queues them as type-safe functors (no string-based
    // invokeMethod, so arguments need no metatype registration).
    void doOpen(const QString& port, int handshakeMs)
    {
        DeviceInfo info;
        QString error;
        bool ok = false;
        {
            std::lock_guard lock(m_mutex);
            m_conn = std::make_unique<PicoConnection>();
            if (pollLatencyOverride)
                m_conn->setPollLatency(*pollLatencyOverride);
            m_conn->setProgressCallback([this](CaptureStage s) { Q_EMIT progress(static_cast<int>(s)); });
        }
        ok = m_conn->open(port.toStdString(), std::chrono::milliseconds(handshakeMs));
        if (ok)
            info = m_conn->info();
        else
            error = QString::fromStdString(m_conn->lastError());
        Q_EMIT opened(ok, error, info);
    }

    void doClose()
    {
        std::unique_ptr<PicoConnection> dying;
        {
            std::lock_guard lock(m_mutex);
            dying = std::move(m_conn);
        }
        dying.reset();
        Q_EMIT closed();
    }

    void doCapture(const lt::CaptureConfig& config)
    {
        CaptureResult r;
        if (m_conn)
            r = m_conn->capture(config);
        else
            r.error = "device is not connected";
        Q_EMIT captureFinished(r);
    }

    void doProbe(const std::vector<lt::ChannelInfo>& channels, int windowMs, uint32_t frequency)
    {
        ProbeResult r;
        if (m_conn)
            r = m_conn->probe(channels, std::chrono::milliseconds(windowMs), frequency);
        else
            r.error = "device is not connected";
        Q_EMIT probeFinished(r);
    }

Q_SIGNALS:
    void opened(bool ok, const QString& error, const lt::DeviceInfo& info);
    void closed();
    void progress(int stage);
    void captureFinished(const lt::CaptureResult& result);
    void probeFinished(const lt::ProbeResult& result);

private:
    std::mutex m_mutex;
    std::unique_ptr<PicoConnection> m_conn;
};

AsyncDevice::AsyncDevice(QObject* parent) : QObject(parent)
{
    qRegisterMetaType<lt::DeviceInfo>();
    qRegisterMetaType<lt::CaptureConfig>();
    qRegisterMetaType<lt::CaptureResult>();
    qRegisterMetaType<lt::ProbeResult>();

    m_thread = new QThread(this);
    m_thread->setObjectName("logictracer-io");
    m_worker = new AsyncWorker;
    m_worker->moveToThread(m_thread);

    connect(m_worker, &AsyncWorker::opened, this, &AsyncDevice::onOpened);
    connect(m_worker, &AsyncWorker::closed, this, &AsyncDevice::closed);
    connect(m_worker, &AsyncWorker::progress, this, &AsyncDevice::progress);
    connect(m_worker, &AsyncWorker::captureFinished, this, &AsyncDevice::onCaptureFinished);
    connect(m_worker, &AsyncWorker::probeFinished, this, &AsyncDevice::onProbeFinished);
    m_thread->start();
}

AsyncDevice::~AsyncDevice()
{
    m_worker->requestCancel();
    // Tear the connection down on its own thread, then stop the thread.
    QMetaObject::invokeMethod(m_worker, &AsyncWorker::doClose, Qt::BlockingQueuedConnection);
    m_thread->quit();
    m_thread->wait();
    delete m_worker;
}

void AsyncDevice::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    Q_EMIT busyChanged(busy);
}

bool AsyncDevice::open(const QString& port, int handshakeTimeoutMs)
{
    if (m_busy)
        return false;
    setBusy(true);
    m_worker->pollLatencyOverride = m_pollLatencyOverride;
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, port, handshakeTimeoutMs] { worker->doOpen(port, handshakeTimeoutMs); },
        Qt::QueuedConnection);
    return true;
}

void AsyncDevice::close()
{
    m_worker->requestCancel();
    m_connected = false;
    QMetaObject::invokeMethod(m_worker, &AsyncWorker::doClose, Qt::QueuedConnection);
}

bool AsyncDevice::startCapture(const lt::CaptureConfig& config)
{
    if (m_busy || !m_connected)
        return false;
    setBusy(true);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, config] { worker->doCapture(config); }, Qt::QueuedConnection);
    return true;
}

bool AsyncDevice::startProbe(const std::vector<lt::ChannelInfo>& channels, int windowMs, uint32_t frequency)
{
    if (m_busy || !m_connected)
        return false;
    setBusy(true);
    QMetaObject::invokeMethod(
        m_worker, [worker = m_worker, channels, windowMs, frequency] { worker->doProbe(channels, windowMs, frequency); },
        Qt::QueuedConnection);
    return true;
}

void AsyncDevice::requestCancel()
{
    if (m_busy)
        m_worker->requestCancel();
}

void AsyncDevice::onOpened(bool ok, const QString& error, const lt::DeviceInfo& info)
{
    m_connected = ok;
    if (ok)
        m_info = info;
    setBusy(false);
    Q_EMIT opened(ok, error);
}

void AsyncDevice::onCaptureFinished(const lt::CaptureResult& result)
{
    if (result.portLost && m_connected) {
        m_connected = false;
        QMetaObject::invokeMethod(m_worker, &AsyncWorker::doClose, Qt::QueuedConnection);
    }
    setBusy(false);
    Q_EMIT captureFinished(result);
}

void AsyncDevice::onProbeFinished(const lt::ProbeResult& result)
{
    if (result.portLost && m_connected) {
        m_connected = false;
        QMetaObject::invokeMethod(m_worker, &AsyncWorker::doClose, Qt::QueuedConnection);
    }
    setBusy(false);
    Q_EMIT probeFinished(result);
}

}  // namespace lt

#include "AsyncDevice.moc"
