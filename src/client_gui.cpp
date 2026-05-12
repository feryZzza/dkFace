#include "../include/attendance.hpp"
#include "../include/client.hpp"
#include "../include/face_recognition.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDialog>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QImage>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPixmap>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTabWidget>
#include <QTextEdit>
#include <QTime>
#include <QTimeEdit>
#include <QVBoxLayout>
#include <QWidget>

#include <atomic>
#include <chrono>
#include <functional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include <opencv2/opencv.hpp>

namespace {

QString toQString(const std::string& text) {
    return QString::fromUtf8(text.c_str());
}

std::string toStdString(const QString& text) {
    return text.trimmed().toUtf8().constData();
}

std::string registerRequest(const std::string& id, const std::string& name) {
    return "CLIENT_REGISTER|" + id + "|" + name + "|";
}

std::string markRequest(const std::string& id, const std::string& name,
                        const std::string& timeText) {
    return "CLIENT_MARK|" + id + "|" + name + "|" + timeText;
}

QImage matToImage(const cv::Mat& frame) {
    cv::Mat rgb;
    cv::cvtColor(frame, rgb, cv::COLOR_BGR2RGB);
    QImage image(rgb.data, rgb.cols, rgb.rows, static_cast<int>(rgb.step),
                 QImage::Format_RGB888);
    return image.copy();
}

class FaceCaptureDialog : public QDialog {
public:
    FaceCaptureDialog(const QString& title, int sampleCount, QWidget* parent = NULL)
        : QDialog(parent),
          sampleCount_(sampleCount),
          cancelled_(false),
          finished_(false),
          ok_(false) {
        setWindowTitle(title);
        resize(760, 620);
        setModal(true);

        QVBoxLayout* layout = new QVBoxLayout(this);
        layout->setContentsMargins(14, 14, 14, 14);
        layout->setSpacing(10);

        previewLabel_ = new QLabel(QStringLiteral("正在打开摄像头..."), this);
        previewLabel_->setAlignment(Qt::AlignCenter);
        previewLabel_->setMinimumSize(640, 480);
        previewLabel_->setStyleSheet("background: #111; color: white; border-radius: 6px;");

        statusLabel_ = new QLabel(QStringLiteral("请正对摄像头"), this);
        cancelButton_ =
            new QPushButton(style()->standardIcon(QStyle::SP_DialogCancelButton),
                            QStringLiteral("取消"), this);

        QHBoxLayout* bottom = new QHBoxLayout;
        bottom->addWidget(statusLabel_, 1);
        bottom->addWidget(cancelButton_);

        layout->addWidget(previewLabel_, 1);
        layout->addLayout(bottom);

        connect(cancelButton_, &QPushButton::clicked, this, [this]() { reject(); });
    }

    ~FaceCaptureDialog() {
        cancelled_.store(true);
        if (worker_.joinable()) worker_.join();
    }

    void start() {
        if (worker_.joinable()) return;

        QPointer<FaceCaptureDialog> self(this);
        QCoreApplication* app = QCoreApplication::instance();
        worker_ = std::thread([this, self, app]() { captureLoop(self, app); });
    }

    std::vector<cv::Mat> samples() const {
        return samples_;
    }

    std::string message() const {
        return message_;
    }

