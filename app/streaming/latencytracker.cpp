#include "latencytracker.h"
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrlQuery>
#include <QRandomGenerator>
#include <QEventLoop>
#include <QTimer>
#include <QThread>
#include <QDebug>
#include <QDateTime>
#include <QCoreApplication>
#include "session.h"
#include "../settings/streamingpreferences.h"

#if defined(Q_OS_WIN)
#include <windows.h>
#elif defined(Q_OS_UNIX) || defined(Q_OS_LINUX) || defined(Q_OS_MAC)
#include <time.h>
#endif

// InfluxSenderThread实现
InfluxSenderThread::InfluxSenderThread() : m_running(true)
{
    qDebug() << "InfluxSenderThread: Constructor called";
}

InfluxSenderThread::~InfluxSenderThread()
{
    stopThread();
}

void InfluxSenderThread::addToQueue(const QString& lineProtocol)
{
    QMutexLocker locker(&m_queueMutex);
    m_dataQueue.enqueue(lineProtocol);
    qDebug() << "InfluxSenderThread: Data added to queue, size:" << m_dataQueue.size();
    
    // 通知线程有新数据
    m_queueNotEmpty.wakeOne();
}

void InfluxSenderThread::stopThread()
{
    qDebug() << "InfluxSenderThread: Stopping thread";
    
    {
        QMutexLocker locker(&m_queueMutex);
        m_running = false;
        m_queueNotEmpty.wakeOne();
    }
    
    // 等待线程结束
    wait();
    qDebug() << "InfluxSenderThread: Thread stopped";
}

void InfluxSenderThread::run()
{
    qDebug() << "InfluxSenderThread: Thread started";
    
    // 创建网络访问管理器
    QNetworkAccessManager networkManager;
    qDebug() << "InfluxSenderThread: Network manager created in thread:" << QThread::currentThread();
    
    while (m_running) {
        QString lineProtocol;
        
        // 从队列中获取数据
        {
            QMutexLocker locker(&m_queueMutex);
            
            // 如果队列为空，等待新数据
            if (m_dataQueue.isEmpty()) {
                qDebug() << "InfluxSenderThread: Queue empty, waiting for data";
                m_queueNotEmpty.wait(&m_queueMutex);
                
                // 如果被唤醒是因为线程需要停止，则退出
                if (!m_running) {
                    qDebug() << "InfluxSenderThread: Thread signaled to stop";
                    break;
                }
                
                // 再次检查队列是否为空
                if (m_dataQueue.isEmpty()) {
                    continue;
                }
            }
            
            // 从队列中取出一条数据
            lineProtocol = m_dataQueue.dequeue();
            qDebug() << "InfluxSenderThread: Data dequeued, remaining items:" << m_dataQueue.size();
        }
        
        // 发送数据到InfluxDB
        try {
            // 从设置中获取InfluxDB配置
            QString influxDBUrl = StreamingPreferences::get()->influxDbUrl;
            QString influxDBDatabase = "testDB"; 
            QString influxDBAuthToken = "apiv3_S9waXgiGOZkGccVT5iuSxDTl_5wrCrJ8cmo7yyl2xKGH5tGnAcjnNwrIrVJK5qpey8ltqcrzmUNvClfhVqdwLg";
            
            // 构建URL
            QUrl url(influxDBUrl);
            QString apiPath = "/api/v3/write_lp";
            QUrlQuery query;
            query.addQueryItem("db", influxDBDatabase);
            query.addQueryItem("precision", "auto");
            url.setPath(apiPath);
            url.setQuery(query);
            
            qDebug() << "InfluxSenderThread: Sending data to URL:" << url.toString();
            
            // 创建请求对象
            QNetworkRequest request(url);
            request.setHeader(QNetworkRequest::ContentTypeHeader, "text/plain");
            
            // 添加认证头
            if (!influxDBAuthToken.isEmpty()) {
                request.setRawHeader("Authorization", QString("Token %1").arg(influxDBAuthToken).toUtf8());
            }
            
            // 创建事件循环以同步等待响应
            QEventLoop loop;
            
            // 发送POST请求
            QNetworkReply* reply = networkManager.post(request, lineProtocol.toUtf8());
            
            // 连接信号以在请求完成时退出事件循环
            QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
            
            // 设置超时
            QTimer timer;
            timer.setSingleShot(true);
            QObject::connect(&timer, SIGNAL(timeout()), &loop, SLOT(quit()));
            timer.start(5000); // 5秒超时
            
            // 等待请求完成或超时
            loop.exec();
            
            // 处理响应
            if (reply->isFinished() && reply->error() == QNetworkReply::NoError) {
                qDebug() << "InfluxSenderThread: Successfully sent data to InfluxDB";
            } else {
                QString errorString = reply->isFinished() ? reply->errorString() : "Request timed out";
                qWarning() << "InfluxSenderThread: Failed to send data to InfluxDB:" << errorString;
                
                if (reply->isFinished()) {
                    qWarning() << "InfluxSenderThread: HTTP Status Code:" << reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
                    QString responseData = reply->readAll();
                    qWarning() << "InfluxSenderThread: Response content:" << responseData;
                }
            }
            
            // 清理
            reply->deleteLater(); // QObject的子类都有deleteLater方法
        }
        catch (const std::exception& e) {
            qWarning() << "InfluxSenderThread: Exception while sending data:" << e.what();
        }
        catch (...) {
            qWarning() << "InfluxSenderThread: Unknown exception while sending data";
        }
    }
    
    qDebug() << "InfluxSenderThread: Thread exiting";
}

