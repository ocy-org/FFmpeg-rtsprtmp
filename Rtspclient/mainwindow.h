#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <QCloseEvent>

class FFmpegWorker;              // 前置声明即可，FFmpeg 头文件放 .cpp

QT_BEGIN_NAMESPACE
namespace Ui { class MainWindow; }
QT_END_NAMESPACE

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;

private slots:
    void on_startBtn_clicked();
    void on_stopBtn_clicked();

private:
    void enumerateCameras();     // 枚举本机摄像头，自动填入 cameraEdit

    Ui::MainWindow *ui = nullptr;
    FFmpegWorker   *m_worker = nullptr;
};

#endif // MAINWINDOW_H
