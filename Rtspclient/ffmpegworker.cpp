#include "ffmpegworker.h"
#include <QDebug>

FFmpegWorker::FFmpegWorker(const QString &cameraName,
                           const QString &rtspUrl,
                           const QString &resolution,
                           const QString &pixelFmt,
                           QObject *parent)
    : QThread(parent)
    , m_cameraName(cameraName)
    , m_rtspUrl(rtspUrl)
    , m_resolution(resolution)
    , m_pixelFmt(pixelFmt)
{
    avdevice_register_all();
}

FFmpegWorker::~FFmpegWorker()
{
    stop();
    wait();
}

void FFmpegWorker::stop()
{
    m_quit = true;
    requestInterruption();
}

int FFmpegWorker::interruptCallback(void *opaque)
{
    auto *w = static_cast<FFmpegWorker*>(opaque);
    return (w->m_quit.load() || w->isInterruptionRequested()) ? 1 : 0;
}

// ============================================================
// 打开摄像头
// ============================================================
bool FFmpegWorker::initInput()
{
    const QString name = m_cameraName.trimmed();
    if (name.isEmpty()) {
        emit errorOccurred("设备名为空");
        return false;
    }

    const AVInputFormat *ifmt = av_find_input_format("dshow");
    if (!ifmt) {
        emit errorOccurred("未找到 dshow 输入格式");
        return false;
    }

    m_inFmt = avformat_alloc_context();
    if (!m_inFmt) return false;
    m_inFmt->interrupt_callback.callback = interruptCallback;
    m_inFmt->interrupt_callback.opaque   = this;

    AVDictionary *opts = nullptr;
    if (!m_resolution.isEmpty())
        av_dict_set(&opts, "video_size", m_resolution.toUtf8().constData(), 0);

    if (m_pixelFmt.compare("mjpeg", Qt::CaseInsensitive) == 0)
        av_dict_set(&opts, "vcodec", "mjpeg", 0);
    else if (m_pixelFmt.compare("yuyv422", Qt::CaseInsensitive) == 0)
        av_dict_set(&opts, "pixel_format", "yuyv422", 0);

    // ★ 优化 2：100M → 10M。dshow 缓冲越小，编码端抖动时积压的帧越少，
    //   延迟上界越低（640x480 MJPEG 每帧约 50~100KB，10M 足够缓冲百余帧）
    av_dict_set(&opts, "rtbufsize", "10M", 0);
    av_dict_set(&opts, "audio_buffer_size", "30", 0);
    av_dict_set(&opts, "crossbar_video_input_pin_number", "0", 0);
    av_dict_set(&opts, "fflags", "nobuffer", 0);
    // 注：flags=low_delay 对 dshow 无效（日志已提示未识别），改在解码器上设置
    av_dict_set(&opts, "analyzeduration", "0", 0);
    av_dict_set(&opts, "probesize", "500000", 0);

    const QByteArray dev = QString("video=%1").arg(name).toUtf8();
    qDebug() << "[Input] 打开设备:" << dev
             << "分辨率:" << (m_resolution.isEmpty() ? "自动" : m_resolution)
             << "格式:" << m_pixelFmt;

    int ret = avformat_open_input(&m_inFmt, dev.constData(), ifmt, &opts);

    AVDictionaryEntry *t = nullptr;
    while ((t = av_dict_get(opts, "", t, AV_DICT_IGNORE_SUFFIX)))
        qDebug() << "[Input] 未识别的选项:" << t->key << "=" << t->value;
    av_dict_free(&opts);

    if (ret < 0) {
        m_inFmt = nullptr;
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        QString hint;
        if (ret == AVERROR(EIO)) {
            hint = QStringLiteral(
                "\n\n排查建议：\n"
                "1) 设备被占用：关闭 Windows相机/浏览器/Zoom，"
                "任务管理器结束旧的 RtspPusher.exe 进程\n"
                "2) 换一种组合重试：分辨率 640x480 + 格式 自动\n"
                "3) 命令行验证：ffplay -f dshow -video_size %1 -i video=\"%2\""
            ).arg(m_resolution.isEmpty() ? "640x480" : m_resolution, name);
        }
        emit errorOccurred(QString("打开摄像头失败 [err=%1] %2%3").arg(ret).arg(err, hint));
        return false;
    }

    if (avformat_find_stream_info(m_inFmt, nullptr) < 0) {
        emit errorOccurred("获取流信息失败");
        return false;
    }

    m_inVideoIdx = av_find_best_stream(m_inFmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_inVideoIdx < 0) {
        emit errorOccurred("未找到视频流");
        return false;
    }

    AVStream *inStream = m_inFmt->streams[m_inVideoIdx];
    const AVCodec *dec = avcodec_find_decoder(inStream->codecpar->codec_id);
    if (!dec) { emit errorOccurred("未找到解码器"); return false; }

    m_decCtx = avcodec_alloc_context3(dec);
    if (!m_decCtx || avcodec_parameters_to_context(m_decCtx, inStream->codecpar) < 0) {
        emit errorOccurred("解码器初始化失败");
        return false;
    }
    // ★ 优化 2 补充：解码器低延迟标志（对应之前未生效的 flags=low_delay）
    m_decCtx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    if (avcodec_open2(m_decCtx, dec, nullptr) < 0) {
        emit errorOccurred("解码器打开失败");
        return false;
    }

    qDebug() << "[Input] 协商完成:" << inStream->codecpar->width << "x"
             << inStream->codecpar->height
             << avcodec_get_name(inStream->codecpar->codec_id);
    return true;
}

