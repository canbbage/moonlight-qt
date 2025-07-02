#pragma once

#include <QObject>
#include <QMap>
#include <QMutex>
#include <QDateTime>
#include <QString>

// 延迟跟踪器类，用于测量输入事件从产生到渲染的各个阶段的延迟
class LatencyTracker : public QObject
{
    Q_OBJECT

public:
    // 定义跟踪阶段的枚举类型
    enum TrackingStage {
        STAGE_INPUT,     // 输入事件产生
        STAGE_SEND,      // 输入事件发送
        STAGE_DECODE,    // 视频帧解码
        STAGE_RENDER,    // 视频帧渲染
        STAGE_COUNT      // 阶段计数，用于数组大小
    };
    
    // 定义事件类型的枚举类型
    enum EventType {
        EVENT_MOUSE_CLICK,   // 鼠标点击
        EVENT_KEY_PRESS,     // 键盘按下
        EVENT_GAMEPAD_BUTTON, // 游戏手柄按钮
        EVENT_COUNT          // 事件类型计数
    };
    
    // 获取单例实例
    static LatencyTracker* instance();
    
    // 销毁单例实例
    static void destroy();
    
    // 生成唯一ID并开始跟踪
    int startTracking(EventType eventType);
    
    // 记录某个阶段的时间戳
    void recordTimestamp(int id, TrackingStage stage);
    
    // 计算并获取各阶段之间的延迟
    QMap<QString, qint64> getLatencies(int id);
    
    // 将完整的延迟数据发送到InfluxDB
    bool sendToInfluxDB(int id);
    
    // 初始化InfluxDB连接
    bool initializeInfluxDB(const QString& influxUrl, const QString& dbName, 
                           const QString& username, const QString& password);
    
    // 清理过期的数据
    void cleanup(int maxAgeMs = 30000);
    
    // 获取所有跟踪的ID列表
    QList<int> getAllTrackingIds() const;
    
    // 获取特定ID的所有时间戳
    QMap<TrackingStage, qint64> getAllTimestamps(int id) const;
    
    // 设置是否启用InfluxDB
    void setInfluxDBEnabled(bool enabled);
    
    // 检查是否存在指定ID
    bool hasTrackingId(int id) const;
    
    // 获取阶段名称
    static QString getStageName(TrackingStage stage);
    
    // 获取事件类型名称
    static QString getEventTypeName(EventType eventType);

private:
    // 单例模式
    static LatencyTracker* s_instance;
    
    // 互斥锁，用于线程安全
    // 声明为mutable，使其可以在const方法中被修改
    mutable QMutex m_mutex;
    
    // 存储事件ID和对应的时间戳
    struct TimestampEntry {
        EventType eventType;                   // 事件类型
        QMap<TrackingStage, qint64> timestamps; // 阶段 -> 时间戳
    };
    
    QMap<int, TimestampEntry> m_entries; // ID -> 时间戳条目
    
    // 当前ID计数器
    int m_currentId;
    
    // 最大ID值，超过此值将重置为1
    static const int MAX_ID = 200;
    
    // InfluxDB相关配置
    QString m_influxUrl;
    QString m_dbName;
    QString m_username;
    QString m_password;
    bool m_influxEnabled;
    
    // 私有构造函数
    LatencyTracker();
    ~LatencyTracker();
    
    // 禁止拷贝
    LatencyTracker(const LatencyTracker&) = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;
}; 