// 获取高精度时间戳（毫秒）
qint64 LatencyTracker::getHighResolutionTimeMs()
{
    qint64 timestamp;
#if defined(Q_OS_WIN)
    // Windows平台使用QueryPerformanceCounter获取高精度
    LARGE_INTEGER counter, freq;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);
    // 转换为毫秒
    timestamp = (counter.QuadPart * 1000) / freq.QuadPart;
#elif defined(CLOCK_MONOTONIC) && !defined(NO_CLOCK_GETTIME)
    // Linux/Unix平台使用clock_gettime获取高精度
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 转换为毫秒
    timestamp = ((qint64)ts.tv_sec * 1000) + (ts.tv_nsec / 1000000);
#else
    // 其他平台回退到LiGetMillis
    timestamp = LiGetMillis();
#endif
    return timestamp;
}

// 获取高精度时间戳（微秒）
qint64 LatencyTracker::getHighResolutionTimeUs()
{
    qint64 timestamp;
#if defined(Q_OS_WIN)
    // Windows平台使用QueryPerformanceCounter获取高精度
    LARGE_INTEGER counter, freq;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);
    // 转换为微秒
    timestamp = (counter.QuadPart * 1000000) / freq.QuadPart;
#elif defined(CLOCK_MONOTONIC) && !defined(NO_CLOCK_GETTIME)
    // Linux/Unix平台使用clock_gettime获取高精度
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 转换为微秒
    timestamp = ((qint64)ts.tv_sec * 1000000) + (ts.tv_nsec / 1000);
#else
    // 其他平台回退到LiGetMillis，但转换为微秒
    timestamp = LiGetMillis() * 1000;
#endif
    return timestamp;
}

// 获取高精度时间戳（纳秒）
qint64 LatencyTracker::getHighResolutionTimeNs()
{
    qint64 timestamp;
#if defined(Q_OS_WIN)
    // Windows平台使用QueryPerformanceCounter获取高精度
    LARGE_INTEGER counter, freq;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&freq);
    // 转换为纳秒
    timestamp = (counter.QuadPart * 1000000000) / freq.QuadPart;
#elif defined(CLOCK_MONOTONIC) && !defined(NO_CLOCK_GETTIME)
    // Linux/Unix平台使用clock_gettime获取高精度
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    // 转换为纳秒
    timestamp = ((qint64)ts.tv_sec * 1000000000) + ts.tv_nsec;
#else
    // 其他平台回退到LiGetMillis，但转换为纳秒
    timestamp = LiGetMillis() * 1000000;
#endif
    return timestamp;
}

// 初始化静态成员
LatencyTracker* LatencyTracker::s_instance = nullptr;

LatencyTracker* LatencyTracker::instance()
{
    if (!s_instance) {
        qDebug() << "LatencyTracker: Creating singleton instance";
        s_instance = new LatencyTracker();
        qDebug() << "LatencyTracker: Singleton instance created";
    }
    return s_instance;
}

void LatencyTracker::destroy()
{
    if (s_instance) {
        delete s_instance;
        s_instance = nullptr;
    }
}

LatencyTracker::LatencyTracker() : m_currentId(1), m_senderThread(nullptr)
{
    qDebug() << "LatencyTracker: Initializing";
    qDebug() << "LatencyTracker: Constructor thread:" << QThread::currentThread();
    
    // 创建并启动发送线程
    m_senderThread = new InfluxSenderThread();
    m_senderThread->QThread::start(); // 明确调用QThread的start方法
    qDebug() << "LatencyTracker: Sender thread started";
}