bool FFmpegWorker::initEncoder()
{
    const AVCodec *enc = avcodec_find_encoder_by_name("libx264");
    if (!enc) enc = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!enc) { emit errorOccurred("未找到 H.264 编码器（需要 libx264）"); return false; }

    m_encCtx = avcodec_alloc_context3(enc);
    if (!m_encCtx) return false;

    AVStream *inStream = m_inFmt->streams[m_inVideoIdx];
    m_encCtx->width  = inStream->codecpar->width  > 0 ? inStream->codecpar->width  : 640;
    m_encCtx->height = inStream->codecpar->height > 0 ? inStream->codecpar->height : 480;
    m_encCtx->pix_fmt   = AV_PIX_FMT_YUV420P;

    // ★ 优化 1（核心）：time_base 改为毫秒精度，pts 按真实时钟打。
    //   之前固定 25fps 打戳，而摄像头实际按 30fps 出帧，
    //   播放速率 < 生产速率导致帧在链路里无限积压、延迟持续增长
    m_encCtx->time_base = AVRational{1, 1000};
    m_encCtx->framerate = AVRational{30, 1};   // 仅作元信息声明
    m_encCtx->bit_rate  = m_encCtx->height >= 720 ? 2000000 : 800000;
    m_encCtx->gop_size  = 30;
    m_encCtx->max_b_frames = 0;

    av_opt_set(m_encCtx->priv_data, "preset",  "ultrafast",   0);
    av_opt_set(m_encCtx->priv_data, "tune",    "zerolatency", 0);
    av_opt_set(m_encCtx->priv_data, "profile", "baseline",    0);

    if (avcodec_open2(m_encCtx, enc, nullptr) < 0) {
        emit errorOccurred("编码器打开失败");
        return false;
    }
    return true;
}

bool FFmpegWorker::initOutput()
{
    int ret = avformat_alloc_output_context2(&m_outFmt, nullptr, "rtsp",
                                             m_rtspUrl.toUtf8().constData());
    if (ret < 0 || !m_outFmt) {
        emit errorOccurred("创建 RTSP 输出上下文失败");
        return false;
    }

    m_outStream = avformat_new_stream(m_outFmt, nullptr);
    if (!m_outStream) return false;
    avcodec_parameters_from_context(m_outStream->codecpar, m_encCtx);
    m_outStream->time_base = m_encCtx->time_base;

    AVDictionary *opts = nullptr;
    av_dict_set(&opts, "rtsp_transport", "tcp", 0);
    av_dict_set(&opts, "flush_packets", "1", 0);   // 每包即发，不做输出缓冲

    ret = avformat_write_header(m_outFmt, &opts);
    av_dict_free(&opts);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, err, sizeof(err));
        emit errorOccurred(QString("连接 RTSP 服务器失败: %1\n请确认 mediamtx 已启动").arg(err));
        return false;
    }
    return true;
}

