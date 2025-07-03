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
    entry.timestamps[STAGE_INPUT] = QDateTime::currentMSecsSinceEpoch();
    
    m_entries[id] = entry;
    
    qDebug() << "LatencyTracker: Started tracking ID:" << id << "EventType:" << getEventTypeName(eventType) << "at stage:" << getStageName(STAGE_INPUT);
    
    return id;
}

void LatencyTracker::recordTimestamp(int id, TrackingStage stage)
{
    QMutexLocker locker(&m_mutex);
    
    if (m_entries.contains(id)) {
        qint64 timestamp = QDateTime::currentMSecsSinceEpoch();
        m_entries[id].timestamps[stage] = timestamp;
        qDebug() << "LatencyTracker: Recorded ID:" << id << "at stage:" << getStageName(stage) << "timestamp:" << timestamp;
    } else {
        qWarning() << "LatencyTracker: Attempted to record timestamp for unknown ID:" << id;
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

QString LatencyTracker::getStageName(TrackingStage stage)
{
    switch (stage) {
        case STAGE_INPUT:
            return "input";
        case STAGE_SEND:
            return "send";
        case STAGE_DECODE:
            return "decode";
        case STAGE_RENDER:
            return "render";
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
        default:
            return "unknown";
    }
} 