LatencyTracker::~LatencyTracker()
{
    // 清理资源
    m_entries.clear();
    
    // 停止并删除发送线程
    if (m_senderThread) {
        m_senderThread->stopThread();
        delete m_senderThread;
        m_senderThread = nullptr;
    }
}

// 异步发送InfluxDB数据
void LatencyTracker::sendToInfluxDBAsync(const QString& lineProtocol)
{
    qDebug() << "LatencyTracker: Sending data to InfluxDB asynchronously";
    
    if (m_senderThread) {
        m_senderThread->addToQueue(lineProtocol);
        qDebug() << "LatencyTracker: Data added to sender thread queue";
    } else {
        qWarning() << "LatencyTracker: Sender thread is null, cannot send data";
    }
}

int LatencyTracker::startTracking(EventType eventType)
{
    QMutexLocker locker(&m_mutex);
    
    // 生成唯一ID (顺序ID)
    int id = m_currentId;
    
    // 更新ID计数器，超过最大值则重置为1
    m_currentId = (m_currentId % MAX_ID) + 1;
    
    // 创建新条目并记录起始时间戳（微秒级精度）
    TimestampEntry entry;
    entry.eventType = eventType;
    entry.timestamps[STAGE_INPUT] = getHighResolutionTimeUs();
    
    m_entries[id] = entry;
    
    qDebug() << "LatencyTracker: Started tracking ID:" << id << "EventType:" << getEventTypeName(eventType) << "at stage:" << getStageName(STAGE_INPUT);
    
    return id;
}

// 使用指定ID开始跟踪
void LatencyTracker::startTrackingWithId(int id, EventType eventType)
{
    QMutexLocker locker(&m_mutex);
    
    // 创建新条目
    TimestampEntry entry;
    entry.eventType = eventType;
    
    m_entries[id] = entry;
    
    qDebug() << "LatencyTracker: Started tracking with specified ID:" << id << "EventType:" << getEventTypeName(eventType);
}

void LatencyTracker::recordTimestamp(int id, TrackingStage stage)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_entries.contains(id)) {
        qint64 timestamp = getHighResolutionTimeUs(); // 使用微秒级精度
        m_entries[id].timestamps[stage] = timestamp;
        qDebug() << "LatencyTracker: Recorded ID:" << id << "at stage:" << getStageName(stage) << "timestamp:" << timestamp << "微秒";
    } else {
        qWarning() << "LatencyTracker: Attempted to record timestamp for unknown ID:" << id;
    }
}

// 记录某个阶段的指定时间戳
void LatencyTracker::recordTimestamp(int id, TrackingStage stage, qint64 timestamp)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_entries.contains(id)) {
        m_entries[id].timestamps[stage] = timestamp;
        qDebug() << "LatencyTracker: Recorded ID:" << id << "at stage:" << getStageName(stage) << "with specified timestamp:" << timestamp;
    } else {
        qWarning() << "LatencyTracker: Attempted to record specified timestamp for unknown ID:" << id;
    }
}

QMap<QString, qint64> LatencyTracker::getLatencies(int id)
{
    QMutexLocker locker(&m_mutex);
    QMap<QString, qint64> latencies;
    
    if (!m_entries.contains(id)) {
        qWarning() << "LatencyTracker: Attempted to get latencies for unknown ID:" << id;
        return latencies;
    }
    
    const TimestampEntry& entry = m_entries[id];
    
    // 计算各阶段之间的延迟
    for (int i = 0; i < STAGE_COUNT - 1; i++) {
        TrackingStage currentStage = static_cast<TrackingStage>(i);
        TrackingStage nextStage = static_cast<TrackingStage>(i + 1);
        
        if (entry.timestamps.contains(currentStage) && entry.timestamps.contains(nextStage)) {
            QString latencyName = QString("%1_to_%2").arg(getStageName(currentStage)).arg(getStageName(nextStage));
            latencies[latencyName] = entry.timestamps[nextStage] - entry.timestamps[currentStage];
        }
    }
    
    // 计算总延迟
    if (entry.timestamps.contains(STAGE_INPUT) && entry.timestamps.contains(STAGE_RENDER)) {
        latencies["total"] = entry.timestamps[STAGE_RENDER] - entry.timestamps[STAGE_INPUT];
    }
    
    return latencies;
}

