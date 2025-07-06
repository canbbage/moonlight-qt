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
#include <QDebug>
#include "session.h"

// 初始化静态成员
LatencyTracker* LatencyTracker::s_instance = nullptr;

LatencyTracker* LatencyTracker::instance()
{
    if (!s_instance) {
        s_instance = new LatencyTracker();
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

LatencyTracker::LatencyTracker() : QObject(nullptr), m_influxEnabled(false), m_currentId(1)
{
    // 默认构造函数
}

LatencyTracker::~LatencyTracker()
{
    // 清理资源
    m_entries.clear();
}

int LatencyTracker::startTracking(EventType eventType)
{
    QMutexLocker locker(&m_mutex);
    
    // 生成唯一ID (顺序ID)
    int id = m_currentId;
    
    // 更新ID计数器，超过最大值则重置为1
    m_currentId = (m_currentId % MAX_ID) + 1;
    
    // 创建新条目并记录起始时间戳
    TimestampEntry entry;
    entry.eventType = eventType;
    entry.timestamps[STAGE_INPUT] = LiGetMillis(); // 使用LiGetMillis()替代SDL_GetTicks()
    
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
        // 使用PltGetMillis()获取时间戳，与Moonlight-common-c使用相同的时间基准
        qint64 timestamp = LiGetMillis();  // LiGetMillis内部调用PltGetMillis
        m_entries[id].timestamps[stage] = timestamp;
        qDebug() << "LatencyTracker: Recorded ID:" << id << "at stage:" << getStageName(stage) << "timestamp:" << timestamp;
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

bool LatencyTracker::sendToInfluxDB(int id)
{
    QMutexLocker locker(&m_mutex);
    
    if (!m_influxEnabled) {
        qDebug() << "LatencyTracker: InfluxDB is not enabled, skipping send for ID:" << id;
        return false;
    }
    
    if (!m_entries.contains(id)) {
        qWarning() << "LatencyTracker: Attempted to send data for unknown ID:" << id;
        return false;
    }
    
    QMap<QString, qint64> latencies = getLatencies(id);
    if (latencies.isEmpty()) {
        qWarning() << "LatencyTracker: No latency data available for ID:" << id;
        return false;
    }
    
    // 提取事件类型
    EventType eventType = m_entries[id].eventType;
    QString eventTypeName = getEventTypeName(eventType);
    
    // 构建InfluxDB的行协议数据
    // 格式: measurement,tag1=value1,tag2=value2 field1=value1,field2=value2 timestamp
    QString lineProtocol = QString("input_latency,event_type=%1").arg(eventTypeName);
    
    // 添加会话相关信息作为标签
    Session* session = Session::get();
    if (session) {
        // 使用计算机名称和应用ID作为标识
        if (session->m_Computer && !session->m_Computer->name.isEmpty()) {
            lineProtocol += QString(",computer_name=%1").arg(session->m_Computer->name);
        }
        lineProtocol += QString(",app_id=%1").arg(session->m_App.id);
    }
    
    // 添加字段
    lineProtocol += " ";
    QMapIterator<QString, qint64> i(latencies);
    bool firstField = true;
    while (i.hasNext()) {
        i.next();
        if (!firstField) {
            lineProtocol += ",";
        }
        lineProtocol += QString("%1=%2i").arg(i.key()).arg(i.value());
        firstField = false;
    }
    
    // 添加时间戳（纳秒级）
    qint64 timestamp = QDateTime::currentMSecsSinceEpoch() * 1000000;
    lineProtocol += QString(" %1").arg(timestamp);
    
    // 创建网络请求
    QNetworkAccessManager manager;
    QUrl url(m_influxUrl);
    url.setPath("/write");
    
    QUrlQuery query;
    query.addQueryItem("db", m_dbName);
    query.addQueryItem("precision", "ns");
    url.setQuery(query);
    
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    
    // 如果有用户名和密码，添加基本认证
    if (!m_username.isEmpty()) {
        QString auth = QString("%1:%2").arg(m_username).arg(m_password);
        QByteArray authBytes = auth.toUtf8().toBase64();
        request.setRawHeader("Authorization", "Basic " + authBytes);
    }
    
    // 发送POST请求
    QNetworkReply* reply = manager.post(request, lineProtocol.toUtf8());
    
    // 等待请求完成
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    bool success = (reply->error() == QNetworkReply::NoError);
    
    if (!success) {
        qWarning() << "LatencyTracker: Failed to send data to InfluxDB:" << reply->errorString();
    } else {
        qDebug() << "LatencyTracker: Successfully sent data for ID:" << id;
    }
    
    reply->deleteLater();
    
    return success;
}

bool LatencyTracker::initializeInfluxDB(const QString& influxUrl, const QString& dbName, 
                                      const QString& username, const QString& password)
{
    QMutexLocker locker(&m_mutex);
    
    m_influxUrl = influxUrl;
    m_dbName = dbName;
    m_username = username;
    m_password = password;
    
    // 测试连接
    QNetworkAccessManager manager;
    QUrl url(m_influxUrl);
    url.setPath("/ping");
    
    QNetworkRequest request(url);
    
    // 如果有用户名和密码，添加基本认证
    if (!m_username.isEmpty()) {
        QString auth = QString("%1:%2").arg(m_username).arg(m_password);
        QByteArray authBytes = auth.toUtf8().toBase64();
        request.setRawHeader("Authorization", "Basic " + authBytes);
    }
    
    // 发送GET请求
    QNetworkReply* reply = manager.get(request);
    
    // 等待请求完成
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    
    bool success = (reply->error() == QNetworkReply::NoError);
    
    if (success) {
        m_influxEnabled = true;
        qDebug() << "LatencyTracker: Successfully connected to InfluxDB at" << influxUrl;
    } else {
        qWarning() << "LatencyTracker: Failed to connect to InfluxDB:" << reply->errorString();
    }
    
    reply->deleteLater();
    
    return success;
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

void LatencyTracker::setInfluxDBEnabled(bool enabled)
{
    QMutexLocker locker(&m_mutex);
    m_influxEnabled = enabled;
}

bool LatencyTracker::hasTrackingId(int id) const
{
    QMutexLocker locker(&m_mutex);
    return m_entries.constFind(id) != m_entries.constEnd();
}

// 这里曾经有一个getEventType的定义，已移至下方

// 记录Sunshine的时间戳（纳秒级别）
void LatencyTracker::recordSunshineTimestamp(int id, TrackingStage stage, int64_t timestampNs)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_entries.contains(id)) {
        // 直接存储纳秒值，不转换为毫秒
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
    
    // 计算各个阶段的时间间隔
    qint64 inputToSendTime = 0;
    qint64 sunshineInputToEncodeTime = 0;
    qint64 encodeTime = 0;
    qint64 sendToReceiveTime = 0;
    qint64 receiveToDecodeTime = 0;
    qint64 decodeTime = 0;
    
    // 1. 端侧输入到发送之间 (毫秒)
    if (timestamps.contains(STAGE_INPUT) && timestamps.contains(STAGE_SEND)) {
        inputToSendTime = timestamps[STAGE_SEND] - timestamps[STAGE_INPUT];
    }
    
    // 2. sunshine收到输入到编码之间 (纳秒转毫秒)
    if (timestamps.contains(STAGE_SUNSHINE_INPUT_ARRIVAL) && 
        timestamps.contains(STAGE_SUNSHINE_ENCODE_START)) {
        // 将纳秒转换为毫秒
        qint64 inputArrivalMs = timestamps[STAGE_SUNSHINE_INPUT_ARRIVAL] / 1000000;
        qint64 encodeStartMs = timestamps[STAGE_SUNSHINE_ENCODE_START] / 1000000;
        sunshineInputToEncodeTime = encodeStartMs - inputArrivalMs;
    }
    
    // 3. 编码开始到编码结束之间 (纳秒转毫秒)
    if (timestamps.contains(STAGE_SUNSHINE_ENCODE_START) && 
        timestamps.contains(STAGE_SUNSHINE_ENCODE_END)) {
        // 将纳秒转换为毫秒
        qint64 encodeStartMs = timestamps[STAGE_SUNSHINE_ENCODE_START] / 1000000;
        qint64 encodeEndMs = timestamps[STAGE_SUNSHINE_ENCODE_END] / 1000000;
        encodeTime = encodeEndMs - encodeStartMs;
    }
    
    // 4. 端侧发送输入到端侧收到码流之间 (毫秒)
    if (timestamps.contains(STAGE_SEND) && 
        timestamps.contains(STAGE_FRAME_RECEIVE)) {
        sendToReceiveTime = timestamps[STAGE_FRAME_RECEIVE] - 
                           timestamps[STAGE_SEND];
    }
    
    // 5. 端侧收到码流到端侧解码之前 (毫秒)
    if (timestamps.contains(STAGE_FRAME_RECEIVE) && 
        timestamps.contains(STAGE_DECODE)) {
        receiveToDecodeTime = timestamps[STAGE_DECODE] - 
                             timestamps[STAGE_FRAME_RECEIVE];
    }
    
    // 6. 解码前到解码后 (毫秒)
    if (timestamps.contains(STAGE_DECODE) && 
        timestamps.contains(STAGE_DECODE_END)) {
        decodeTime = timestamps[STAGE_DECODE_END] - 
                    timestamps[STAGE_DECODE];
    }
    
    // 获取事件类型
    EventType eventType = m_entries[id].eventType;
    QString eventTypeName = getEventTypeName(eventType);
    
    // 打印所有时间间隔，单位为毫秒
    qint64 totalLatency = (timestamps.contains(STAGE_INPUT) && timestamps.contains(STAGE_RENDER_END)) ?
                         timestamps[STAGE_RENDER_END] - timestamps[STAGE_INPUT] : 0;
    
    SDL_LogInfo(SDL_LOG_CATEGORY_APPLICATION,
              "完成帧的渲染，traceId: %d，事件类型: %s\n"
              "1. 端侧输入到发送: %lld ms\n"
              "2. Sunshine收到输入到编码: %lld ms\n"
              "3. 编码时间: %lld ms\n"
              "4. 端侧发送到接收: %lld ms\n"
              "5. 接收到解码: %lld ms\n"
              "6. 解码时间: %lld ms\n"
              "7. Pacer时间: %lld ms\n"
              "8. 渲染时间: %lld ms\n"
              "总延迟: %lld ms",
              id, eventTypeName.toUtf8().constData(),
              inputToSendTime,
              sunshineInputToEncodeTime,
              encodeTime, 
              sendToReceiveTime, 
              receiveToDecodeTime, 
              decodeTime, 
              pacerTime, 
              renderTime, 
              totalLatency);
} 