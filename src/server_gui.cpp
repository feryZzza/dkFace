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
#include <QLineEdit>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QSpinBox>
#include <QStringList>
#include <QStyle>
#include <QTextCursor>
#include <QTextEdit>
#include <QTime>
#include <QTimeEdit>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#include <algorithm>
#include <string>

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
            return QStringLiteral("事件");
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

FeedbackKind classifyServerMessage(const QString& text) {
    if (text.contains(QStringLiteral("失败")) ||
        text.contains(QStringLiteral("错误")) ||
        text.contains(QStringLiteral("无法"))) {
        return FeedbackError;
    }
    if (text.contains(QStringLiteral("停止")) ||
        text.contains(QStringLiteral("缺勤")) ||
        text.contains(QStringLiteral("超过"))) {
        return FeedbackWarning;
    }
    if (text.contains(QStringLiteral("启动")) ||
        text.contains(QStringLiteral("成功")) ||
        text.contains(QStringLiteral("已设置"))) {
        return FeedbackSuccess;
    }
    return FeedbackInfo;
}

QStringList splitTopLevel(const QString& text, QChar delimiter) {
    QStringList parts;
    QString current;
    int depth = 0;
    for (int i = 0; i < text.size(); ++i) {
        const QChar ch = text.at(i);
        if (ch == QChar('(') || ch == QChar(0xff08)) ++depth;
        if (ch == QChar(')') || ch == QChar(0xff09)) depth = std::max(0, depth - 1);

        if (ch == delimiter && depth == 0) {
            parts << current.trimmed();
            current.clear();
        } else {
            current.append(ch);
        }
    }
    if (!current.trimmed().isEmpty()) parts << current.trimmed();
    return parts;
}

QStringList attendanceFields(const QString& line) {
    QStringList fields;
    QStringList sections = splitTopLevel(line, QChar(0xff1b));
    for (int i = 0; i < sections.size(); ++i) {
        QStringList parts = splitTopLevel(sections.at(i), QChar(0xff0c));
        for (int j = 0; j < parts.size(); ++j) {
            QString part = parts.at(j).trimmed();
            if (!part.isEmpty()) fields << part;
        }
    }
    return fields;
}

QString fieldValue(const QStringList& fields, const QString& prefix) {
    for (int i = 0; i < fields.size(); ++i) {
        if (fields.at(i).startsWith(prefix)) {
            return fields.at(i).mid(prefix.size()).trimmed();
        }
    }
    return QString();
}

QString fieldRowHtml(const QString& field) {
    int separator = field.indexOf(QChar(':'));
    if (separator < 0) separator = field.indexOf(QChar(0xff1a));
    if (separator < 0) {
        return QStringLiteral(
                   "<tr><td colspan='2' style='padding:9px 10px; color:#374151;"
                   "border-top:1px solid #e5e7eb;'>%1</td></tr>")
            .arg(escapedHtml(field));
    }

    QString label = field.left(separator).trimmed();
    QString value = field.mid(separator + 1).trimmed();
    return QStringLiteral(
               "<tr>"
               "<td style='width:128px; padding:9px 10px; color:#57606a;"
               "font-weight:600; background:#f8fafc; border-top:1px solid #e5e7eb;'>%1</td>"
               "<td style='padding:9px 10px; color:#1f2937;"
               "background:#ffffff; border-top:1px solid #e5e7eb;'>%2</td>"
               "</tr>")
        .arg(escapedHtml(label))
        .arg(escapedHtml(value));
}