    void reject() {
        if (!finished_.load()) {
            cancelled_.store(true);
            statusLabel_->setText(QStringLiteral("正在关闭摄像头..."));
            cancelButton_->setEnabled(false);
            return;
        }
        QDialog::reject();
    }

private:
    void captureLoop(QPointer<FaceCaptureDialog> self, QCoreApplication* app) {
        std::vector<cv::Mat> collected;
        std::string message;
        bool ok = false;

        cv::CascadeClassifier cascade;
        if (!face::loadFaceCascadeForCapture(cascade, message)) {
            finishFromWorker(self, app, false, collected, message);
            return;
        }

        cv::VideoCapture camera;
        if (!camera.open(0, cv::CAP_V4L2)) {
            camera.open(0);
        }
        if (!camera.isOpened()) {
            finishFromWorker(self, app, false, collected,
                             "无法打开摄像头，请检查摄像头权限或设备连接");
            return;
        }

        camera.set(cv::CAP_PROP_BUFFERSIZE, 1);
        camera.set(cv::CAP_PROP_FRAME_WIDTH, 640);
        camera.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

        for (int i = 0; i < 10 && !cancelled_.load(); ++i) {
            cv::Mat warmupFrame;
            camera.read(warmupFrame);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        const int stableFrameCount = 8;
        const int sampleIntervalFrames = 6;
        int stableCount = 0;
        int framesSinceSample = sampleIntervalFrames;

        for (int i = 0; i < 360 &&
                        static_cast<int>(collected.size()) < sampleCount_ &&
                        !cancelled_.load();
             ++i) {
            cv::Mat frame;
            if (!camera.read(frame) || frame.empty()) {
                updateStatus(self, app, QStringLiteral("未读取到摄像头画面"));
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
                continue;
            }

            cv::Rect faceRect;
            QString status;
            if (face::findLargestFaceForCapture(frame, cascade, faceRect)) {
                ++stableCount;
                ++framesSinceSample;

                if (stableCount >= stableFrameCount &&
                    framesSinceSample >= sampleIntervalFrames) {
                    collected.push_back(face::normalizeFaceForCapture(frame, faceRect));
                    framesSinceSample = 0;
                }

                cv::rectangle(frame, faceRect, cv::Scalar(0, 220, 0), 2);
                status = QStringLiteral("Collected %1/%2, please look at camera")
                             .arg(static_cast<int>(collected.size()))
                             .arg(sampleCount_);
                cv::putText(frame, status.toUtf8().constData(), cv::Point(20, 35),
                            cv::FONT_HERSHEY_SIMPLEX, 0.7, cv::Scalar(0, 220, 0), 2);
            } else {
                stableCount = 0;
                framesSinceSample = sampleIntervalFrames;
                status = QStringLiteral("未检测到人脸，请调整光线并正对摄像头");
                cv::putText(frame, "Look at camera", cv::Point(20, 35),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);
            }

            updateFrame(self, app, matToImage(frame), status);
            std::this_thread::sleep_for(std::chrono::milliseconds(25));
        }

        camera.release();

        if (cancelled_.load()) {
            message = "已取消人脸采集";
        } else if (collected.empty()) {
            message = "未检测到人脸，请调整光线并正对摄像头";
        } else if (static_cast<int>(collected.size()) < sampleCount_) {
            std::ostringstream output;
            output << "人脸采集不足：需要 " << sampleCount_ << " 张，实际采集 "
                   << collected.size() << " 张，请保持正对摄像头";
            message = output.str();
        } else {
            ok = true;
            message = "人脸采集完成";
        }

        finishFromWorker(self, app, ok, collected, message);
    }

    void updateFrame(QPointer<FaceCaptureDialog> self, QCoreApplication* app,
                     const QImage& image, const QString& status) {
        if (!app) return;
        QMetaObject::invokeMethod(app, [self, image, status]() {
            if (!self) return;
            self->previewLabel_->setPixmap(QPixmap::fromImage(image).scaled(
                self->previewLabel_->size(), Qt::KeepAspectRatio,
                Qt::SmoothTransformation));
            self->statusLabel_->setText(status);
        }, Qt::QueuedConnection);
    }

    void updateStatus(QPointer<FaceCaptureDialog> self, QCoreApplication* app,
                      const QString& status) {
        if (!app) return;
        QMetaObject::invokeMethod(app, [self, status]() {
            if (!self) return;
            self->statusLabel_->setText(status);
        }, Qt::QueuedConnection);
    }

    void finishFromWorker(QPointer<FaceCaptureDialog> self, QCoreApplication* app,
                          bool ok, const std::vector<cv::Mat>& samples,
                          const std::string& message) {
        if (!app) return;
        QMetaObject::invokeMethod(app, [self, ok, samples, message]() {
            if (!self) return;
            self->finish(ok, samples, message);
        }, Qt::QueuedConnection);
    }

    void finish(bool ok, const std::vector<cv::Mat>& samples,
                const std::string& message) {
        ok_ = ok;
        samples_ = samples;
        message_ = message;
        finished_.store(true);
        cancelButton_->setEnabled(false);
        statusLabel_->setText(toQString(message));

        if (worker_.joinable()) worker_.join();

        if (ok_) {
            QDialog::accept();
        } else {
            QDialog::reject();
        }
    }

    QLabel* previewLabel_;
    QLabel* statusLabel_;
    QPushButton* cancelButton_;
    int sampleCount_;
    std::atomic<bool> cancelled_;
    std::atomic<bool> finished_;
    bool ok_;
    std::vector<cv::Mat> samples_;
    std::string message_;
    std::thread worker_;
};

class ClientWindow : public QWidget {
public:
    explicit ClientWindow(QWidget* parent = NULL) : QWidget(parent), busy_(false) {
        setWindowTitle(QStringLiteral("人脸考勤客户端"));
        resize(980, 700);

        QVBoxLayout* root = new QVBoxLayout(this);
        root->setContentsMargins(18, 18, 18, 18);
        root->setSpacing(12);

        root->addWidget(createConnectionBox());

        QTabWidget* tabs = new QTabWidget(this);
        tabs->addTab(createEmployeeTab(), QStringLiteral("员工"));
        tabs->addTab(createAttendanceTab(), QStringLiteral("打卡"));
        tabs->addTab(createFaceAndPlanTab(), QStringLiteral("刷脸与计划"));
        root->addWidget(tabs, 1);
        root->addWidget(createLogBox(), 1);

        setStyleSheet(
            "QWidget { font-size: 14px; }"
            "QGroupBox { font-weight: 600; border: 1px solid #d0d7de;"
            " border-radius: 6px; margin-top: 10px; padding-top: 10px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }"
            "QLineEdit, QSpinBox, QTimeEdit { min-height: 30px; padding: 2px 6px; }"
            "QPushButton { min-height: 32px; padding: 4px 12px; border-radius: 4px; }"
            "QTextEdit { border: 1px solid #d0d7de; border-radius: 6px; }");
    }

private:
    QWidget* createConnectionBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("服务器连接"), this);
        QHBoxLayout* layout = new QHBoxLayout(box);

        hostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"), box);
        hostEdit_->setMinimumWidth(180);
        portSpin_ = new QSpinBox(box);
        portSpin_->setRange(1, 65535);
        portSpin_->setValue(face::DEFAULT_PORT);

        QPushButton* testButton = actionButton(QStringLiteral("测试连接"),
                                               QStyle::SP_DialogApplyButton, box);
        statusLabel_ = new QLabel(QStringLiteral("未连接"), box);
        statusLabel_->setMinimumWidth(260);

        layout->addWidget(new QLabel(QStringLiteral("地址"), box));
        layout->addWidget(hostEdit_);
        layout->addWidget(new QLabel(QStringLiteral("端口"), box));
        layout->addWidget(portSpin_);
        layout->addWidget(testButton);
        layout->addWidget(statusLabel_, 1);

        connect(testButton, &QPushButton::clicked, this, [this]() {
            const std::string host = currentHost();
            const int port = currentPort();
            runTask(QStringLiteral("测试连接"), [host, port]() {
                std::string response =
                    face::sendClientRequest(host, port, "CLIENT_PING|||");
                return response;
            });
        });

        return box;
    }

    QWidget* createEmployeeTab() {
        QWidget* page = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(page);

        QGroupBox* formBox = new QGroupBox(QStringLiteral("员工注册"), page);
        QFormLayout* form = new QFormLayout(formBox);
        employeeIdEdit_ = new QLineEdit(formBox);
        employeeNameEdit_ = new QLineEdit(formBox);
        form->addRow(QStringLiteral("工号"), employeeIdEdit_);
        form->addRow(QStringLiteral("姓名"), employeeNameEdit_);

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* registerButton =
            actionButton(QStringLiteral("注册/更新"), QStyle::SP_DialogApplyButton, formBox);
        QPushButton* faceRegisterButton =
            actionButton(QStringLiteral("录入人脸并注册"), QStyle::SP_FileDialogContentsView,
                         formBox);
        buttons->addWidget(registerButton);
        buttons->addWidget(faceRegisterButton);
        buttons->addStretch(1);
        form->addRow(buttons);

        connect(registerButton, &QPushButton::clicked, this, [this]() {
            std::string id;
            if (!requireValue(employeeIdEdit_, QStringLiteral("工号"), id)) return;
            const std::string name = toStdString(employeeNameEdit_->text());
            const std::string host = currentHost();
            const int port = currentPort();
            runTask(QStringLiteral("注册员工"), [host, port, id, name]() {
                return face::sendClientRequest(host, port, registerRequest(id, name));
            });
        });

        connect(faceRegisterButton, &QPushButton::clicked, this, [this]() {
            std::string id;
            if (!requireValue(employeeIdEdit_, QStringLiteral("工号"), id)) return;
            const std::string name = toStdString(employeeNameEdit_->text());
            const std::string host = currentHost();
            const int port = currentPort();
            runCameraTask(QStringLiteral("录入人脸并注册"), [this, host, port, id, name]() {
                std::string faceMessage;
                if (!enrollFaceWithDialog(id, faceMessage)) {
                    throw std::runtime_error(faceMessage);
                }
                std::string response =
                    face::sendClientRequest(host, port, registerRequest(id, name));
                return faceMessage + "\n" + response;
            });
        });

        layout->addWidget(formBox);
        layout->addStretch(1);
        return page;
    }

