#pragma once

#include <QObject>
#include <QTimer>
#include <QMutex>
#include <QMap>
#include <QString>
#include <QThread>
#include <QQueue>
#include <QNetworkAccessManager>
#include <QNetworkRequest>
#include <QNetworkReply>
#include <QUrlQuery>
#include <QWaitCondition>

// FPS数据发送线程类
class FpsSenderThread : public QThread
{
    Q_OBJECT
public:
    FpsSenderThread();
    ~FpsSenderThread();
    
    // 添加FPS数据到队列
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

// 帧率监控器类
class FpsMonitor : public QObject
{
    Q_OBJECT

public:
    // 获取单例实例
    static FpsMonitor* instance();
    
    // 销毁单例实例
    static void destroy();
    
    // 记录帧渲染
    void recordFrameRendered();
    
    // 记录帧接收
    void recordFrameReceived();
    
    // 记录帧解码
    void recordFrameDecoded();
    
    Q_INVOKABLE void startMonitoring();
    Q_INVOKABLE void stopMonitoring();
    
    // 检查是否正在监控
    bool isMonitoring() const;

private:
    // 单例模式
    static FpsMonitor* s_instance;
    
    // 互斥锁，用于线程安全
    mutable QMutex m_mutex;
    
    // 定时器，每秒统计一次
    QTimer* m_timer;
    
    // 计数器
    uint32_t m_receivedFrames;
    uint32_t m_decodedFrames;
    uint32_t m_renderedFrames;
    
    // 统计开始时间戳
    qint64 m_startTimestamp;
    
    // 是否正在监控
    bool m_isMonitoring;
    
    // 发送线程
    FpsSenderThread* m_senderThread;
    
    // 私有构造函数
    FpsMonitor();
    ~FpsMonitor();
    
    // 禁止拷贝
    FpsMonitor(const FpsMonitor&) = delete;
    FpsMonitor& operator=(const FpsMonitor&) = delete;
    
    // 异步发送FPS数据到InfluxDB
    void sendFpsDataToInfluxDBAsync(const QString& lineProtocol);
    
    // 定时器槽函数
    void onTimerTimeout();
    
    // 重置计数器
    void resetCounters();
    
    // 获取当前时间戳（纳秒）
    qint64 getCurrentTimestampNs();
}; 