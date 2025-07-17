#include "fpsmonitor.h"
#include "settings/streamingpreferences.h"
#include "streaming/session.h"
#include <QDateTime>
#include <QEventLoop>
#include <QTimer>
#include <QDebug>
#include <QUrl>
#include <QUrlQuery>
#include <QNetworkRequest>
#include <QNetworkReply>

#ifdef Q_OS_WIN
#include <Windows.h>
#elif defined(CLOCK_MONOTONIC) && !defined(NO_CLOCK_GETTIME)
#include <time.h>
#else
#include "Limelight.h"
#endif

FpsMonitor* FpsMonitor::s_instance = nullptr;

// FpsSenderThread 实现
FpsSenderThread::FpsSenderThread() : m_running(true) {
    qDebug() << "FpsSenderThread: Initializing";
}

FpsSenderThread::~FpsSenderThread() {
    stopThread();
}

void FpsSenderThread::addToQueue(const QString& lineProtocol) {
    QMutexLocker locker(&m_queueMutex);
    m_dataQueue.enqueue(lineProtocol);
    m_queueNotEmpty.wakeOne();
    qDebug() << "FpsSenderThread: Data added to queue, queue size:" << m_dataQueue.size();
}

void FpsSenderThread::stopThread() {
    QMutexLocker locker(&m_queueMutex);
    m_running = false;
    m_queueNotEmpty.wakeOne();
    locker.unlock();
    if (isRunning()) {
        wait();
    }
}

void FpsSenderThread::run() {
    qDebug() << "FpsSenderThread: Thread started";
    QNetworkAccessManager networkManager;
    while (m_running) {
        QString lineProtocol;
        {
            QMutexLocker locker(&m_queueMutex);
            while (m_dataQueue.isEmpty() && m_running) {
                m_queueNotEmpty.wait(&m_queueMutex);
            }
            if (!m_running) break;
            lineProtocol = m_dataQueue.dequeue();
            qDebug() << "FpsSenderThread: Data dequeued, remaining items:" << m_dataQueue.size();
        }
        // 发送数据到InfluxDB
        try {
            QString influxDBUrl = StreamingPreferences::get()->influxDbUrl;
            QString influxDBDatabase = "testDB";
            QString influxDBAuthToken = "apiv3_S9waXgiGOZkGccVT5iuSxDTl_5wrCrJ8cmo7yyl2xKGH5tGnAcjnNwrIrVJK5qpey8ltqcrzmUNvClfhVqdwLg";
            QUrl url(influxDBUrl);
            QString apiPath = "/api/v3/write_lp";
            QUrlQuery query;
            query.addQueryItem("db", influxDBDatabase);
            query.addQueryItem("precision", "auto");
            url.setPath(apiPath);
            url.setQuery(query);
            qDebug() << "FpsSenderThread: Sending FPS data to URL:" << url.toString();
            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
            if (!influxDBAuthToken.isEmpty()) {
                request.setRawHeader("Authorization", QString("Token %1").arg(influxDBAuthToken).toUtf8());
            }
            QEventLoop loop;
            QNetworkReply* reply = networkManager.post(request, lineProtocol.toUtf8());
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, SIGNAL(timeout()), &loop, SLOT(quit()));
            timer.start(5000); // 5秒超时
            loop.exec();
            if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
                qDebug() << "FpsSenderThread: Successfully sent FPS data to InfluxDB";
            } else {
                QString errorString = reply->isFinished() ? reply->errorString() : "Request timed out";
                qWarning() << "FpsSenderThread: Failed to send FPS data to InfluxDB:" << errorString;
                if (reply->isFinished()) {
                    qWarning() << "FpsSenderThread: HTTP Status Code:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    QString responseData = reply->readAll();
                    qWarning() << "FpsSenderThread: Response content:" << responseData;
                }
            }
            reply->deleteLater();
        } catch (const std::exception& e) {
            qWarning() << "FpsSenderThread: Exception while sending FPS data:" << e.what();
        } catch (...) {
            qWarning() << "FpsSenderThread: Unknown exception while sending FPS data";
        }
    }
    qDebug() << "FpsSenderThread: Thread exiting";
}

// FpsMonitor 实现
FpsMonitor* FpsMonitor::instance() {
    if (!s_instance) {
        qDebug() << "FpsMonitor: Creating singleton instance";
        s_instance = new FpsMonitor();
        qDebug() << "FpsMonitor: Singleton instance created";
    }
    return s_instance;
}

void FpsMonitor::destroy() {
    if (s_instance) {
        delete s_instance;
        s_instance = nullptr;
    }
}

FpsMonitor::FpsMonitor() :
    m_timer(nullptr),
    m_receivedFrames(0),
    m_decodedFrames(0),
    m_renderedFrames(0),
    m_startTimestamp(0),
    m_isMonitoring(false),
    m_senderThread(nullptr)
{
    //qDebug() << "FpsMonitor thread:" << QThread::currentThread();
    //qDebug() << "Main thread:" << qApp->thread();
    qDebug() << "FpsMonitor: Initializing";
    m_timer = new QTimer(this);
    m_timer->setSingleShot(false);
    connect(m_timer, &QTimer::timeout, this, &FpsMonitor::onTimerTimeout);
    m_senderThread = new FpsSenderThread();
    m_senderThread->start();
    qDebug() << "FpsMonitor: Sender thread started";
}

