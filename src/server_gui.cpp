#include "attendance.hpp"
#include "server.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDate>
#include <QDateEdit>
#include <QDateTime>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStyle>
#include <QTextEdit>
#include <QTime>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <string>

namespace {

QString toQString(const std::string& text) {
    return QString::fromUtf8(text.c_str());
}

std::string toStdString(const QString& text) {
    return text.trimmed().toUtf8().constData();
}

class ServerWindow : public QWidget {
public:
    explicit ServerWindow(QWidget* parent = NULL) : QWidget(parent) {
        setWindowTitle(QStringLiteral("人脸考勤服务端"));
        resize(1040, 760);

        QVBoxLayout* root = new QVBoxLayout(this);
        root->setContentsMargins(18, 18, 18, 18);
        root->setSpacing(12);

        root->addWidget(createServerBox());

        QHBoxLayout* content = new QHBoxLayout;
        content->addWidget(createRecordsBox(), 1);
        content->addWidget(createLogBox(), 1);
        root->addLayout(content, 1);

        refreshTimer_ = new QTimer(this);
        refreshTimer_->setInterval(3000);
        connect(refreshTimer_, &QTimer::timeout, this, [this]() {
            if (server_.isRunning()) refreshRecords();
        });
        refreshTimer_->start();

        updateServerControls();
        refreshRecords();

        setStyleSheet(
            "QWidget { font-size: 14px; }"
            "QGroupBox { font-weight: 600; border: 1px solid #d0d7de;"
            " border-radius: 6px; margin-top: 10px; padding-top: 10px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 4px; }"
            "QSpinBox, QDateEdit, QTimeEdit { min-height: 30px; padding: 2px 6px; }"
            "QPushButton { min-height: 32px; padding: 4px 12px; border-radius: 4px; }"
            "QTextEdit { border: 1px solid #d0d7de; border-radius: 6px; }");
    }

protected:
    void closeEvent(QCloseEvent* event) {
        server_.stop();
        QWidget::closeEvent(event);
    }

private:
    QWidget* createServerBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("服务控制"), this);
        QVBoxLayout* outer = new QVBoxLayout(box);

        QHBoxLayout* firstRow = new QHBoxLayout;
        portSpin_ = new QSpinBox(box);
        portSpin_->setRange(1, 65535);
        portSpin_->setValue(face::DEFAULT_PORT);
        statusLabel_ = new QLabel(QStringLiteral("服务未启动"), box);

