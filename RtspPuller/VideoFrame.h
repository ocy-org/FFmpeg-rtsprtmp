#ifndef VIDEOFRAME_H
#define VIDEOFRAME_H

#include <QByteArray>

// 一帧解码后的 YUV420P 数据（三个独立平面，行距可能含对齐填充）
struct VideoFrame {
    int w = 0, h = 0;
    int yStride = 0, uStride = 0, vStride = 0;  // 各平面每行字节数（= linesize）
    QByteArray y, u, v;                         // 平面数据（解码帧会被复用，必须拷贝）
};

#endif // VIDEOFRAME_H
