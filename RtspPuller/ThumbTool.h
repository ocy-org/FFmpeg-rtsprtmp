// client/ThumbTool.h —— 用OpenCV从已下载的mp4抽帧存封面
// 关键认知：OpenCV的VideoCapture在PC端后端就是FFmpeg——两库天然协作
#pragma once
#include <QString>
#include <opencv2/videoio.hpp>
#include <opencv2/imgcodecs.hpp>

struct ThumbTool {
    // lastFrame=true → 取段尾帧（对应"缩略图取碰撞瞬间"的遗留优化项）
    static bool make(const QString& mp4, const QString& jpg, bool lastFrame = false) {
        cv::VideoCapture cap(mp4.toStdString());
        if (!cap.isOpened()) return false;
        if (lastFrame)
            cap.set(cv::CAP_PROP_POS_AVI_RATIO, 1.0);   // 定位到100%处=末帧
        cv::Mat bgr;
        if (!cap.read(bgr)) return false;
        return cv::imwrite(jpg.toStdString(), bgr,
                           {cv::IMWRITE_JPEG_QUALITY, 85});
    }
};