FpsMonitor::~FpsMonitor() {
    stopMonitoring();
    if (m_senderThread) {
        m_senderThread->stopThread();
        delete m_senderThread;
        m_senderThread = nullptr;
    }
}

void FpsMonitor::startMonitoring() {
    qDebug() << "FpsMonitor: startMonitoring called";
    qDebug() << "m_timer valid:" << (m_timer != nullptr);
    QMutexLocker locker(&m_mutex);
    if (!m_isMonitoring) {
        qDebug() << "FpsMonitor: Starting FPS monitoring";
        resetCounters();
        m_startTimestamp = getCurrentTimestampNs();
        m_isMonitoring = true;
        m_timer->start(1000);
        qDebug() << "FpsMonitor: timer started, isActive=" << m_timer->isActive();
        qDebug() << "FpsMonitor: FPS monitoring started";
    }else {
        qDebug() << "FpsMonitor: already monitoring";
    }
}

void FpsMonitor::stopMonitoring() {
    qDebug() << "FpsMonitor: stopMonitoring called";
    QMutexLocker locker(&m_mutex);
    if (m_isMonitoring) {
        qDebug() << "FpsMonitor: Stopping FPS monitoring";
        m_timer->stop();
        m_isMonitoring = false;
        qDebug() << "FpsMonitor: FPS monitoring stopped";
        qDebug() << "FpsMonitor: timer stopped, isActive=" << m_timer->isActive();
    }
}

bool FpsMonitor::isMonitoring() const {
    QMutexLocker locker(&m_mutex);
    return m_isMonitoring;
}

void FpsMonitor::recordFrameReceived() {
    QMutexLocker locker(&m_mutex);
    m_receivedFrames++;
}

void FpsMonitor::recordFrameDecoded() {
    QMutexLocker locker(&m_mutex);
    m_decodedFrames++;
}

void FpsMonitor::recordFrameRendered() {
    QMutexLocker locker(&m_mutex);
    m_renderedFrames++;
}

void FpsMonitor::onTimerTimeout() {
    qDebug() << "FpsMonitor: onTimerTimeout called";
    QMutexLocker locker(&m_mutex);
    if (!m_isMonitoring) return;
    qint64 currentTimestamp = getCurrentTimestampNs();
    qint64 elapsedTimeNs = currentTimestamp - m_startTimestamp;
    double elapsedTimeSeconds = elapsedTimeNs / 1000000000.0;
    double receivedFps = elapsedTimeSeconds > 0 ? m_receivedFrames / elapsedTimeSeconds : 0;
    double decodedFps = elapsedTimeSeconds > 0 ? m_decodedFrames / elapsedTimeSeconds : 0;
    double renderedFps = elapsedTimeSeconds > 0 ? m_renderedFrames / elapsedTimeSeconds : 0;
    qDebug() << "FpsMonitor: FPS stats - Received:" << receivedFps << "Decoded:" << decodedFps << "Rendered:" << renderedFps;
    try {
        QString lineProtocol = QString("fps_metrics");
        Session* session = Session::get();
        if (session) {
            if (session->m_Computer && !session->m_Computer->name.isEmpty()) {
                QString computerName = session->m_Computer->name;
                computerName.replace(" ", "\\ ");
                computerName.replace(",", "\\,");
                computerName.replace("=", "\\=");
                lineProtocol += QString(",computer_name=%1").arg(computerName);
            }
            lineProtocol += QString(",app_id=%1").arg(session->m_App.id);
        }
        lineProtocol += QString(" received_fps=%1,decoded_fps=%2,rendered_fps=%3")
            .arg(receivedFps, 0, 'f', 2)
            .arg(decodedFps, 0, 'f', 2)
            .arg(renderedFps, 0, 'f', 2);
        qint64 timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000;
        lineProtocol += QString(" %1\n").arg(timestampNs);
        qDebug() << "FpsMonitor: Built InfluxDB line protocol data:" << lineProtocol;
        sendFpsDataToInfluxDBAsync(lineProtocol);
    } catch (const std::exception& e) {
        qWarning() << "FpsMonitor: Exception while preparing FPS data for InfluxDB:" << e.what();
    } catch (...) {
        qWarning() << "FpsMonitor: Unknown exception while preparing FPS data for InfluxDB";
    }
    resetCounters();
    m_startTimestamp = currentTimestamp;
}

void FpsMonitor::resetCounters() {
    m_receivedFrames = 0;
    m_decodedFrames = 0;
    m_renderedFrames = 0;
}

void FpsMonitor::sendFpsDataToInfluxDBAsync(const QString& lineProtocol) {
    if (m_senderThread) {
        m_senderThread->addToQueue(lineProtocol);
    }
}

qint64 FpsMonitor::getCurrentTimestampNs() {
    qint64 timestamp;
#if defined(Q_OS_WIN)
    LARGE_INTEGER counter, freq;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);
    timestamp = (counter.QuadPart * 1000000000) / freq.QuadPart;
#elif defined(CLOCK_MONOTONIC) && !defined(NO_CLOCK_GETTIME)
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    timestamp = ((qint64)ts.tv_sec * 1000000000) + ts.tv_nsec;
#else
    timestamp = LiGetMillis() * 1000000;
#endif
    return timestamp;
} 