QString attendanceCardHtml(const QString& line, int index) {
    QStringList fields = attendanceFields(line);
    QString employeeId = fieldValue(fields, QStringLiteral("工号:"));
    QString employeeName = fieldValue(fields, QStringLiteral("姓名:"));
    QString plan = fieldValue(fields, QStringLiteral("计划:"));
    QString salary = fieldValue(fields, QStringLiteral("本月工资:"));
    QString salaryPrefix;

    for (int i = 0; i < fields.size(); ++i) {
        if (fields.at(i).startsWith(QStringLiteral("本月工资额"))) {
            salaryPrefix = fields.at(i);
            break;
        }
    }
    if (salary.isEmpty() && !salaryPrefix.isEmpty()) {
        int pos = salaryPrefix.indexOf(QChar('='));
        if (pos >= 0) salary = salaryPrefix.mid(pos + 1).trimmed();
    }

    QString title = QStringLiteral("员工记录 %1").arg(index + 1);
    if (!employeeId.isEmpty()) title = QStringLiteral("工号 %1").arg(employeeId);
    if (!employeeName.isEmpty()) title += QStringLiteral(" · %1").arg(employeeName);

    QString rows;
    for (int i = 0; i < fields.size(); ++i) {
        const QString field = fields.at(i);
        if (field.startsWith(QStringLiteral("工号:")) ||
            field.startsWith(QStringLiteral("姓名:")) ||
            field.startsWith(QStringLiteral("本月工资额"))) {
            continue;
        }
        rows += fieldRowHtml(field);
    }
    if (rows.isEmpty()) rows = fieldRowHtml(line);

    QString badges;
    if (!plan.isEmpty()) {
        badges += QStringLiteral(
                      "<span style='display:inline-block; margin-left:8px; padding:3px 8px;"
                      "border-radius:10px; background:#ddf4ff; color:#0969da;"
                      "font-size:12px; font-weight:600;'>%1</span>")
                      .arg(escapedHtml(plan));
    }
    if (!salary.isEmpty()) {
        badges += QStringLiteral(
                      "<span style='display:inline-block; margin-left:8px; padding:3px 8px;"
                      "border-radius:10px; background:#ecfdf3; color:#1f7a4d;"
                      "font-size:12px; font-weight:600;'>工资 %1</span>")
                      .arg(escapedHtml(salary));
    }

    return QStringLiteral(
               "<table cellspacing='0' cellpadding='0' style='width:100%; margin:0 0 14px 0;"
               "border-collapse:collapse; border:2px solid #bfd7ff; background:#ffffff;'>"
               "<tr><td style='padding:12px 14px; background:#f0f6ff;"
               "border-bottom:1px solid #bfd7ff; border-left:5px solid #0969da;'>"
               "<span style='font-size:17px; font-weight:700; color:#111827;'>%1</span>%2"
               "</td></tr>"
               "<tr><td style='padding:0;'>"
               "<table cellspacing='0' cellpadding='0' style='width:100%;"
               "border-collapse:collapse; background:#ffffff;'>%3</table>"
               "</td></tr>"
               "</table>")
        .arg(escapedHtml(title))
        .arg(badges)
        .arg(rows);
}

QString attendanceStateHtml(const QString& text, FeedbackKind kind) {
    return QStringLiteral(
               "<table cellspacing='0' cellpadding='0' style='width:100%;"
               "border-collapse:collapse; border:2px solid %1; background:%3;'>"
               "<tr><td style='padding:14px 16px; border-left:5px solid %2;"
               "color:#374151; line-height:1.5;'>%4</td></tr></table>")
        .arg(feedbackColor(kind))
        .arg(feedbackColor(kind))
        .arg(feedbackBackground(kind))
        .arg(escapedHtml(text));
}

QString attendanceRecordsHtml(const QString& text) {
    QString value = text.trimmed();
    if (value.isEmpty()) {
        return attendanceStateHtml(QStringLiteral("当前没有员工考勤记录"), FeedbackInfo);
    }

    FeedbackKind state = classifyServerMessage(value);
    if (value.contains(QStringLiteral("当前没有")) ||
        value.contains(QStringLiteral("失败")) ||
        value.contains(QStringLiteral("未找到"))) {
        return attendanceStateHtml(value, state);
    }

    QString html;
    QStringList lines = value.split(QChar('\n'), Qt::SkipEmptyParts);
    for (int i = 0; i < lines.size(); ++i) {
        html += attendanceCardHtml(lines.at(i).trimmed(), i);
    }
    return html;
}

