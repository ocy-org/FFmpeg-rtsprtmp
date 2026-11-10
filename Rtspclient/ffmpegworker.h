#ifndef FFMPEGWORKER_H
#define FFMPEGWORKER_H

#include <QThread>
#include <atomic>

extern "C" {
#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libavutil/time.h>          // av_gettime()
#include <libavdevice/avdevice.h>
#include <libswscale/swscale.h>
}

class FFmpegWorker : public QThread
{
    Q_OBJECT
public:
    explicit FFmpegWorker(const QString &cameraName,
                          const QString &rtspUrl,
                          const QString &resolution,
                          const QString &pixelFmt,
                          QObject *parent = nullptr);
    ~FFmpegWorker() override;

    void stop();

signals:
    void stateChanged(const QString &msg);
    void errorOccurred(const QString &err);

protected:
    void run() override;

private:
    static int interruptCallback(void *opaque);

    bool initInput();
    bool initEncoder();
    bool initOutput();
    void cleanup();

    QString m_cameraName;
    QString m_rtspUrl;
    QString m_resolution;
    QString m_pixelFmt;
    std::atomic<bool> m_quit{false};

    AVFormatContext *m_inFmt     = nullptr;
    AVFormatContext *m_outFmt    = nullptr;
    AVCodecContext  *m_decCtx    = nullptr;
    AVCodecContext  *m_encCtx    = nullptr;
    AVStream        *m_outStream = nullptr;
    AVPacket        *m_pkt       = nullptr;
    SwsContext      *m_swsCtx    = nullptr;
    int              m_inVideoIdx = -1;
    int64_t          m_startTime  = 0;   // ★ 起始时刻，用于真实时钟 pts（微秒）
    int64_t          m_lastPts    = -1;  // ★ 上一帧 pts，保证严格单调递增
};

#endif // FFMPEGWORKER_H
