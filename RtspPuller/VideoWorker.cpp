// 本文件：在解码线程里用 ffmpeg (libav*) 拉 RTSP、解码 H.264 成 YUV 并送入 FrameHub。
#include "VideoWorker.h"
extern "C" {
#include <libavformat/avformat.h>    // 解封装：打开流、读包(网络层)
#include <libavcodec/avcodec.h>      // 解码：H.264 比特流 -> 原始帧
#include <libavutil/imgutils.h>      // 图像/平面尺寸工具（此处间接）
#include <libavutil/time.h>          // av_gettime / av_usleep：高精度时间与睡眠
}

// run()：线程主体——“自愈重连”外层循环。
// 一次 RTSP 会话失败或长时间无帧，sessionOnce 返回后自动重开，直到 running_ 被置 0。
void VideoWorker::run() {
    avformat_network_init();   // 网络模块初始化（推荐显式调用）
    while (running_.loadAcquire()) {         // 只要没被停止，就一直保持有会话在跑
        sessionOnce();                       // 跑一次完整会话（内部含解码主循环）
        if (!running_.loadAcquire()) break;        // 用户已停止→正常退出
        emit errorOccurred("5秒未解出画面 → 重连RTSP（对齐SPS/PPS+IDR）…"); // 告知上层
        av_usleep(800000);                          // 0.8s退避再重连（避免疯狂重连占满CPU）
    }
}