class ServerWindow : public QWidget {
public:
    explicit ServerWindow(QWidget* parent = NULL)
        : QWidget(parent), showingQueryResult_(false) {
        setWindowTitle(QStringLiteral("人脸考勤服务端"));
        resize(1600, 1000);

        QVBoxLayout* root = new QVBoxLayout(this);
        root->setContentsMargins(22, 20, 22, 20);
        root->setSpacing(14);

        QLabel* title = new QLabel(QStringLiteral("人脸考勤服务端"), this);
        title->setObjectName(QStringLiteral("pageTitle"));
        QLabel* subtitle =
            new QLabel(QStringLiteral("监听客户端请求、维护考勤记录和统一打卡时间"), this);
        subtitle->setObjectName(QStringLiteral("pageSubtitle"));
        root->addWidget(title);
        root->addWidget(subtitle);

        root->addWidget(createServerBox());

        QHBoxLayout* content = new QHBoxLayout;
        content->addWidget(createRecordsBox(), 1);
        content->addWidget(createLogBox(), 1);
        root->addLayout(content, 1);

        refreshTimer_ = new QTimer(this);
        refreshTimer_->setInterval(3000);
        connect(refreshTimer_, &QTimer::timeout, this, [this]() {
            if (server_.isRunning() && !showingQueryResult_) refreshRecords();
        });
        refreshTimer_->start();

        updateServerControls();
        refreshRecords();

        setStyleSheet(
            "QWidget { background: #f6f8fb; color: #1f2937; font-size: 14px; }"
            "QLabel#pageTitle { font-size: 24px; font-weight: 700; color: #111827; }"
            "QLabel#pageSubtitle { color: #6b7280; padding-bottom: 2px; }"
            "QLabel#sectionTitle { color: #374151; font-weight: 700; padding-top: 4px; }"
            "QGroupBox { background: #ffffff; font-weight: 600; border: 1px solid #d0d7de;"
            " border-radius: 8px; margin-top: 12px; padding: 14px; }"
            "QGroupBox::title { subcontrol-origin: margin; left: 12px; padding: 0 5px;"
            " color: #374151; background: #f6f8fb; }"
            "QLineEdit, QSpinBox, QDateEdit, QTimeEdit { background: #ffffff; min-height: 32px;"
            " padding: 3px 8px; border: 1px solid #c9d1d9; border-radius: 5px; }"
            "QLineEdit:focus, QSpinBox:focus, QDateEdit:focus, QTimeEdit:focus { border: 1px solid #0969da; }"
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
            "QLabel#statusBadge[state=\"ok\"] { color: #1f7a4d; background: #ecfdf3;"
            " border-color: #8ee0ad; }"
            "QLabel#statusBadge[state=\"warning\"] { color: #9a6700; background: #fff8c5;"
            " border-color: #eac54f; }"
            "QLabel#statusBadge[state=\"error\"] { color: #b42318; background: #ffebe9;"
            " border-color: #ffaba8; }"
            "QTextEdit { background: #ffffff; border: 1px solid #d0d7de;"
            " border-radius: 8px; padding: 8px; }");
    }

protected:
    void closeEvent(QCloseEvent* event) {
        server_.stop();
        QWidget::closeEvent(event);
    }

private:
    QPushButton* actionButton(const QString& text, QStyle::StandardPixmap icon,
                              QWidget* parent, const char* role = "secondary") {
        QPushButton* button = new QPushButton(style()->standardIcon(icon), text, parent);
        button->setProperty("role", role);
        return button;
    }