    QWidget* createAttendanceTab() {
        QWidget* page = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(page);

        QGroupBox* formBox = new QGroupBox(QStringLiteral("手动打卡"), page);
        QFormLayout* form = new QFormLayout(formBox);
        markIdEdit_ = new QLineEdit(formBox);
        markNameEdit_ = new QLineEdit(formBox);
        markTimeCheck_ = new QCheckBox(QStringLiteral("指定打卡时间"), formBox);
        markTimeEdit_ = new QTimeEdit(QTime::currentTime(), formBox);
        markTimeEdit_->setDisplayFormat(QStringLiteral("HH:mm"));
        markTimeEdit_->setEnabled(false);
        connect(markTimeCheck_, &QCheckBox::toggled, markTimeEdit_, &QTimeEdit::setEnabled);

        QHBoxLayout* timeRow = new QHBoxLayout;
        timeRow->addWidget(markTimeCheck_);
        timeRow->addWidget(markTimeEdit_);
        timeRow->addStretch(1);

        form->addRow(QStringLiteral("工号"), markIdEdit_);
        form->addRow(QStringLiteral("姓名"), markNameEdit_);
        form->addRow(QStringLiteral("时间"), timeRow);

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* markButton =
            actionButton(QStringLiteral("手动打卡"), QStyle::SP_DialogApplyButton, formBox);
        QPushButton* faceMarkButton =
            actionButton(QStringLiteral("刷脸打卡"), QStyle::SP_ComputerIcon, formBox);
        buttons->addWidget(markButton);
        buttons->addWidget(faceMarkButton);
        buttons->addStretch(1);
        form->addRow(buttons);

        connect(markButton, &QPushButton::clicked, this, [this]() {
            std::string id;
            if (!requireValue(markIdEdit_, QStringLiteral("工号"), id)) return;
            const std::string name = toStdString(markNameEdit_->text());
            const std::string timeText = selectedMarkTime();
            const std::string host = currentHost();
            const int port = currentPort();
            runTask(QStringLiteral("手动打卡"), [host, port, id, name, timeText]() {
                return face::sendClientRequest(host, port, markRequest(id, name, timeText));
            });
        });

        connect(faceMarkButton, &QPushButton::clicked, this, [this]() {
            const std::string timeText = selectedMarkTime();
            const std::string host = currentHost();
            const int port = currentPort();
            runCameraTask(QStringLiteral("刷脸打卡"), [this, host, port, timeText]() {
                std::string id;
                std::string faceMessage;
                double score = 0.0;
                if (!recognizeFaceWithDialog(id, score, faceMessage)) {
                    throw std::runtime_error(faceMessage);
                }
                std::string response =
                    face::sendClientRequest(host, port, markRequest(id, "", timeText));
                return faceMessage + "\n" + response;
            });
        });

        layout->addWidget(formBox);
        layout->addStretch(1);
        return page;
    }

