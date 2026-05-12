#include "../include/server.hpp"

#include "../include/attendance.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <iostream>
#include <sstream>
#include <string>
#include <thread>

namespace face {
namespace {

void sendAll(int socketFd, const std::string& data) {
    const char* current = data.c_str();
    std::size_t left = data.size();
    while (left > 0) {
        ssize_t sent = send(socketFd, current, left, 0);
        if (sent <= 0) return;
        current += sent;
        left -= static_cast<std::size_t>(sent);
    }
}

std::string oneLine(const std::string& value) {
    std::string result = value;
    for (std::size_t i = 0; i < result.size(); ++i) {
        if (result[i] == '\r' || result[i] == '\n') result[i] = ' ';
    }
    return result;
}

void writeLog(const AttendanceTcpServer::LogCallback& logCallback,
              const std::string& message) {
    if (logCallback) logCallback(message);
}

void handleClient(int clientFd, const std::string& peer = "",
                  AttendanceTcpServer::LogCallback logCallback =
                      AttendanceTcpServer::LogCallback()) {
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buffer[4096];
    ssize_t received = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (received > 0) {
        buffer[received] = '\0';
        // 网络层只负责收发字符串，具体考勤规则交给 attendance 模块处理。
        std::string request(buffer);
        std::string response = processRequest(request);
        if (!peer.empty()) {
            writeLog(logCallback, "客户端 " + peer + " 请求: " + oneLine(request));
            writeLog(logCallback, "客户端 " + peer + " 响应: " + oneLine(response));
        }
        sendAll(clientFd, response + "\n");
    }
    close(clientFd);
}

void handleServerConsole() {
    std::string line;
    while (std::getline(std::cin, line)) {
        std::istringstream input(line);
        std::string command;
        std::string dateText;
        std::string timeText;
        input >> command >> dateText >> timeText;
        if (command.empty()) continue;

        if (command == "date") {
            std::string message;
            setAttendanceDateOverride(dateText, message);
            std::cout << message << std::endl;
        } else if (command == "time") {
            std::string message;
            setAttendanceTimeOverride(dateText, message);
            std::cout << message << std::endl;
        } else if (command == "now") {
            std::string message;
            setAttendanceDateTimeOverride(dateText, timeText, message);
            std::cout << message << std::endl;
        } else if (command == "query") {
            std::cout << queryAttendanceRecord(dateText) << std::endl;
        } else if (command == "list") {
            std::cout << listAttendanceRecords() << std::endl;
        } else if (command == "help") {
            std::cout
                << "服务端命令：\n"
                << "  date 2026-MM-DD  修改后续打卡日期\n"
                << "  time HH:MM       修改后续打卡时间\n"
                << "  now 2026-MM-DD HH:MM  同时修改日期和时间\n"
                << "  query 工号       查询指定员工情况\n"
                << "  list             查看全部考勤记录\n"
                << "  date             清除日期覆盖\n"
                << "  time             清除时间覆盖"
                << std::endl;
        } else {
            std::cout << "未知服务端命令，输入 help 查看可用命令" << std::endl;
        }
    }
}

}  // namespace

AttendanceTcpServer::AttendanceTcpServer()
    : serverFd_(-1), running_(false), port_(0) {}

AttendanceTcpServer::~AttendanceTcpServer() {
    stop();
}

bool AttendanceTcpServer::start(int port, const std::string& dateOverride,
                                const std::string& timeOverride,
                                std::string& message,
                                LogCallback logCallback) {
    if (running_.load()) {
        message = "考勤服务器已经在运行";
        return false;
    }

    std::string dateMessage;
    if (!setAttendanceDateOverride(dateOverride, dateMessage)) {
        message = dateMessage;
        return false;
    }
    std::string timeMessage;
    if (!setAttendanceTimeOverride(timeOverride, timeMessage)) {
        message = timeMessage;
        return false;
    }

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        message = "创建 socket 失败";
        return false;
    }