    QWidget* createServerBox() {
        QGroupBox* box = new QGroupBox(QStringLiteral("服务控制"), this);
        QVBoxLayout* outer = new QVBoxLayout(box);

        QHBoxLayout* firstRow = new QHBoxLayout;
        portSpin_ = new QSpinBox(box);
        portSpin_->setRange(1, 65535);
        portSpin_->setValue(face::DEFAULT_PORT);
        statusLabel_ = new QLabel(QStringLiteral("服务未启动"), box);
        statusLabel_->setObjectName(QStringLiteral("statusBadge"));
        statusLabel_->setMinimumWidth(230);
        setStatus(QStringLiteral("服务未启动"), "idle");

        startButton_ = actionButton(QStringLiteral("启动服务"), QStyle::SP_MediaPlay,
                                    box, "primary");
        stopButton_ = actionButton(QStringLiteral("停止服务"), QStyle::SP_MediaStop,
                                   box, "danger");

        firstRow->addWidget(new QLabel(QStringLiteral("监听端口"), box));
        firstRow->addWidget(portSpin_);
        firstRow->addWidget(startButton_);
        firstRow->addWidget(stopButton_);
        firstRow->addWidget(statusLabel_, 1);

        QWidget* overrideBox = new QWidget(box);
        QVBoxLayout* overrideLayout = new QVBoxLayout(overrideBox);
        overrideLayout->setContentsMargins(0, 8, 0, 0);
        QLabel* overrideTitle = new QLabel(QStringLiteral("打卡日期与时间"), overrideBox);
        overrideTitle->setObjectName(QStringLiteral("sectionTitle"));
        QFormLayout* overrideForm = new QFormLayout;
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

        applyOverrideButton_ = actionButton(QStringLiteral("应用设置"),
                                            QStyle::SP_DialogApplyButton,
                                            overrideBox, "accent");
        overrideForm->addRow(QStringLiteral("日期"), dateRow);
        overrideForm->addRow(QStringLiteral("时间"), timeRow);
        overrideForm->addRow(applyOverrideButton_);
        overrideLayout->addWidget(overrideTitle);
        overrideLayout->addLayout(overrideForm);

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

        QHBoxLayout* queryRow = new QHBoxLayout;
        queryIdEdit_ = new QLineEdit(box);
        queryIdEdit_->setPlaceholderText(QStringLiteral("输入员工工号"));
        QPushButton* queryButton = actionButton(QStringLiteral("查询个人情况"),
                                                QStyle::SP_FileDialogDetailedView,
                                                box, "accent");
        queryRow->addWidget(new QLabel(QStringLiteral("工号"), box));
        queryRow->addWidget(queryIdEdit_, 1);
        queryRow->addWidget(queryButton);

        recordModeLabel_ = new QLabel(QStringLiteral("当前显示：全部员工考勤记录"), box);
        recordModeLabel_->setObjectName(QStringLiteral("sectionTitle"));

        recordsEdit_ = new QTextEdit(box);
        recordsEdit_->setReadOnly(true);
        recordsEdit_->setLineWrapMode(QTextEdit::WidgetWidth);
        recordsEdit_->setPlaceholderText(QStringLiteral("暂无考勤记录"));
        recordsEdit_->setStyleSheet(
            "QTextEdit { background: #f8fafc; border: 1px solid #bfd7ff;"
            "border-radius: 8px; padding: 10px; }");

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* refreshButton = actionButton(QStringLiteral("刷新记录"),
                                                  QStyle::SP_BrowserReload, box);
        buttons->addStretch(1);
        buttons->addWidget(refreshButton);
        connect(queryButton, &QPushButton::clicked, this, [this]() { queryRecord(); });
        connect(refreshButton, &QPushButton::clicked, this, [this]() { refreshRecords(); });

        layout->addLayout(queryRow);
        layout->addWidget(recordModeLabel_);
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
        logEdit_->setPlaceholderText(QStringLiteral("当前暂无服务事件"));

        QHBoxLayout* buttons = new QHBoxLayout;
        QPushButton* clearButton = actionButton(QStringLiteral("清空日志"),
                                                QStyle::SP_DialogResetButton, box);
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
                self->appendLog(text, classifyServerMessage(text));
            }, Qt::QueuedConnection);
        };
    }

    void startServer() {
        std::string message;
        if (!server_.start(portSpin_->value(), selectedDateOverride(),
                           selectedTimeOverride(), message, makeLogCallback())) {
            appendLog(QStringLiteral("启动失败：") + toQString(message), FeedbackError);
            setStatus(QStringLiteral("启动失败"), "error");
            QMessageBox::warning(this, QStringLiteral("启动失败"), toQString(message));
            return;
        }

        setStatus(QStringLiteral("服务运行中，端口 %1").arg(portSpin_->value()), "ok");
        updateServerControls();
        refreshRecords();
    }

    void stopServer() {
        server_.stop();
        setStatus(QStringLiteral("服务已停止"), "warning");
        updateServerControls();
    }

    void applyAttendanceOverrides() {
        std::string dateMessage;
        std::string timeMessage;
        bool dateOk = face::setAttendanceDateOverride(selectedDateOverride(), dateMessage);
        bool timeOk = face::setAttendanceTimeOverride(selectedTimeOverride(), timeMessage);

        appendLog(toQString(dateMessage), dateOk ? FeedbackSuccess : FeedbackError);
        appendLog(toQString(timeMessage), timeOk ? FeedbackSuccess : FeedbackError);
        if (!dateOk || !timeOk) {
            QMessageBox::warning(this, QStringLiteral("设置失败"),
                                 toQString(dateOk ? timeMessage : dateMessage));
        }
    }

    void refreshRecords() {
        showingQueryResult_ = false;
        recordModeLabel_->setText(QStringLiteral("当前显示：全部员工考勤记录"));
        recordsEdit_->setHtml(attendanceRecordsHtml(toQString(face::listAttendanceRecords())));
    }

    void queryRecord() {
        QString idText = queryIdEdit_->text().trimmed();
        if (idText.isEmpty()) {
            appendLog(QStringLiteral("查询失败：工号不能为空"), FeedbackError);
            queryIdEdit_->setFocus();
            return;
        }

        QString result = toQString(face::queryAttendanceRecord(toStdString(idText)));
        showingQueryResult_ = true;
        recordModeLabel_->setText(QStringLiteral("当前显示：工号 %1 的查询结果").arg(idText));
        recordsEdit_->setHtml(attendanceRecordsHtml(result));
        appendLog(QStringLiteral("查询工号 %1\n%2").arg(idText).arg(result),
                  classifyServerMessage(result));
    }

    void appendLog(const QString& text, FeedbackKind kind = FeedbackInfo) {
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
        html += QStringLiteral("<div style='color:#374151; line-height:1.45;'>%1</div>")
                    .arg(escapedHtml(text));
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

    void updateServerControls() {
        const bool running = server_.isRunning();
        startButton_->setEnabled(!running);
        stopButton_->setEnabled(running);
        portSpin_->setEnabled(!running);
    }

    face::AttendanceTcpServer server_;
    QTimer* refreshTimer_;
    bool showingQueryResult_;
    QSpinBox* portSpin_;
    QLabel* statusLabel_;
    QPushButton* startButton_;
    QPushButton* stopButton_;
    QPushButton* applyOverrideButton_;
    QCheckBox* dateCheck_;
    QDateEdit* dateEdit_;
    QCheckBox* timeCheck_;
    QTimeEdit* timeEdit_;
    QLineEdit* queryIdEdit_;
    QLabel* recordModeLabel_;
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