    QWidget* createFaceAndPlanTab() {
        QWidget* page = new QWidget(this);
        QVBoxLayout* layout = new QVBoxLayout(page);

        QGroupBox* faceBox = new QGroupBox(QStringLiteral("刷脸查询"), page);
        QHBoxLayout* faceLayout = new QHBoxLayout(faceBox);
        QPushButton* identifyButton =
            actionButton(QStringLiteral("识别人脸"), QStyle::SP_ComputerIcon, faceBox);
        QPushButton* salaryButton =
            actionButton(QStringLiteral("刷脸查询工资"), QStyle::SP_FileDialogDetailedView,
                         faceBox);
        faceLayout->addWidget(identifyButton);
        faceLayout->addWidget(salaryButton);
        faceLayout->addStretch(1);

        QGroupBox* planBox = new QGroupBox(QStringLiteral("激励计划与删除"), page);
        QFormLayout* planForm = new QFormLayout(planBox);
        planIdEdit_ = new QLineEdit(planBox);
        planForm->addRow(QStringLiteral("目标工号"), planIdEdit_);

        QHBoxLayout* planButtons = new QHBoxLayout;
        QPushButton* hardworkButton =
            actionButton(QStringLiteral("加入激励计划"), QStyle::SP_ArrowUp, planBox);
        QPushButton* normalButton =
            actionButton(QStringLiteral("退出激励计划"), QStyle::SP_ArrowDown, planBox);
        QPushButton* deleteButton =
            actionButton(QStringLiteral("删除员工"), QStyle::SP_TrashIcon, planBox);
        planButtons->addWidget(hardworkButton);
        planButtons->addWidget(normalButton);
        planButtons->addWidget(deleteButton);
        planButtons->addStretch(1);
        planForm->addRow(planButtons);

        connect(identifyButton, &QPushButton::clicked, this, [this]() {
            runCameraTask(QStringLiteral("识别人脸"), [this]() {
                std::string id;
                std::string faceMessage;
                double score = 0.0;
                if (!recognizeFaceWithDialog(id, score, faceMessage)) {
                    throw std::runtime_error(faceMessage);
                }
                return faceMessage;
            });
        });

        connect(salaryButton, &QPushButton::clicked, this, [this]() {
            const std::string host = currentHost();
            const int port = currentPort();
            runCameraTask(QStringLiteral("刷脸查询工资"), [this, host, port]() {
                std::string id;
                std::string faceMessage;
                double score = 0.0;
                if (!recognizeFaceWithDialog(id, score, faceMessage)) {
                    throw std::runtime_error(faceMessage);
                }
                std::string response =
                    face::sendClientRequest(host, port, "CLIENT_QUERY|" + id + "||");
                return faceMessage + "\n" + response;
            });
        });

        connect(hardworkButton, &QPushButton::clicked, this, [this]() {
            startProtectedPlanTask(QStringLiteral("加入激励计划"), "CLIENT_HARDWORK");
        });
        connect(normalButton, &QPushButton::clicked, this, [this]() {
            startProtectedPlanTask(QStringLiteral("退出激励计划"), "CLIENT_NORMAL");
        });
        connect(deleteButton, &QPushButton::clicked, this, [this]() {
            std::string id;
            if (!requireValue(planIdEdit_, QStringLiteral("目标工号"), id)) return;
            if (QMessageBox::question(this, QStringLiteral("确认删除"),
                                      QStringLiteral("确认删除该员工全部信息？")) !=
                QMessageBox::Yes) {
                return;
            }
            startProtectedPlanTask(QStringLiteral("删除员工"), "CLIENT_DELETE", id);
        });

        layout->addWidget(faceBox);
        layout->addWidget(planBox);
        layout->addStretch(1);
        return page;
    }

