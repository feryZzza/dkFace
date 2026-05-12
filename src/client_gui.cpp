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
#include <QTextCursor>
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

enum FeedbackKind {
    FeedbackInfo,
    FeedbackSuccess,
    FeedbackWarning,
    FeedbackError
};

QString toQString(const std::string& text) {
    return QString::fromUtf8(text.c_str());
}

std::string toStdString(const QString& text) {
    return text.trimmed().toUtf8().constData();
}

QString escapedHtml(const QString& text) {
    return text.toHtmlEscaped().replace(QStringLiteral("\n"), QStringLiteral("<br>"));
}

const char* feedbackState(FeedbackKind kind) {
    switch (kind) {
        case FeedbackSuccess:
            return "ok";
        case FeedbackWarning:
            return "warning";
        case FeedbackError:
            return "error";
        case FeedbackInfo:
        default:
            return "info";
    }
}

QString feedbackText(FeedbackKind kind) {
    switch (kind) {
        case FeedbackSuccess:
            return QStringLiteral("完成");
        case FeedbackWarning:
            return QStringLiteral("注意");
        case FeedbackError:
            return QStringLiteral("失败");
        case FeedbackInfo:
        default:
            return QStringLiteral("进行中");
    }
}

QString feedbackColor(FeedbackKind kind) {
    switch (kind) {
        case FeedbackSuccess:
            return QStringLiteral("#1f7a4d");
        case FeedbackWarning:
            return QStringLiteral("#9a6700");
        case FeedbackError:
            return QStringLiteral("#b42318");
        case FeedbackInfo:
        default:
            return QStringLiteral("#0969da");
    }
}

