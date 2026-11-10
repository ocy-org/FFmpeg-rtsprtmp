// 本类是“后台解码线程”：在独立 QThread 里用 ffmpeg 拉 RTSP、解码成 YUV，
// 再把帧 publish 进 FrameHub；显示端(YuvGLWidget)从 hub 取帧绘制。
// GUI 线程与解码线程之间“开/停”的控制靠原子标志 running_ 跨线程通信。
#pragma once
#include <QThread>
#include <QAtomicInt>
#include <memory>           // std::shared_ptr（间接被 VideoFrame/FrameHub 使用）
#include <QByteArray>
#include "VideoFrame.h"     // 一帧 YUV 的数据结构（y/u/v、w/h、各 stride）
#include "FrameHub.h"       // 帧中转枢纽：本线程 publish，显示线程 grab

// VideoWorker：拉流+解码线程。对外只暴露“开流/停流/查询是否在跑/取枢纽”四件事。
class VideoWorker : public QThread {
    Q_OBJECT                // 启用元对象：本类要 emit stats / errorOccurred 信号
public:
    explicit VideoWorker(QObject* parent = nullptr) : QThread(parent) {}

    // 开流：记录 URL 并拉起线程,已在跑就直接返回，避免起两个线程。
    void startStream(const QString& url) {
        if (running_.loadAcquire()) return;   // 已运行：忽略本次（换流需先 stopStream）
        url_ = url;                           // 保存目标 RTSP 地址，供 run() 使用
        running_.storeRelease(1);             // 置运行标志=1（release 语义，解码线程可见）
        start();                              // QThread::start —— 真正创建线程并进入 run()
    }

    // 停流：置标志=0 通知解码循环退出，再 join 等它真正结束（最多 3 秒）。
    // 注意 wait(3000) 会阻塞“调用它的线程”（通常是 GUI），故上层用 isRunning() 预判。
    void stopStream() { running_.storeRelease(0); wait(3000); }

    bool isRunning() const { return running_.loadAcquire(); }   // ★新增：避免GUI线程白等3秒
    // 廉价的原子读：用来决定“要不要付 stopStream 里那个 wait 的阻塞成本”。

    FrameHub* hub() { return &hub_; }         // 交帧枢纽指针，MainWindow 用它 attach 显示控件

signals:
    void stats(double fps, double kbps);        // 每秒一次：帧率 + 码率(kbps)，供界面显示
    void errorOccurred(const QString& msg);     // 出错/重连提示，转发到日志区

protected:
    void run() override;     // 外层：自愈重连循环（反复调 sessionOnce，断流自动重开）
private:
    void sessionOnce();         // 内层：一次完整RTSP会话（原run的内容：打开流→解码→发布）
    QString url_;               // 当前拉流的 RTSP 地址
    QAtomicInt running_{0};     // 运行标志：0=停/1=跑，跨线程的唯一通信变量
    FrameHub hub_;              // 帧枢纽（本对象拥有）：解码线程 publish，显示线程 grab
};
