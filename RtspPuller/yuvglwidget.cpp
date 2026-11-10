// 本文件：YuvGLWidget 的 OpenGL 实现——着色器源码、初始化、平面上传、绘制。
#include "yuvglwidget.h"
#include "VideoFrame.h"            // 完整定义：paintGL 里要读 f->y/u/v、w/h、stride 等成员

// ---- 顶点着色器（GLSL 1.x / GLES2 风格）----
// 输入 pos：-1~1 的全屏四边形顶点坐标；输出 vUV：对应的纹理坐标。
// 关键：vUV.t = 1.0 - vUV.t 做 Y 轴翻转——因为内存里 YUV 首行是图像顶部，
//       而 GL 纹理坐标原点在左下，不翻转会上下颠倒。
static const char* VSH =
    "attribute vec2 pos; varying vec2 vUV;\n"
    "void main(){ vUV = (pos + 1.0) * 0.5; vUV.t = 1.0 - vUV.t;\n"
    "  gl_Position = vec4(pos, 0.0, 1.0); }";

// BT.601 全范围转换；若画面发灰改用limited range: rgb = 1.164*(y-0.0625) + ...
// ---- 片元着色器：核心 YUV->RGB 数学 ----
// 三个采样器分别取 Y/U/V；U、V 减 0.5 把 [0,1] 还原到 [-0.5,0.5] 的色度偏差，
// 再套用 BT.601 系数：R=Y+1.402V, G=Y-0.344U-0.714V, B=Y+1.772U。
static const char* FSH =
    "uniform sampler2D texY, texU, texV; varying vec2 vUV;\n"
    "void main(){\n"
    "  float y = texture2D(texY, vUV).r;\n"           // Y 亮度（单通道，存于 .r）
    "  float u = texture2D(texU, vUV).r - 0.5;\n"     // U 色度（去偏置）
    "  float v = texture2D(texV, vUV).r - 0.5;\n"     // V 色度（去偏置）
    "  gl_FragColor = vec4(y + 1.402*v, y - 0.344*u - 0.714*v, y + 1.772*u, 1.0);\n"
    "}";

// initializeGL：GL 上下文已就绪，整个控件生命周期只调用一次，用来创建常驻 GPU 资源。
void YuvGLWidget::initializeGL() {
    initializeOpenGLFunctions();                 // 解析本上下文可用的 GL 函数指针（QOpenGLFunctions 要求）
    prog_.addShaderFromSourceCode(QOpenGLShader::Vertex, VSH);    // 编译顶点着色器
    prog_.addShaderFromSourceCode(QOpenGLShader::Fragment, FSH);  // 编译片元着色器
    prog_.link();                                // 链接成可执行程序
    const float quad[] = {-1,-1, 1,-1, -1,1, 1,1};      // 三角带：4 顶点铺满 NDC 全屏
    vbo_.create(); vbo_.bind(); vbo_.allocate(quad, sizeof(quad)); // 建 VBO 并把顶点送到显存
    glGenTextures(3, tex_);                      // 申请 3 个纹理对象（Y/U/V 各一）
}

// upload：把一块 YUV 平面（一维字节）上传为单通道纹理。
// plane=平面数据，stride=源每行有效字节数（可能因对齐>w），w/h=该平面显示宽高。
void YuvGLWidget::upload(GLuint tex, const QByteArray& plane,
                         int stride, int w, int h) {      // ★签名加w
    glBindTexture(GL_TEXTURE_2D, tex);           // 绑定目标纹理，后续操作作用于它
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);       // 行按 1 字节对齐（默认 4，会错位 YUV）
    glPixelStorei(GL_UNPACK_ROW_LENGTH, stride); // 源数据行距=stride（跳过得码器的行填充）
    // ★修复B：GL_LUMINANCE 在 GL2.1/ES2/GL3兼容档全线可用（GL_R8要GL3.0+）
    // 用 GL_LUMINANCE 把单通道灰度当作 .r 读，兼容性最好
    glTexImage2D(GL_TEXTURE_2D, 0, GL_LUMINANCE, w, h, 0,
                 GL_LUMINANCE, GL_UNSIGNED_BYTE, plane.constData());
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);      // 复位行距，避免影响之后的纹理上传
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); // 缩小用线性插值
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR); // 放大用线性插值
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); // 横向边缘钳位，防采样越界
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE); // 纵向边缘钳位
}


// paintGL：每次重绘回调，负责“上传当前帧三平面 -> 按信箱比例设视口 -> 画全屏四边形”。
void YuvGLWidget::paintGL() {
    glClearColor(0, 0, 0, 1); glClear(GL_COLOR_BUFFER_BIT);  // 清屏为黑（信箱上下/左右黑边）
    if (!pending_) return;                       // 没有待绘帧：保持黑屏直接返回
    const auto f = pending_;                     // 取出帧的本地副本（shared_ptr，引用计数安全）

    upload(tex_[0], f->y, f->yStride, f->w,     f->h);       // Y 平面：全分辨率
    upload(tex_[1], f->u, f->uStride, f->w / 2, f->h / 2);   // U 平面：420 色度为 1/4（宽高各半）
    upload(tex_[2], f->v, f->vStride, f->w / 2, f->h / 2);   // V 平面：同上

    // 信箱式适配：任意窗口比例下保持16:9不拉伸
    // fa=视频宽高比，wa=窗口宽高比；取较小轴缩放 1.0，较大轴缩到比例匹配，剩余即黑边
    const double fa = double(f->w) / f->h, wa = double(width()) / height();
    double sx = fa < wa ? fa / wa : 1.0, sy = fa < wa ? 1.0 : wa / fa;
    glViewport(int(width() * (1 - sx) / 2), int(height() * (1 - sy) / 2),  // 居中偏移
               int(width() * sx), int(height() * sy));                     // 实际绘制区域大小

    prog_.bind();                                // 启用 YUV->RGB 着色器程序
    prog_.setUniformValue("texY", 0); prog_.setUniformValue("texU", 1);  // 片元里三个采样器
    prog_.setUniformValue("texV", 2);            // 分别绑定到纹理单元 0/1/2
    vbo_.bind();                                 // 绑定全屏四边形顶点缓冲
    prog_.enableAttributeArray("pos");           // 启用顶点属性 pos
    prog_.setAttributeBuffer("pos", GL_FLOAT, 0, 2); // 描述 pos：float、偏移0、每顶点2分量
    for (int i = 0; i < 3; ++i) {                // 把 Y/U/V 三纹理分别激活到单元 0/1/2
        glActiveTexture(GL_TEXTURE0 + i); glBindTexture(GL_TEXTURE_2D, tex_[i]);
    }
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);       // 一次画 4 顶点三角形带 = 整屏
}
