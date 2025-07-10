#pragma once

#include <QObject>
#include <QMap>
#include <QMutex>
#include <QDateTime>
#include <QString>
#include <QThread>
#include <QQueue>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QWaitCondition>

// 发送线程类，专门用于发送数据到InfluxDB
class InfluxSenderThread : public QThread
{
    Q_OBJECT
public:
    InfluxSenderThread();
    ~InfluxSenderThread();
    
    // 添加数据到队列
    void addToQueue(const QString& lineProtocol);
    
    // 停止线程
    void stopThread();
    
protected:
    // 线程执行函数
    void run() override;
    
private:
    // 数据队列
    QQueue<QString> m_dataQueue;
    
    // 互斥锁，用于保护队列
    QMutex m_queueMutex;
    
    // 条件变量，用于通知线程有新数据
    QWaitCondition m_queueNotEmpty;
    
    // 线程是否应该继续运行
    bool m_running;
};

// 延迟跟踪器类，用于测量输入事件从产生到渲染的各个阶段的延迟
class LatencyTracker : public QObject
{
    Q_OBJECT

public:
    // 定义跟踪阶段的枚举类型
    enum TrackingStage {
        STAGE_INPUT,     // 输入事件产生
        STAGE_SEND,      // 输入事件发送
        STAGE_DECODE,    // 视频帧解码开始
        STAGE_DECODE_END, // 视频帧解码结束
        STAGE_PACER_START, // 视频帧进入pacer
        STAGE_PACER_END,  // 视频帧离开pacer，准备渲染
        STAGE_RENDER,    // 视频帧渲染开始
        STAGE_RENDER_END, // 视频帧渲染结束
        STAGE_SUNSHINE_INPUT_ARRIVAL, // Sunshine接收到输入的时间
        STAGE_SUNSHINE_ENCODE_START,  // Sunshine开始编码的时间
        STAGE_SUNSHINE_ENCODE_END,    // Sunshine结束编码的时间
        STAGE_FRAME_RECEIVE, // 视频帧接收时间
        STAGE_FRAME_ENQUEUE, // 视频帧入队时间
        STAGE_COUNT      // 阶段计数，用于数组大小
    };
    
    // 定义事件类型的枚举类型
    enum EventType {
        EVENT_MOUSE_CLICK,   // 鼠标点击
        EVENT_KEY_PRESS,     // 键盘按下
        EVENT_GAMEPAD_BUTTON, // 游戏手柄按钮
        EVENT_UNKNOWN,       // 未知事件类型
        EVENT_COUNT          // 事件类型计数
    };
    
    // 获取单例实例
    static LatencyTracker* instance();
    
    // 销毁单例实例
    static void destroy();
    
    // 生成唯一ID并开始跟踪
    int startTracking(EventType eventType);
    
    // 使用指定ID开始跟踪
    void startTrackingWithId(int id, EventType eventType);
    
    // 记录某个阶段的时间戳
    void recordTimestamp(int id, TrackingStage stage);
    
    // 记录某个阶段的指定时间戳
    void recordTimestamp(int id, TrackingStage stage, qint64 timestamp);
    
    // 记录Sunshine的时间戳（纳秒级别）
    void recordSunshineTimestamp(int id, TrackingStage stage, int64_t timestampNs);
    
    // 计算并获取各阶段之间的延迟
    QMap<QString, qint64> getLatencies(int id);
    
    
    // 清理过期的数据
    void cleanup(int maxAgeMs = 30000);
    
    // 获取所有跟踪的ID列表
    QList<int> getAllTrackingIds() const;
    
    // 获取特定ID的所有时间戳
    QMap<TrackingStage, qint64> getAllTimestamps(int id) const;
    

    // 检查是否存在指定ID
    bool hasTrackingId(int id) const;
    
    // 获取事件类型
    EventType getEventType(int id) const;
    
    // 计算并打印各个阶段的延迟
    void calculateAndLogLatencies(int id, qint64 pacerTime, qint64 renderTime);
    
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
    
    
    // 私有构造函数
    LatencyTracker();
    ~LatencyTracker();
    
    // 禁止拷贝
    LatencyTracker(const LatencyTracker&) = delete;
    LatencyTracker& operator=(const LatencyTracker&) = delete;
    
    // 异步发送InfluxDB数据
    void sendToInfluxDBAsync(const QString& lineProtocol);
    
    // 发送线程
    InfluxSenderThread* m_senderThread;
}; 