// sessionOnce()：一次完整 RTSP 会话。打开流→找视频轨→建解码器→循环读包解码发布。
// 遇到“长期 0 帧”就 return，交给外层 run() 重开，从而触发一次全新的 SPS/PPS+IDR 对齐。
void VideoWorker::sessionOnce() {
    // ---- 打开流（低延迟参数与上版一致）----
    AVDictionary* opt = nullptr;             // 键值对选项表，传给 avformat_open_input
    av_dict_set(&opt, "rtsp_transport", "tcp", 0);        // 强制走 TCP（防丢包花屏）
    av_dict_set(&opt, "timeout", "3000000", 0);           // I/O 超时 3 秒(微秒)，防永久阻塞
    av_dict_set(&opt, "reordering_queue_size", "0", 0);   // 关乱序重排队列，降延迟
    av_dict_set(&opt, "max_delay", "0", 0);               // 缓冲延迟下限置 0，尽快出帧

    AVFormatContext* fmt = avformat_alloc_context();      // 分配解封装上下文
    fmt->probesize = 512 * 1024;                          // 探测码流用的最大字节数（限流降低时延）
    fmt->max_analyze_duration = 2 * AV_TIME_BASE;         // 探测时长上限 2 秒
    int ret = avformat_open_input(&fmt, url_.toUtf8().constData(), nullptr, &opt); // 打开 RTSP
    av_dict_free(&opt);                        // 选项表用完即释放
    if (ret < 0) {                             // 打开失败（连不上/URL 非法等）
        char e[AV_ERROR_MAX_STRING_SIZE] = {0};
        av_strerror(ret, e, sizeof(e));        // 错误码转可读文字
        emit errorOccurred(QString("打开流失败: %1").arg(e));
        return;                                // 交外层重连
    }
    if (avformat_find_stream_info(fmt, nullptr) < 0) {   // 分析各流参数（编码/分辨率等）
        emit errorOccurred("find_stream_info失败");
        avformat_close_input(&fmt);            // 失败要先关闭已打开的输入，避免泄漏
        return;
    }
    // 在多个流里挑出最优视频轨索引（-1=自动选最佳，-1=不指定相关流）
    const int vidx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (vidx < 0) {                            // 根本没视频轨
        emit errorOccurred("流中无视频轨");
        avformat_close_input(&fmt);
        return;
    }
    // SDP 里没带出宽度（=编码参数缺失）：设备端常见问题，需等码流内嵌 SPS/PPS
    if (fmt->streams[vidx]->codecpar->width == 0)
        emit errorOccurred("SDP无编码参数（设备端已知项）→ 等带内SPS/PPS+IDR，5秒未出自动重连");

    // 依据编码 ID 找解码器并创建解码上下文
    const AVCodec* codec = avcodec_find_decoder(fmt->streams[vidx]->codecpar->codec_id);
    AVCodecContext* ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(ctx, fmt->streams[vidx]->codecpar); // 把流参数灌进解码器
    ctx->flags  |= AV_CODEC_FLAG_LOW_DELAY;     // 低延迟模式：不缓冲等待，尽快出帧
    ctx->thread_count = 1;                      // 单线程解码：避免帧序错乱、降延迟
    ctx->thread_type = FF_THREAD_SLICE;         // （单线程下基本无效，仅显式声明）
    avcodec_open2(ctx, codec, nullptr);         // 打开解码器，准备收包

    AVPacket* pkt = av_packet_alloc();          // 压缩数据包（H.264 比特流片段）
    AVFrame*  frm = av_frame_alloc();           // 解码后的原始帧（YUV）
    qint64 lastStats = av_gettime();            // 上次统计 fps/kbps 的时刻
    qint64 lastFrame = av_gettime();   // ★看门狗基准：最近一次解出帧的时刻
    int nFrames = 0;                            // 本统计周期内解出的帧数
    qint64 nBytes = 0;                          // 本统计周期内收到的视频字节数
    bool decodedAny = false;                    // 本会话是否至少解出过 1 帧（区分“整段 0 帧”）

    while (running_.loadAcquire()) {            // 解码主循环，running_=0 即退出
        ret = av_read_frame(fmt, pkt);          // 从网络/缓冲读一个包
        if (ret < 0) {                          // 读包失败（网络抖动/暂时没数据）
            if (av_gettime() - lastFrame > 5000000) break;   // 长期无帧→重开
            av_usleep(100000);                                // 网络抖动稍候重试
            continue;
        }
        if (pkt->stream_index != vidx) { av_packet_unref(pkt); continue; } // 非视频轨：丢弃
        nBytes += pkt->size;                    // 累计码率统计字节（含网络收到的视频负载）
        avcodec_send_packet(ctx, pkt);          // 把压缩包喂给解码器
        // 一次喂包可能吐出多帧（解码器有延迟），循环取空
        while (avcodec_receive_frame(ctx, frm) == 0) {
            lastFrame = av_gettime();                        // ★解出帧→喂狗（刷新看门狗基准）
            decodedAny = true;                               // 标记本会话出过帧
            ++nFrames;                                       // 帧计数 +1
            // 把解码帧的 YUV 三平面拷贝进 VideoFrame（拷贝是必要的：frm 会被复用）
            auto f = std::make_shared<VideoFrame>();
            f->w = frm->width;  f->h = frm->height;          // 帧宽高
            f->yStride = frm->linesize[0];                   // Y 行距（含可能的对齐填充）
            f->uStride = frm->linesize[1];                   // U 行距
            f->vStride = frm->linesize[2];                   // V 行距
            // 按 linesize*行数 精确拷贝每个平面（Y 全尺寸，U/V 各 1/4：420）
            f->y = QByteArray((const char*)frm->data[0], frm->linesize[0] * frm->height);
            f->u = QByteArray((const char*)frm->data[1], frm->linesize[1] * frm->height / 2);
            f->v = QByteArray((const char*)frm->data[2], frm->linesize[2] * frm->height / 2);
            hub_.publish(f);                     // 发布到枢纽：显示线程下一次 grab 就能拿到
        }
        av_packet_unref(pkt);                    // 释放本包引用（循环复用 pkt）
        if (av_gettime() - lastFrame > 5000000) break;       // ★5秒0帧→重开会话
        if (av_gettime() - lastStats >= 1000000) {           // 每满 1 秒上报一次统计
            emit stats(nFrames, nBytes * 8.0 / 1000.0);      // fps + 码率(kbps)（字节*8/1000）
            nFrames = 0; nBytes = 0; lastStats += 1000000;   // 清零并推进统计窗口基准
        }
    }
    // 会话结束：若整段都没解出过帧，多半是设备没下发 SPS/PPS，给出定位提示
    if (!decodedAny)
        emit errorOccurred("本会话始终0帧（设备未下发SPS/PPS）→ 已自动重连；若反复出现需查设备端SDP/IDR设置");
    // 资源逆序释放：帧 -> 包 -> 解码上下文 -> 解封装上下文，避免泄漏
    av_frame_free(&frm);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    avformat_close_input(&fmt);
}
