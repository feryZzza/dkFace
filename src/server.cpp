#include "server.hpp"

#include "attendance.hpp"

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

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

void handleClient(int clientFd) {
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(clientFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));

    char buffer[4096];
    ssize_t received = recv(clientFd, buffer, sizeof(buffer) - 1, 0);
    if (received > 0) {
        buffer[received] = '\0';
        // 网络层只负责收发字符串，具体考勤规则交给 attendance 模块处理。
        sendAll(clientFd, processRequest(buffer) + "\n");
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
        } else if (command == "list") {
            std::cout << listAttendanceRecords() << std::endl;
        } else if (command == "help") {
            std::cout
                << "服务端命令：\n"
                << "  date 2026-MM-DD  修改后续打卡日期\n"
                << "  time HH:MM       修改后续打卡时间\n"
                << "  now 2026-MM-DD HH:MM  同时修改日期和时间\n"
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
    std::cout << "服务端命令：date 2026-MM-DD，time HH:MM，now 2026-MM-DD HH:MM，list"
              << std::endl;
    std::thread(handleServerConsole).detach();

    while (true) {
        // 每个客户端连接用一个线程处理，便于多人同时打卡。
        int clientFd = accept(serverFd, NULL, NULL);
        if (clientFd >= 0) {
            std::thread(handleClient, clientFd).detach();
        }
    }
}

}  // namespace face