QString feedbackBackground(FeedbackKind kind) {
    switch (kind) {
        case FeedbackSuccess:
            return QStringLiteral("#ecfdf3");
        case FeedbackWarning:
            return QStringLiteral("#fff8c5");
        case FeedbackError:
            return QStringLiteral("#ffebe9");
        case FeedbackInfo:
        default:
            return QStringLiteral("#ddf4ff");
    }
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
        layout->setContentsMargins(18, 18, 18, 18);
        layout->setSpacing(12);

        previewLabel_ = new QLabel(QStringLiteral("正在打开摄像头..."), this);
        previewLabel_->setAlignment(Qt::AlignCenter);
        previewLabel_->setMinimumSize(640, 480);
        previewLabel_->setStyleSheet(
            "background: #111827; color: white; border-radius: 8px;"
            "font-size: 16px; font-weight: 600;");

        statusLabel_ = new QLabel(QStringLiteral("请正对摄像头"), this);
        statusLabel_->setObjectName(QStringLiteral("captureStatus"));
        cancelButton_ =
            new QPushButton(style()->standardIcon(QStyle::SP_DialogCancelButton),
                            QStringLiteral("取消"), this);
        cancelButton_->setProperty("role", "secondary");

        QHBoxLayout* bottom = new QHBoxLayout;
        bottom->addWidget(statusLabel_, 1);
        bottom->addWidget(cancelButton_);

        layout->addWidget(previewLabel_, 1);
        layout->addLayout(bottom);

        setStyleSheet(
            "QDialog { background: #f6f8fb; color: #1f2937; }"
            "QLabel#captureStatus { background: #ffffff; border: 1px solid #d0d7de;"
            " border-radius: 6px; padding: 8px 10px; color: #374151; }"
            "QPushButton { min-height: 34px; padding: 5px 14px; border-radius: 5px;"
            " border: 1px solid #c9d1d9; background: #ffffff; color: #24292f; }"
            "QPushButton:hover { background: #f3f4f6; }"
            "QPushButton:disabled { color: #8c959f; background: #f6f8fa; }");

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
                status = QStringLiteral("已采集 %1/%2，请保持正对摄像头")
                             .arg(static_cast<int>(collected.size()))
                             .arg(sampleCount_);
                std::ostringstream overlayText;
                overlayText << "Samples " << collected.size() << "/" << sampleCount_;
                cv::putText(frame, overlayText.str(), cv::Point(20, 35),
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
        resize(1600, 1000);

        QVBoxLayout* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 20, 22, 20);
        root->setSpacing(14);

        QLabel* title = new QLabel(QStringLiteral("人脸考勤客户端"), this);
        title->setObjectName(QStringLiteral("pageTitle"));
        QLabel* subtitle =
            new QLabel(QStringLiteral("员工注册、打卡、人脸认证和计划管理"), this);
        subtitle->setObjectName(QStringLiteral("pageSubtitle"));
        root->addWidget(title);
        root->addWidget(subtitle);

        root->addWidget(createConnectionBox());

        QTabWidget* tabs = new QTabWidget(this);
        tabs->setObjectName(QStringLiteral("mainTabs"));
        tabs->addTab(createEmployeeTab(), QStringLiteral("员工"));
        tabs->addTab(createAttendanceTab(), QStringLiteral("打卡"));
        tabs->addTab(createFaceAndPlanTab(), QStringLiteral("刷脸与计划"));
        root->addWidget(tabs, 1);
        root->addWidget(createLogBox(), 1);

        setStyleSheet(
            "QWidget { background: #f6f8fb; color: #1f2937; font-size: 14px; }"
            "QLabel#pageTitle { font-size: 24px; font-weight: 700; color: #111827; }"
            "QLabel#pageSubtitle { color: #6b7280; padding-bottom: 2px; }"
            "QGroupBox { background: #ffffff; font-weight: 600; border: 1px solid #d0d7de;"
            " border-radius: 8px; margin-top: 12px; padding: 14px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px;"
            " color: #374151; background: #f6f8fb; }"
            "QTabWidget::pane { border: 1px solid #d0d7de; border-radius: 8px;"
            " background: #ffffff; top: -1px; }"
            "QTabBar::tab { background: #eef2f7; color: #4b5563; min-width: 108px;"
            " padding: 9px 16px; border: 1px solid #d0d7de;"
            " border-top-left-radius: 6px; border-top-right-radius: 6px; }"
            "QTabBar::tab:selected { background: #ffffff; color: #0969da;"
            " border-bottom-color: #ffffff; font-weight: 600; }"
            "QLineEdit, QSpinBox, QTimeEdit { background: #ffffff; min-height: 32px;"
            " padding: 3px 8px; border: 1px solid #c9d1d9; border-radius: 5px; }"
            "QLineEdit:focus, QSpinBox:focus, QTimeEdit:focus { border: 1px solid #0969da; }"
            "QCheckBox { spacing: 8px; }"
            "QPushButton { min-height: 34px; padding: 5px 14px; border-radius: 5px;"
            " border: 1px solid #c9d1d9; background: #ffffff; color: #24292f;"
            " font-weight: 600; }"
            "QPushButton:hover { background: #f3f4f6; }"
            "QPushButton:pressed { background: #eaeef2; }"
            "QPushButton:disabled { color: #8c959f; background: #f6f8fa; }"
            "QPushButton[role=\"primary\"] { background: #1f7a4d; border-color: #1f7a4d;"
            " color: #ffffff; }"
            "QPushButton[role=\"primary\"]:hover { background: #17623d; }"
            "QPushButton[role=\"accent\"] { background: #0969da; border-color: #0969da;"
            " color: #ffffff; }"
            "QPushButton[role=\"accent\"]:hover { background: #0757b8; }"
            "QPushButton[role=\"danger\"] { background: #b42318; border-color: #b42318;"
            " color: #ffffff; }"
            "QPushButton[role=\"danger\"]:hover { background: #912018; }"
            "QLabel#statusBadge { border-radius: 12px; padding: 5px 12px;"
            " font-weight: 600; border: 1px solid transparent; }"
            "QLabel#statusBadge[state=\"idle\"] { color: #57606a; background: #f6f8fa;"
            " border-color: #d0d7de; }"
            "QLabel#statusBadge[state=\"busy\"] { color: #0969da; background: #ddf4ff;"
            " border-color: #54aeff; }"
            "QLabel#statusBadge[state=\"ok\"] { color: #1f7a4d; background: #ecfdf3;"
            " border-color: #8ee0ad; }"
            "QLabel#statusBadge[state=\"warning\"] { color: #9a6700; background: #fff8c5;"
            " border-color: #eac54f; }"
            "QLabel#statusBadge[state=\"error\"] { color: #b42318; background: #ffebe9;"
            " border-color: #ffaba8; }"
            "QTextEdit { background: #ffffff; border: 1px solid #d0d7de;"
            " border-radius: 8px; padding: 8px; }");
    }

