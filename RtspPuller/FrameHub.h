#ifndef FRAMEHUB_H
#define FRAMEHUB_H

#include <QMutex>
#include <memory>
#include "VideoFrame.h"

// 最新帧信箱：解码线程 publish 覆盖旧帧（天然丢帧，延迟不累积），
// 显示线程 grab 只取比 lastSeq 更新的帧，无新帧返回空
class FrameHub {
public:
    void publish(std::shared_ptr<VideoFrame> f) {
        if (!f) return;
        QMutexLocker lk(&m_);
        latest_ = std::move(f);
        ++seq_;
    }

    std::shared_ptr<VideoFrame> grab(quint64 &lastSeq) {
        QMutexLocker lk(&m_);
        if (seq_ == lastSeq) return nullptr;   // 无新帧
        lastSeq = seq_;
        return latest_;
    }

    quint64 seq() const { QMutexLocker lk(&m_); return seq_; }

private:
    mutable QMutex m_;
    std::shared_ptr<VideoFrame> latest_;
    quint64 seq_ = 0;
};

#endif // FRAMEHUB_H