void LatencyTracker::cleanup(int maxAgeMs)
{
    QMutexLocker locker(&m_mutex);
    qint64 now = QDateTime::currentMSecsSinceEpoch();
    
    QMutableMapIterator<int, TimestampEntry> i(m_entries);
    while (i.hasNext()) {
        i.next();
        
        // 如果最早的时间戳已经超过最大年龄，则删除此条目
        qint64 oldestTimestamp = 0;
        QMapIterator<TrackingStage, qint64> j(i.value().timestamps);
        while (j.hasNext()) {
            j.next();
            if (oldestTimestamp == 0 || j.value() < oldestTimestamp) {
                oldestTimestamp = j.value();
            }
        }
        
        if (now - oldestTimestamp > maxAgeMs) {
            qDebug() << "LatencyTracker: Cleaning up expired entry ID:" << i.key();
            i.remove();
        }
    }
}

QList<int> LatencyTracker::getAllTrackingIds() const
{
    QMutexLocker locker(&m_mutex);
    return m_entries.keys();
}

QMap<LatencyTracker::TrackingStage, qint64> LatencyTracker::getAllTimestamps(int id) const
{
    QMutexLocker locker(&m_mutex);
    
    QMap<TrackingStage, qint64> result;
    auto it = m_entries.constFind(id);
    if (it != m_entries.constEnd()) {
        result = it.value().timestamps;
    }
    
    return result;
}



bool LatencyTracker::hasTrackingId(int id) const
{
    QMutexLocker locker(&m_mutex);
    return m_entries.constFind(id) != m_entries.constEnd();
}

// 记录Sunshine的时间戳（纳秒级别）
void LatencyTracker::recordSunshineTimestamp(int id, TrackingStage stage, int64_t timestampNs)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_entries.contains(id)) {
        // 存储纳秒值，保持原始精度
        m_entries[id].timestamps[stage] = timestampNs;
        qDebug() << "LatencyTracker: Recorded Sunshine timestamp for ID:" << id 
                 << "at stage:" << getStageName(stage) 
                 << "timestamp (ns):" << timestampNs;
    } else {
        qWarning() << "LatencyTracker: Attempted to record Sunshine timestamp for unknown ID:" << id;
    }
}

QString LatencyTracker::getStageName(TrackingStage stage)
{
    switch (stage) {
        case STAGE_INPUT:
            return "input";
        case STAGE_SEND:
            return "send";
        case STAGE_DECODE:
            return "decode";
        case STAGE_DECODE_END:
            return "decode_end";
        case STAGE_PACER_START:
            return "pacer_start";
        case STAGE_PACER_END:
            return "pacer_end";
        case STAGE_RENDER:
            return "render";
        case STAGE_RENDER_END:
            return "render_end";
        case STAGE_SUNSHINE_INPUT_ARRIVAL:
            return "sunshine_input_arrival";
        case STAGE_SUNSHINE_ENCODE_START:
            return "sunshine_encode_start";
        case STAGE_SUNSHINE_ENCODE_END:
            return "sunshine_encode_end";
        case STAGE_FRAME_RECEIVE:
            return "frame_receive";
        case STAGE_FRAME_ENQUEUE:
            return "frame_enqueue";
        default:
            return "unknown";
    }
}

QString LatencyTracker::getEventTypeName(EventType eventType)
{
    switch (eventType) {
        case EVENT_MOUSE_CLICK:
            return "mouse_click";
        case EVENT_KEY_PRESS:
            return "key_press";
        case EVENT_GAMEPAD_BUTTON:
            return "gamepad_button";
        case EVENT_UNKNOWN:
            return "unknown";
        default:
            return "unknown";
    }
} 

// 获取事件类型
LatencyTracker::EventType LatencyTracker::getEventType(int id) const
{
    QMutexLocker locker(&m_mutex);
    
    if (m_entries.contains(id)) {
        return m_entries[id].eventType;
    }
    
    return EVENT_UNKNOWN;
}

