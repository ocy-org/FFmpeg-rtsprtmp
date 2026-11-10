#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include <QMainWindow>
#include <memory>

class VideoWorker;
struct VideoFrame;

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
    void onStart();
    void onStop();
    void onSnapshot();

private:
    Ui::MainWindow *ui = nullptr;
    VideoWorker *worker_ = nullptr;

    quint64 snapshotSeq_ = 0;   // 截图线程独立的取帧游标
    bool     saving_      = false;   // 防重入：保存期间禁用按钮
};

#endif // MAINWINDOW_H
