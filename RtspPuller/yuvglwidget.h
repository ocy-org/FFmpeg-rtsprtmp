// 本文件：用 OpenGL 把 YUV420 三个平面（Y/U/V）上传为 3 张纹理，
//         再在片元着色器里做 YUV->RGB 颜色空间转换，实现 GPU 直渲，
//         避免 CPU 端转 RGB 的开销，是低延迟播放器显示端的核心。
#pragma once
#include <QOpenGLWidget>                      // Qt 的 OpenGL 渲染窗口控件（继承自 QWidget）
#include <QOpenGLFunctions>                   // 提供跨平台的 GL 函数封装（glBindTexture 等）
#include <QOpenGLShaderProgram>               // 顶点/片元着色器程序的封装（编译+链接+绑定）
#include <QOpenGLBuffer>                       // VBO（顶点缓冲对象）封装，存放全屏四边形顶点
#include <memory>                             // std::shared_ptr，用于跨线程安全共享 VideoFrame
#include "FrameHub.h"                          // 帧中转枢纽：解码线程 publish、GUI 线程 grab
#include <QTimer>
struct VideoFrame;

// YuvGLWidget：显示端控件
class YuvGLWidget : public QOpenGLWidget, protected QOpenGLFunctions {
    Q_OBJECT                                  // 启用 Qt 元对象（信号槽），需被 moc 处理
public:
    // 构造：只做“装配”，真正建 GL 资源在 initializeGL() 里做（那时才有 current GL context）
    explicit YuvGLWidget(QWidget* parent = nullptr) : QOpenGLWidget(parent) {
        pullTimer_.setInterval(33);              // ~30Hz拉取，≤1帧显示延迟（33ms≈1/30秒）
        // 定时器到点即从 FrameHub 抢最新帧；用 lambda 版 connect 不必依赖具名槽
        connect(&pullTimer_, &QTimer::timeout, this, [this]{
            if (!hub_) return;                   // 还没 attach 枢纽：本轮什么都不做
            quint64 s0 = lastSeq_;               // 记录取帧前的帧序号快照（用于丢帧统计）
            auto f = hub_->grab(lastSeq_);       // 取“比 lastSeq_ 更新”的最新一帧；无新帧返回空
            if (f) { pending_ = std::move(f);    // 有新帧：转移所有权到 pending_，等待 paintGL 绘制
                     if (hub_->seq() - s0 > 1) ++dropped_;   // 被覆盖掉的帧数=丢帧统计
                     update(); }                 // 请求重绘，触发 paintGL（异步、非阻塞）
        });
        pullTimer_.start();                      // 启动拉帧循环（构造即开始轮询枢纽）
    }
    void attach(FrameHub* h) { hub_ = h; }       // MainWindow里接线：把解码线程的枢纽交给我
    quint64 dropped_ = 0;                        // 累计丢帧数（被更快新帧覆盖的帧），供上层显示

protected:
    void initializeGL() override;               // GL 上下文就绪后回调一次：建着色器/VBO/纹理
    void paintGL() override;                    // 每次 update()/重绘回调：上传帧+绘制
private:
    // 把一个 YUV 平面（plane）上传到指定纹理 tex；stride=源每行字节数，w/h=该平面宽高
    void upload(GLuint tex, const QByteArray& plane,
                int stride, int w, int h);
    QOpenGLShaderProgram prog_;                 // YUV->RGB 的着色器程序（顶点+片元）
    QOpenGLBuffer vbo_{QOpenGLBuffer::VertexBuffer};   // 全屏四边形顶点缓冲（三角形带）
    GLuint tex_[3] = {0, 0, 0};                 // 3 张纹理句柄：[0]=Y [1]=U [2]=V
    std::shared_ptr<VideoFrame> pending_;       // 待绘制帧（由定时器线程写入，paintGL 读取）
    FrameHub* hub_ = nullptr;                   // 帧枢纽指针（attach 后有效，跨线程取帧入口）
    QTimer pullTimer_;                          // ~30Hz 拉帧定时器（运行在 GUI 线程）
    quint64 lastSeq_ = 0;                       // 上一次成功取到的帧序号，保证只取更新的帧
};