    QWidget* createLogBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("操作结果"), this);
        QVBoxLayout* layout = new QVBoxLayout(box);
        logEdit_ = new QTextEdit(box);
        logEdit_->setReadOnly(true);
        logEdit_->setMinimumHeight(190);

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* clearButton =
            actionButton(QStringLiteral("清空"), QStyle::SP_DialogResetButton, box);
        buttons->addStretch(1);
        buttons->addWidget(clearButton);

        connect(clearButton, &QPushButton::clicked, logEdit_, &QTextEdit::clear);

        layout->addWidget(logEdit_);
        layout->addLayout(buttons);
        return box;
    }

    QPushButton* actionButton(const QString& text, QStyle::StandardPixmap icon,
                              QWidget* parent) {
        QPushButton* button = new QPushButton(style()->standardIcon(icon), text, parent);
        actionButtons_.push_back(button);
        return button;
    }

    bool requireValue(QLineEdit* edit, const QString& label, std::string& value) {
        value = toStdString(edit->text());
        if (!value.empty()) return true;

        QMessageBox::warning(this, QStringLiteral("输入不完整"),
                             label + QStringLiteral("不能为空"));
        edit->setFocus();
        return false;
    }

    std::string currentHost() const {
        std::string host = toStdString(hostEdit_->text());
        return host.empty() ? "127.0.0.1" : host;
    }

    int currentPort() const {
        return portSpin_->value();
    }

    std::string selectedMarkTime() const {
        if (!markTimeCheck_->isChecked()) return "";
        return markTimeEdit_->time().toString(QStringLiteral("HH:mm")).toStdString();
    }

    bool captureFaceSamplesWithDialog(const QString& title, int sampleCount,
                                      std::vector<cv::Mat>& samples,
                                      std::string& message) {
        FaceCaptureDialog dialog(title, sampleCount, this);
        dialog.start();
        if (dialog.exec() != QDialog::Accepted) {
            message = dialog.message();
            return false;
        }

        samples = dialog.samples();
        message = dialog.message();
        return true;
    }

    bool enrollFaceWithDialog(const std::string& employeeId, std::string& message) {
        std::vector<cv::Mat> samples;
        if (!captureFaceSamplesWithDialog(QStringLiteral("录入人脸"),
                                          face::enrollFaceSampleCount(),
                                          samples, message)) {
            return false;
        }

        return face::saveFaceEnrollmentSamples(employeeId, samples, message);
    }

    bool recognizeFaceWithDialog(std::string& employeeId, double& score,
                                 std::string& message) {
        std::vector<cv::Mat> samples;
        if (!captureFaceSamplesWithDialog(QStringLiteral("识别人脸"),
                                          face::recognizeFaceSampleCount(),
                                          samples, message)) {
            return false;
        }

        return face::recognizeFaceSamples(samples, employeeId, score, message);
    }

    void startProtectedPlanTask(const QString& title, const std::string& requestType,
                                const std::string& knownId = std::string()) {
        std::string id = knownId;
        if (id.empty() && !requireValue(planIdEdit_, QStringLiteral("目标工号"), id)) return;

        const std::string host = currentHost();
        const int port = currentPort();
        runCameraTask(title, [this, host, port, id, requestType, title]() {
            std::string confirmedId;
            std::string faceMessage;
            double score = 0.0;
            if (!recognizeFaceWithDialog(confirmedId, score, faceMessage)) {
                throw std::runtime_error(faceMessage);
            }
            if (confirmedId != id) {
                std::string action = title.toUtf8().constData();
                throw std::runtime_error(action + "失败：识别工号 " + confirmedId +
                                         " 与目标工号 " + id + " 不一致\n" +
                                         faceMessage);
            }

            std::string response =
                face::sendClientRequest(host, port, requestType + "|" + confirmedId + "||");
            return faceMessage + "\n" + response;
        });
    }

    void runTask(const QString& title, std::function<std::string()> task) {
        if (busy_) {
            QMessageBox::information(this, QStringLiteral("正在执行"),
                                     QStringLiteral("请等待当前操作完成"));
            return;
        }

        setBusy(true);
        appendLog(QStringLiteral("开始：") + title);

        QPointer<ClientWindow> self(this);
        QCoreApplication* app = QCoreApplication::instance();
        std::thread([self, app, title, task]() {
            bool ok = true;
            std::string result;
            try {
                result = task();
            } catch (const std::exception& error) {
                ok = false;
                result = error.what();
            } catch (...) {
                ok = false;
                result = "操作失败：未知错误";
            }

            if (!app) return;
            QMetaObject::invokeMethod(app, [self, title, ok, result]() {
                if (!self) return;
                self->appendLog((ok ? QStringLiteral("完成：") : QStringLiteral("失败：")) +
                                title + QStringLiteral("\n") + toQString(result));
                self->statusLabel_->setText(ok ? QStringLiteral("操作完成")
                                               : QStringLiteral("操作失败"));
                self->setBusy(false);
            }, Qt::QueuedConnection);
        }).detach();
    }

    void runCameraTask(const QString& title, std::function<std::string()> task) {
        if (busy_) {
            QMessageBox::information(this, QStringLiteral("正在执行"),
                                     QStringLiteral("请等待当前操作完成"));
            return;
        }

        setBusy(true);
        appendLog(QStringLiteral("开始：") + title);

        bool ok = true;
        std::string result;
        try {
            result = task();
        } catch (const std::exception& error) {
            ok = false;
            result = error.what();
        } catch (...) {
            ok = false;
            result = "操作失败：未知错误";
        }

        appendLog((ok ? QStringLiteral("完成：") : QStringLiteral("失败：")) +
                  title + QStringLiteral("\n") + toQString(result));
        statusLabel_->setText(ok ? QStringLiteral("操作完成")
                                 : QStringLiteral("操作失败"));
        setBusy(false);
    }

    void appendLog(const QString& text) {
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        logEdit_->append(stamp + QStringLiteral("  ") + text);
    }

    void setBusy(bool busy) {
        busy_ = busy;
        for (std::size_t i = 0; i < actionButtons_.size(); ++i) {
            actionButtons_[i]->setEnabled(!busy);
        }
        if (busy) {
            statusLabel_->setText(QStringLiteral("正在执行操作"));
        }
    }

    bool busy_;
    QLineEdit* hostEdit_;
    QSpinBox* portSpin_;
    QLabel* statusLabel_;
    QTextEdit* logEdit_;
    QLineEdit* employeeIdEdit_;
    QLineEdit* employeeNameEdit_;
    QLineEdit* markIdEdit_;
    QLineEdit* markNameEdit_;
    QCheckBox* markTimeCheck_;
    QTimeEdit* markTimeEdit_;
    QLineEdit* planIdEdit_;
    std::vector<QPushButton*> actionButtons_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    ClientWindow window;
    window.show();
    return app.exec();
}