void FFmpegWorker::run()
{
    if (!initInput())   { cleanup(); return; }
    if (!initEncoder()) { cleanup(); return; }
    if (!initOutput())  { cleanup(); return; }

    emit stateChanged("推流中…");

    m_pkt = av_packet_alloc();
    m_startTime = av_gettime();    // ★ 优化 1：从首帧前开始计时
    m_lastPts   = -1;

    while (!m_quit.load()) {
        int ret = av_read_frame(m_inFmt, m_pkt);
        if (ret < 0) {
            if (m_quit.load()) break;
            if (ret == AVERROR(EAGAIN)) { av_usleep(2000); continue; }
            char err[AV_ERROR_MAX_STRING_SIZE] = {0};
            av_strerror(ret, err, sizeof(err));
            emit errorOccurred(QString("读取摄像头帧失败: %1").arg(err));
            break;
        }
        if (m_pkt->stream_index != m_inVideoIdx) {
            av_packet_unref(m_pkt);
            continue;
        }

        ret = avcodec_send_packet(m_decCtx, m_pkt);
        av_packet_unref(m_pkt);
        if (ret < 0) continue;

        AVFrame *decFrame = av_frame_alloc();
        while (!m_quit.load() &&
               avcodec_receive_frame(m_decCtx, decFrame) == 0) {

            m_swsCtx = sws_getCachedContext(m_swsCtx,
                            decFrame->width, decFrame->height,
                            (AVPixelFormat)decFrame->format,
                            m_encCtx->width, m_encCtx->height,
                            m_encCtx->pix_fmt,
                            SWS_BICUBIC, nullptr, nullptr, nullptr);
            if (!m_swsCtx) { av_frame_free(&decFrame); goto end; }

            AVFrame *encFrame = av_frame_alloc();
            encFrame->format = m_encCtx->pix_fmt;
            encFrame->width  = m_encCtx->width;
            encFrame->height = m_encCtx->height;
            if (av_frame_get_buffer(encFrame, 0) < 0) {
                av_frame_free(&encFrame);
                continue;
            }

            sws_scale(m_swsCtx,
                      decFrame->data, decFrame->linesize, 0, decFrame->height,
                      encFrame->data, encFrame->linesize);

            // ★ 优化 1：pts = 距推流开始的真实毫秒数。
            //   无论摄像头是 30fps 还是 10fps，时间戳永远与真实时间对齐，
            //   播放端速率 = 生产速率，帧不再积压
            int64_t pts = (av_gettime() - m_startTime) / 1000;
            if (pts <= m_lastPts) pts = m_lastPts + 1;   // 保证严格单调递增
            m_lastPts = pts;
            encFrame->pts = pts;

            avcodec_send_frame(m_encCtx, encFrame);
            av_frame_free(&encFrame);

            while (avcodec_receive_packet(m_encCtx, m_pkt) == 0) {
                av_packet_rescale_ts(m_pkt, m_encCtx->time_base, m_outStream->time_base);
                m_pkt->stream_index = 0;
                if (av_interleaved_write_frame(m_outFmt, m_pkt) < 0) {
                    av_packet_unref(m_pkt);
                    emit errorOccurred("写入 RTSP 失败，请检查 mediamtx 是否在运行");
                    av_frame_free(&decFrame);
                    goto end;
                }
                av_packet_unref(m_pkt);
            }
            av_frame_unref(decFrame);
        }
        av_frame_free(&decFrame);
    }

end:
    av_write_trailer(m_outFmt);
    cleanup();
    emit stateChanged("推流已停止");
}

void FFmpegWorker::cleanup()
{
    if (m_pkt)    { av_packet_free(&m_pkt); m_pkt = nullptr; }
    if (m_swsCtx) { sws_freeContext(m_swsCtx); m_swsCtx = nullptr; }
    if (m_decCtx) { avcodec_free_context(&m_decCtx); }
    if (m_encCtx) { avcodec_free_context(&m_encCtx); }
    if (m_outFmt) {
        if (!(m_outFmt->oformat->flags & AVFMT_NOFILE) && m_outFmt->pb)
            avio_closep(&m_outFmt->pb);
        avformat_free_context(m_outFmt);
        m_outFmt = nullptr;
    }
    if (m_inFmt) {
        avformat_close_input(&m_inFmt);
        m_inFmt = nullptr;
    }
}