// 计算并打印各个阶段的延迟
void LatencyTracker::calculateAndLogLatencies(int id, qint64 pacerTime, qint64 renderTime)
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_entries.contains(id)) {
        qWarning() << "LatencyTracker: Attempted to calculate latencies for unknown ID:" << id;
        return;
    }
    
    // 获取所有时间戳
    QMap<TrackingStage, qint64> timestamps = m_entries[id].timestamps;
    
    // 调试输出所有时间戳
    qDebug() << "LatencyTracker: All timestamps for ID" << id << ": (微秒级精度)";
    for (auto it = timestamps.constBegin(); it != timestamps.constEnd(); ++it) {
        qDebug() << "  Stage:" << getStageName(it.key()) << "Timestamp:" << it.value() << "微秒";
    }
    
    // 计算各个阶段的时间间隔（微秒）
    qint64 inputToSendTime = 0;
    qint64 sunshineInputToEncodeTime = 0;
    qint64 encodeTime = 0;
    qint64 sendToReceiveTime = 0;
    qint64 receiveToDecodeTime = 0;
    qint64 decodeTime = 0;
    qint64 pacerTimeUs = 0;
    qint64 renderTimeUs = 0;
    qint64 sendAndReceiveTime = 0;
    
    // 1. 端侧输入到发送之间 (微秒)
    if (timestamps.contains(STAGE_INPUT) && timestamps.contains(STAGE_SEND)) {
        inputToSendTime = timestamps[STAGE_SEND] - timestamps[STAGE_INPUT];
    }
    
    // 2. sunshine收到输入到编码之间 (纳秒转微秒)
    if (timestamps.contains(STAGE_SUNSHINE_INPUT_ARRIVAL) && 
        timestamps.contains(STAGE_SUNSHINE_ENCODE_START)) {
        // 将纳秒转换为微秒
        qint64 inputArrivalUs = timestamps[STAGE_SUNSHINE_INPUT_ARRIVAL] / 1000;
        qint64 encodeStartUs = timestamps[STAGE_SUNSHINE_ENCODE_START] / 1000;
        sunshineInputToEncodeTime = encodeStartUs - inputArrivalUs;
    }
    
    // 3. 编码开始到编码结束之间 (纳秒转微秒)
    if (timestamps.contains(STAGE_SUNSHINE_ENCODE_START) && 
        timestamps.contains(STAGE_SUNSHINE_ENCODE_END)) {
        // 将纳秒转换为微秒
        qint64 encodeStartUs = timestamps[STAGE_SUNSHINE_ENCODE_START] / 1000;
        qint64 encodeEndUs = timestamps[STAGE_SUNSHINE_ENCODE_END] / 1000;
        encodeTime = encodeEndUs - encodeStartUs;
    }
    
    // 4. 端侧发送输入到端侧解码之间 (微秒)
    if (timestamps.contains(STAGE_SEND) && 
        timestamps.contains(STAGE_DECODE)) {
        sendToReceiveTime = timestamps[STAGE_DECODE] - 
                           timestamps[STAGE_SEND];
    }
    
    // 5. 端侧收到码流到端侧解码之前 (微秒)
    if (timestamps.contains(STAGE_FRAME_RECEIVE) && 
        timestamps.contains(STAGE_DECODE)) {
        receiveToDecodeTime = 0;
        // timestamps[STAGE_DECODE] - 
        //                      timestamps[STAGE_FRAME_RECEIVE];
    }
    
    // 6. 解码前到解码后 (微秒)
    if (timestamps.contains(STAGE_DECODE) && 
        timestamps.contains(STAGE_DECODE_END)) {
        decodeTime = timestamps[STAGE_DECODE_END] - 
                    timestamps[STAGE_DECODE];
    }
    
    // 7. 使用我们自己记录的时间戳计算pacer时间 (微秒)
    if (timestamps.contains(STAGE_PACER_START) && 
        timestamps.contains(STAGE_PACER_END)) {
        pacerTimeUs = timestamps[STAGE_PACER_END] - 
                     timestamps[STAGE_PACER_START];
    }
    
    // 8. 使用我们自己记录的时间戳计算渲染时间 (微秒)
    if (timestamps.contains(STAGE_RENDER) && 
        timestamps.contains(STAGE_RENDER_END)) {
        renderTimeUs = timestamps[STAGE_RENDER_END] - 
                      timestamps[STAGE_RENDER];
    }
    
    // 9. 端侧发送输入到端侧收到码流之间 (微秒)
    if (timestamps.contains(STAGE_SEND) && 
        timestamps.contains(STAGE_FRAME_RECEIVE)) {
        sendAndReceiveTime = sendToReceiveTime - sunshineInputToEncodeTime - encodeTime;
    }
    
    // 获取事件类型
    EventType eventType = m_entries[id].eventType;
    QString eventTypeName = getEventTypeName(eventType);
    
    // 计算总延迟 (微秒)
    qint64 totalLatency = (timestamps.contains(STAGE_INPUT) && timestamps.contains(STAGE_RENDER_END)) ?
                         timestamps[STAGE_RENDER_END] - timestamps[STAGE_INPUT] : 0;
    
    // 打印所有时间间隔，单位为微秒
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
              "完成帧的渲染，traceId: %d，事件类型: %s (使用微秒级精度时间戳)\n"
              "1. 端侧输入到发送: %lld 微秒 (%.2f 毫秒)\n"
              "2. Sunshine收到输入到编码: %lld 微秒 (%.2f 毫秒)\n"
              "3. 编码时间: %lld 微秒 (%.2f 毫秒)\n"
              "4. 端侧发送到解码: %lld 微秒 (%.2f 毫秒)\n"
              "5. 接收到解码: %lld 微秒 (%.2f 毫秒)\n"
              "6. 解码时间: %lld 微秒 (%.2f 毫秒)\n"
              "7. Pacer时间: %lld 微秒 (%.2f 毫秒)\n"
              "8. 渲染时间: %lld 微秒 (%.2f 毫秒)\n"
              "总延迟: %lld 微秒 (%.2f 毫秒)",
              id, eventTypeName.toUtf8().constData(),
              inputToSendTime, inputToSendTime / 1000.0,
              sunshineInputToEncodeTime, sunshineInputToEncodeTime / 1000.0,
              encodeTime, encodeTime / 1000.0,
              sendToReceiveTime, sendToReceiveTime / 1000.0,
              receiveToDecodeTime, receiveToDecodeTime / 1000.0,
              decodeTime, decodeTime / 1000.0,
              pacerTimeUs, pacerTimeUs / 1000.0,
              renderTimeUs, renderTimeUs / 1000.0,
              totalLatency, totalLatency / 1000.0);
              
    // 写死InfluxDB配置，直接发送数据
    // 是否启用InfluxDB (可以根据需要修改为true)
    bool enableInfluxDB = true;
    
    if (enableInfluxDB) {
        try {
            qDebug() << "LatencyTracker: Preparing data for InfluxDB";
            // 获取当前时间的纳秒时间戳
            qint64 timestampNs = QDateTime::currentMSecsSinceEpoch() * 1000000;
            
            // 构建行协议数据
            // 使用有意义的测量名称和标签
            QString lineProtocol = QString("latency_metrics,event_type=%1").arg(eventTypeName);
            
            // 添加会话相关信息作为标签
            Session* session = Session::get();
            if (session) {
                if (session->m_Computer && !session->m_Computer->name.isEmpty()) {
                    // 确保标签值不包含特殊字符
                    QString computerName = session->m_Computer->name;
                    computerName.replace(" ", "\\ "); // 空格需要转义
                    computerName.replace(",", "\\,"); // 逗号需要转义
                    computerName.replace("=", "\\="); // 等号需要转义
                    lineProtocol += QString(",computer_name=%1").arg(computerName);
                }
                lineProtocol += QString(",app_id=%1").arg(session->m_App.id);
            }
            
            // 添加字段值 - 所有延迟指标（微秒）
            lineProtocol += QString(" input_to_send=%1i,sunshine_input_to_encode=%2i,encode_time=%3i,")
                            .arg(inputToSendTime)
                            .arg(sunshineInputToEncodeTime)
                            .arg(encodeTime);
                            
            lineProtocol += QString("send_and_receive=%1i,receive_to_decode=%2i,decode_time=%3i,")
                            .arg(sendAndReceiveTime)
                            .arg(receiveToDecodeTime)
                            .arg(decodeTime);
                            
            lineProtocol += QString("pacer_time=%1i,render_time=%2i,total_latency=%3i")
                            .arg(pacerTimeUs)
                            .arg(renderTimeUs)
                            .arg(totalLatency);
                            
            // 添加时间戳和换行符
            lineProtocol += QString(" %1\n").arg(timestampNs);
            
            qDebug() << "LatencyTracker: Built InfluxDB line protocol data:" << lineProtocol;
            
            // 使用专门的线程发送数据到InfluxDB
            sendToInfluxDBAsync(lineProtocol);
        }
        catch (const std::exception& e) {
            qWarning() << "LatencyTracker: Exception while preparing data for InfluxDB:" << e.what();
        }
        catch (...) {
            qWarning() << "LatencyTracker: Unknown exception while preparing data for InfluxDB";
        }
    }
} 

