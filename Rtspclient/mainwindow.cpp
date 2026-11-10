#include "mainwindow.h"
#include "ui_mainwindow.h"
#include "ffmpegworker.h"

extern "C" {
#include <libavdevice/avdevice.h>
#include <libavdevice/version.h>
#include <libavformat/avformat.h>
}

#include <QMessageBox>
#include <QDebug>

// UI 显示文本 → dshow 参数值
static QString formatKey(const QString &uiText)
{
    const QString s = uiText.toLower();
    if (s.contains("mjpeg")) return "mjpeg";
    if (s.contains("yuyv") || s.contains("yuy2")) return "yuyv422";
    return "auto";
}

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
    , ui(new Ui::MainWindow)
{
    ui->setupUi(this);
    setWindowTitle("Qt + FFmpeg 摄像头 RTSP 推流");

    enumerateCameras();
}

MainWindow::~MainWindow()
{
    delete ui;
}

void MainWindow::enumerateCameras()
{
    avdevice_register_all();

#if LIBAVDEVICE_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
    const AVInputFormat *ifmt = av_find_input_format("dshow");
#else
    AVInputFormat *ifmt = av_find_input_format("dshow");
#endif
    if (!ifmt) return;

    AVDeviceInfoList *devList = nullptr;
    if (avdevice_list_input_sources(ifmt, nullptr, nullptr, &devList) < 0 || !devList)
        return;

    QStringList cameras, candidates;
    for (int i = 0; i < devList->nb_devices; ++i) {
        const AVDeviceInfo *d = devList->devices[i];
        const QString name = QString::fromUtf8(d->device_name);
        qDebug() << "[Enum] 设备:" << name
                 << "描述:" << (d->device_description ? d->device_description : "");

#if LIBAVDEVICE_VERSION_INT >= AV_VERSION_INT(59, 0, 100)
        for (int j = 0; j < d->nb_media_types; ++j) {
            if (d->media_types[j] == AVMEDIA_TYPE_VIDEO) { cameras << name; break; }
        }
#else
        const QString s = name.toLower();
        if (s.contains("cam") || s.contains("video") || s.contains("uvc")
                || name.contains("摄像"))
            cameras << name;
        else if (!s.contains("mic") && !s.contains("audio")
                 && !name.contains("麦克风") && !name.contains("音频"))
            candidates << name;
#endif
    }
    avdevice_free_list_devices(&devList);

    const QStringList names = cameras + candidates;
    qDebug() << "[Enum] 检测到摄像头:" << names;
    if (!names.isEmpty())
        ui->cameraEdit->setText(names.first());
}

void MainWindow::on_startBtn_clicked()
{
    if (m_worker) return;

    QString camera = ui->cameraEdit->text().trimmed();
    if (camera.isEmpty()) {
        QMessageBox::warning(this, "提示", "未检测到摄像头，请确认设备未被其他程序占用");
        return;
    }
    QString url = ui->rtspEdit->text().trimmed();
    if (url.isEmpty()) url = "rtsp://127.0.0.1:8554/live";

    QString res = ui->resCombo->currentText().trimmed();
    if (res == "自动") res.clear();              // 空串 = 不设置 video_size
    QString fmt = formatKey(ui->fmtCombo->currentText());

    m_worker = new FFmpegWorker(camera, url, res, fmt, this);

    connect(m_worker, &FFmpegWorker::stateChanged,
            ui->statusLabel, &QLabel::setText);
    connect(m_worker, &FFmpegWorker::errorOccurred, this,
            [this](const QString &err) {
                QMessageBox::warning(this, "错误", err);
                on_stopBtn_clicked();
            });

    m_worker->start();
    ui->statusLabel->setText("正在连接…");
    ui->startBtn->setEnabled(false);
    ui->stopBtn->setEnabled(true);
}

void MainWindow::on_stopBtn_clicked()
{
    if (!m_worker) return;

    m_worker->stop();
    m_worker->wait();
    m_worker->deleteLater();
    m_worker = nullptr;

    ui->startBtn->setEnabled(true);
    ui->stopBtn->setEnabled(false);
}

void MainWindow::closeEvent(QCloseEvent *event)
{
    on_stopBtn_clicked();
    event->accept();
}