        startButton_ =
            new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay),
                            QStringLiteral("启动服务"), box);
        stopButton_ =
            new QPushButton(style()->standardIcon(QStyle::SP_MediaStop),
                            QStringLiteral("停止服务"), box);

        firstRow->addWidget(new QLabel(QStringLiteral("监听端口"), box));
        firstRow->addWidget(portSpin_);
        firstRow->addWidget(startButton_);
        firstRow->addWidget(stopButton_);
        firstRow->addWidget(statusLabel_, 1);

        QGroupBox* overrideBox = new QGroupBox(QStringLiteral("打卡日期与时间"), box);
        QFormLayout* overrideForm = new QFormLayout(overrideBox);
        dateCheck_ = new QCheckBox(QStringLiteral("指定日期"), overrideBox);
        dateEdit_ = new QDateEdit(defaultDate(), overrideBox);
        dateEdit_->setCalendarPopup(true);
        dateEdit_->setDisplayFormat(QStringLiteral("yyyy-MM-dd"));
        dateEdit_->setMinimumDate(QDate(2026, 1, 1));
        dateEdit_->setMaximumDate(QDate(2026, 12, 31));
        dateEdit_->setEnabled(false);
        connect(dateCheck_, &QCheckBox::toggled, dateEdit_, &QDateEdit::setEnabled);

        timeCheck_ = new QCheckBox(QStringLiteral("指定时间"), overrideBox);
        timeEdit_ = new QTimeEdit(QTime::currentTime(), overrideBox);
        timeEdit_->setDisplayFormat(QStringLiteral("HH:mm"));
        timeEdit_->setEnabled(false);
        connect(timeCheck_, &QCheckBox::toggled, timeEdit_, &QTimeEdit::setEnabled);

        QHBoxLayout* dateRow = new QHBoxLayout;
        dateRow->addWidget(dateCheck_);
        dateRow->addWidget(dateEdit_);
        dateRow->addStretch(1);

        QHBoxLayout* timeRow = new QHBoxLayout;
        timeRow->addWidget(timeCheck_);
        timeRow->addWidget(timeEdit_);
        timeRow->addStretch(1);

        applyOverrideButton_ =
            new QPushButton(style()->standardIcon(QStyle::SP_DialogApplyButton),
                            QStringLiteral("应用设置"), overrideBox);
        overrideForm->addRow(QStringLiteral("日期"), dateRow);
        overrideForm->addRow(QStringLiteral("时间"), timeRow);
        overrideForm->addRow(applyOverrideButton_);

        connect(startButton_, &QPushButton::clicked, this, [this]() { startServer(); });
        connect(stopButton_, &QPushButton::clicked, this, [this]() { stopServer(); });
        connect(applyOverrideButton_, &QPushButton::clicked, this,
                [this]() { applyAttendanceOverrides(); });

        outer->addLayout(firstRow);
        outer->addWidget(overrideBox);
        return box;
    }

    QWidget* createRecordsBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("考勤记录"), this);
        QVBoxLayout* layout = new QVBoxLayout(box);
        recordsEdit_ = new QTextEdit(box);
        recordsEdit_->setReadOnly(true);
        recordsEdit_->setLineWrapMode(QTextEdit::NoWrap);

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* refreshButton =
            new QPushButton(style()->standardIcon(QStyle::SP_BrowserReload),
                            QStringLiteral("刷新记录"), box);
        buttons->addStretch(1);
        buttons->addWidget(refreshButton);
        connect(refreshButton, &QPushButton::clicked, this, [this]() { refreshRecords(); });

        layout->addWidget(recordsEdit_);
        layout->addLayout(buttons);
        return box;
    }

    QWidget* createLogBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("服务日志"), this);
        QVBoxLayout* layout = new QVBoxLayout(box);
        logEdit_ = new QTextEdit(box);
        logEdit_->setReadOnly(true);
        logEdit_->setLineWrapMode(QTextEdit::WidgetWidth);

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* clearButton =
            new QPushButton(style()->standardIcon(QStyle::SP_DialogResetButton),
                            QStringLiteral("清空日志"), box);
        buttons->addStretch(1);
        buttons->addWidget(clearButton);
        connect(clearButton, &QPushButton::clicked, logEdit_, &QTextEdit::clear);

        layout->addWidget(logEdit_);
        layout->addLayout(buttons);
        return box;
    }

    QDate defaultDate() const {
        QDate today = QDate::currentDate();
        if (today.year() == 2026) return today;
        return QDate(2026, 5, 12);
    }

    std::string selectedDateOverride() const {
        if (!dateCheck_->isChecked()) return "";
        return toStdString(dateEdit_->date().toString(QStringLiteral("yyyy-MM-dd")));
    }

    std::string selectedTimeOverride() const {
        if (!timeCheck_->isChecked()) return "";
        return timeEdit_->time().toString(QStringLiteral("HH:mm")).toStdString();
    }

    face::AttendanceTcpServer::LogCallback makeLogCallback() {
        QPointer<ServerWindow> self(this);
        QCoreApplication* app = QCoreApplication::instance();
        return [self, app](const std::string& message) {
            if (!app) return;
            const QString text = toQString(message);
            QMetaObject::invokeMethod(app, [self, text]() {
                if (!self) return;
                self->appendLog(text);
            }, Qt::QueuedConnection);
        };
    }

    void startServer() {
        std::string message;
        if (!server_.start(portSpin_->value(), selectedDateOverride(),
                           selectedTimeOverride(), message, makeLogCallback())) {
            appendLog(QStringLiteral("启动失败：") + toQString(message));
            QMessageBox::warning(this, QStringLiteral("启动失败"), toQString(message));
            return;
        }

        statusLabel_->setText(QStringLiteral("服务运行中，端口 %1").arg(portSpin_->value()));
        updateServerControls();
        refreshRecords();
    }

    void stopServer() {
        server_.stop();
        statusLabel_->setText(QStringLiteral("服务已停止"));
        updateServerControls();
    }

    void applyAttendanceOverrides() {
        std::string dateMessage;
        std::string timeMessage;
        bool dateOk = face::setAttendanceDateOverride(selectedDateOverride(), dateMessage);
        bool timeOk = face::setAttendanceTimeOverride(selectedTimeOverride(), timeMessage);

        appendLog(toQString(dateMessage));
        appendLog(toQString(timeMessage));
        if (!dateOk || !timeOk) {
            QMessageBox::warning(this, QStringLiteral("设置失败"),
                                 toQString(dateOk ? timeMessage : dateMessage));
        }
    }

    void refreshRecords() {
        recordsEdit_->setPlainText(toQString(face::listAttendanceRecords()));
    }

    void appendLog(const QString& text) {
        const QString stamp =
            QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss"));
        logEdit_->append(stamp + QStringLiteral("  ") + text);
    }

    void updateServerControls() {
        const bool running = server_.isRunning();
        startButton_->setEnabled(!running);
        stopButton_->setEnabled(running);
        portSpin_->setEnabled(!running);
    }

    face::AttendanceTcpServer server_;
    QTimer* refreshTimer_;
    QSpinBox* portSpin_;
    QLabel* statusLabel_;
    QPushButton* startButton_;
    QPushButton* stopButton_;
    QPushButton* applyOverrideButton_;
    QCheckBox* dateCheck_;
    QDateEdit* dateEdit_;
    QCheckBox* timeCheck_;
    QTimeEdit* timeEdit_;
    QTextEdit* recordsEdit_;
    QTextEdit* logEdit_;
};

}  // namespace

int main(int argc, char* argv[]) {
    QApplication app(argc, argv);
    ServerWindow window;
    window.show();
    return app.exec();
}