private:
    QWidget* createConnectionBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("服务器连接"), this);
        QHBoxLayout* layout = new QHBoxLayout(box);

        hostEdit_ = new QLineEdit(QStringLiteral("127.0.0.1"), box);
        hostEdit_->setPlaceholderText(QStringLiteral("服务器 IP"));
        hostEdit_->setMinimumWidth(180);
        portSpin_ = new QSpinBox(box);
        portSpin_->setRange(1, 65535);
        portSpin_->setValue(face::DEFAULT_PORT);

        QPushButton* testButton = actionButton(QStringLiteral("测试连接"),
                                               QStyle::SP_DialogApplyButton, box, "accent");
        statusLabel_ = new QLabel(QStringLiteral("未连接"), box);
        statusLabel_->setObjectName(QStringLiteral("statusBadge"));
        statusLabel_->setMinimumWidth(260);
        setStatus(QStringLiteral("未连接"), "idle");

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
        employeeIdEdit_->setPlaceholderText(QStringLiteral("例如 1001"));
        employeeNameEdit_->setPlaceholderText(QStringLiteral("可留空"));
        form->addRow(QStringLiteral("工号"), employeeIdEdit_);
        form->addRow(QStringLiteral("姓名"), employeeNameEdit_);

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* registerButton =
            actionButton(QStringLiteral("注册/更新"), QStyle::SP_DialogApplyButton,
                         formBox, "primary");
        QPushButton* faceRegisterButton =
            actionButton(QStringLiteral("录入人脸并注册"), QStyle::SP_FileDialogContentsView,
                         formBox, "accent");
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
        markIdEdit_->setPlaceholderText(QStringLiteral("例如 1001"));
        markNameEdit_->setPlaceholderText(QStringLiteral("可留空"));
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
            actionButton(QStringLiteral("手动打卡"), QStyle::SP_DialogApplyButton,
                         formBox, "primary");
        QPushButton* faceMarkButton =
            actionButton(QStringLiteral("刷脸打卡"), QStyle::SP_ComputerIcon,
                         formBox, "accent");
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
            actionButton(QStringLiteral("识别人脸"), QStyle::SP_ComputerIcon,
                         faceBox, "accent");
        QPushButton* salaryButton =
            actionButton(QStringLiteral("刷脸查询工资"), QStyle::SP_FileDialogDetailedView,
                         faceBox, "primary");
        faceLayout->addWidget(identifyButton);
        faceLayout->addWidget(salaryButton);
        faceLayout->addStretch(1);

        QGroupBox* planBox = new QGroupBox(QStringLiteral("激励计划与删除"), page);
        QFormLayout* planForm = new QFormLayout(planBox);
        planIdEdit_ = new QLineEdit(planBox);
        planIdEdit_->setPlaceholderText(QStringLiteral("输入需要确认的员工工号"));
        planForm->addRow(QStringLiteral("目标工号"), planIdEdit_);

        QHBoxLayout* planButtons = new QHBoxLayout;
        QPushButton* hardworkButton =
            actionButton(QStringLiteral("加入激励计划"), QStyle::SP_ArrowUp,
                         planBox, "primary");
        QPushButton* normalButton =
            actionButton(QStringLiteral("退出激励计划"), QStyle::SP_ArrowDown,
                         planBox);
        QPushButton* deleteButton =
            actionButton(QStringLiteral("删除员工"), QStyle::SP_TrashIcon,
                         planBox, "danger");
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
        logEdit_->setPlaceholderText(QStringLiteral("当前暂无操作反馈"));

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
                              QWidget* parent, const char* role = "secondary") {
        QPushButton* button = new QPushButton(style()->standardIcon(icon), text, parent);
        button->setProperty("role", role);
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
        appendFeedback(title, QStringLiteral("已发送请求，正在等待服务端响应"),
                       FeedbackInfo);

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
                FeedbackKind kind = self->classifyFeedback(ok, result);
                self->appendFeedback(title, toQString(result), kind);
                self->setStatus(feedbackText(kind) + QStringLiteral("：") + title,
                                feedbackState(kind));
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
        appendFeedback(title, QStringLiteral("正在执行，需要时请在摄像头窗口完成采集"),
                       FeedbackInfo);

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

        FeedbackKind kind = classifyFeedback(ok, result);
        appendFeedback(title, toQString(result), kind);
        setStatus(feedbackText(kind) + QStringLiteral("：") + title,
                  feedbackState(kind));
        setBusy(false);
    }

    FeedbackKind classifyFeedback(bool executionOk, const std::string& result) const {
        if (!executionOk) return FeedbackError;

        QString text = toQString(result);
        if (text.contains(QStringLiteral("失败")) ||
            text.contains(QStringLiteral("错误")) ||
            text.contains(QStringLiteral("无法")) ||
            text.contains(QStringLiteral("未找到")) ||
            text.contains(QStringLiteral("不足")) ||
            text.contains(QStringLiteral("取消")) ||
            text.contains(QStringLiteral("超时"))) {
            return FeedbackError;
        }
        if (text.contains(QStringLiteral("超过")) ||
            text.contains(QStringLiteral("缺勤"))) {
            return FeedbackWarning;
        }
        return FeedbackSuccess;
    }

    void appendFeedback(const QString& title, const QString& body, FeedbackKind kind) {
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        QString html;
        html += QStringLiteral(
            "<div style='margin:0 0 10px 0; padding:10px 12px;"
            "border:1px solid %1; border-left:4px solid %2;"
            "background:%3; border-radius:6px;'>")
                    .arg(feedbackColor(kind))
                    .arg(feedbackColor(kind))
                    .arg(feedbackBackground(kind));
        html += QStringLiteral(
            "<div style='font-size:12px; color:#57606a; margin-bottom:4px;'>%1 · %2</div>")
                    .arg(stamp)
                    .arg(feedbackText(kind));
        html += QStringLiteral(
            "<div style='font-weight:700; color:#1f2937; margin-bottom:4px;'>%1</div>")
                    .arg(escapedHtml(title));
        html += QStringLiteral("<div style='color:#374151; line-height:1.45;'>%1</div>")
                    .arg(escapedHtml(body));
        html += QStringLiteral("</div>");

        logEdit_->moveCursor(QTextCursor::End);
        logEdit_->insertHtml(html + QStringLiteral("<br>"));
        logEdit_->moveCursor(QTextCursor::End);
    }

    void repolish(QWidget* widget) {
        widget->style()->unpolish(widget);
        widget->style()->polish(widget);
        widget->update();
    }

    void setStatus(const QString& text, const char* state) {
        statusLabel_->setText(text);
        statusLabel_->setProperty("state", state);
        repolish(statusLabel_);
    }

    void setBusy(bool busy) {
        busy_ = busy;
        for (std::size_t i = 0; i < actionButtons_.size(); ++i) {
            actionButtons_[i]->setEnabled(!busy);
        }
        if (busy) {
            setStatus(QStringLiteral("正在执行操作"), "busy");
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