    int reuse = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        close(serverFd);
        message = "绑定端口失败，可能端口已被占用";
        return false;
    }
    if (listen(serverFd, 16) != 0) {
        close(serverFd);
        message = "监听失败";
        return false;
    }

    serverFd_.store(serverFd);
    port_ = port;
    logCallback_ = logCallback;
    running_.store(true);
    worker_ = std::thread(&AttendanceTcpServer::acceptLoop, this);

    std::ostringstream output;
    output << "考勤服务器已启动，端口 " << port
           << "，数据文件 kaoqin.csv，" << dateMessage
           << "，" << timeMessage;
    message = output.str();
    log(message);
    return true;
}

void AttendanceTcpServer::stop() {
    bool wasRunning = running_.exchange(false);

    int serverFd = serverFd_.exchange(-1);
    if (serverFd >= 0) {
        shutdown(serverFd, SHUT_RDWR);
        close(serverFd);
    }

    if (worker_.joinable() && worker_.get_id() != std::this_thread::get_id()) {
        worker_.join();
    }
    if (wasRunning) log("考勤服务器已停止");
}

bool AttendanceTcpServer::isRunning() const {
    return running_.load();
}

int AttendanceTcpServer::port() const {
    return port_;
}

void AttendanceTcpServer::acceptLoop() {
    while (running_.load()) {
        sockaddr_in clientAddress;
        socklen_t clientLength = sizeof(clientAddress);
        int serverFd = serverFd_.load();
        int clientFd = accept(serverFd, reinterpret_cast<sockaddr*>(&clientAddress),
                              &clientLength);
        if (clientFd >= 0) {
            char ipText[INET_ADDRSTRLEN] = {0};
            inet_ntop(AF_INET, &clientAddress.sin_addr, ipText, sizeof(ipText));
            std::ostringstream peer;
            peer << (ipText[0] ? ipText : "unknown") << ':'
                 << ntohs(clientAddress.sin_port);

            LogCallback callback = logCallback_;
            writeLog(callback, "客户端 " + peer.str() + " 已连接");
            std::thread(handleClient, clientFd, peer.str(), callback).detach();
            continue;
        }

        if (!running_.load()) break;
        if (errno == EINTR) continue;

        log("接收客户端连接失败，监听线程退出");
        break;
    }
    running_.store(false);
}

void AttendanceTcpServer::log(const std::string& message) const {
    writeLog(logCallback_, message);
}

int runServer(int port, const std::string& dateOverride,
              const std::string& timeOverride) {
    std::string dateMessage;
    if (!setAttendanceDateOverride(dateOverride, dateMessage)) {
        std::cerr << dateMessage << std::endl;
        return 1;
    }
    std::string timeMessage;
    if (!setAttendanceTimeOverride(timeOverride, timeMessage)) {
        std::cerr << timeMessage << std::endl;
        return 1;
    }

    int serverFd = socket(AF_INET, SOCK_STREAM, 0);
    if (serverFd < 0) {
        std::cerr << "创建 socket 失败" << std::endl;
        return 1;
    }

    int reuse = 1;
    setsockopt(serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in address;
    std::memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(static_cast<uint16_t>(port));

    if (bind(serverFd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0) {
        std::cerr << "绑定端口失败，可能端口已被占用" << std::endl;
        close(serverFd);
        return 1;
    }
    if (listen(serverFd, 16) != 0) {
        std::cerr << "监听失败" << std::endl;
        close(serverFd);
        return 1;
    }

    std::cout << "考勤服务器已启动，端口 " << port
              << "，数据文件 kaoqin.csv，" << dateMessage
              << "，" << timeMessage << std::endl;
    std::cout << "服务端命令：date 2026-MM-DD，time HH:MM，now 2026-MM-DD HH:MM，query 工号，list"
              << std::endl;
    std::thread(handleServerConsole).detach();

    while (true) {
        // 每个客户端连接用一个线程处理，便于多人同时打卡。
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd >= 0) {
            std::thread([clientFd]() { handleClient(clientFd); }).detach();
        }
    }
}

}  // namespace face
