#include "include/attendance.hpp"
#include "include/client.hpp"
#include "include/server.hpp"

#include <cstdlib>
#include <iostream>
#include <sstream>
#include <string>

namespace {

int toIntOrDefault(const std::string& value, int fallback) {
    std::istringstream input(value);
    int result = fallback;
    input >> result;
    return input.fail() ? fallback : result;
}

std::string joinArgs(int argc, char* argv[], int first) {
    std::ostringstream joined;
    for (int i = first; i < argc; ++i) {
        if (i > first) joined << ' ';
        joined << argv[i];
    }
    return joined.str();
}

bool looksLikeDateArg(const std::string& value) {
    return value.size() == 10 && value[4] == '-' && value[7] == '-';
}

bool looksLikeTimeArg(const std::string& value) {
    return value.size() == 5 && value[2] == ':';
}

void printUsage(const char* program) {
    std::cout
        << "用法：\n"
        << "  " << program << " server [端口] [2026-MM-DD] [HH:MM]\n"
        << "  " << program << " client [服务器IP] [端口]\n"
        << "  " << program << " client <命令> [参数...]              # 默认 127.0.0.1:"
        << face::DEFAULT_PORT << "\n"
        << "  " << program << " client <服务器IP> <端口> <命令> [参数...]\n\n"
        << "示例：\n"
        << "  " << program << " server 9102 2026-05-12 08:50\n"
        << "  " << program << " client register 1001 张三\n"
        << "  " << program << " client face_register 1001 张三\n"
        << "  " << program << " client face_mark 08:50\n"
        << "  " << program << " client mark 1001 张三 08:50\n";
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc < 2) {
        printUsage(argv[0]);
        return 0;
    }

    std::string mode = argv[1];
    // server 模式负责监听端口，client 模式负责把命令打包成协议消息发送出去。
    if (mode == "server") {
        int port = face::DEFAULT_PORT;
        std::string dateOverride;
        std::string timeOverride;
        if (argc >= 3) {
            if (looksLikeDateArg(argv[2])) {
                dateOverride = argv[2];
                if (argc >= 4) timeOverride = argv[3];
            } else if (looksLikeTimeArg(argv[2])) {
                timeOverride = argv[2];
            } else {
                port = toIntOrDefault(argv[2], face::DEFAULT_PORT);
                if (argc >= 4) dateOverride = argv[3];
                if (argc >= 5) timeOverride = argv[4];
            }
        }
        return face::runServer(port, dateOverride, timeOverride);
    }

    if (mode == "client") {
        std::string host = "127.0.0.1";
        int port = face::DEFAULT_PORT;

        // 支持简写：./main client mark 1001 张三 08:50
        if (argc >= 3 && face::isClientCommandName(argv[2])) {
            return face::runClientOnce(host, port, joinArgs(argc, argv, 2));
        }

        if (argc >= 3) host = argv[2];
        if (argc >= 4) port = toIntOrDefault(argv[3], face::DEFAULT_PORT);
        if (argc >= 5) {
            return face::runClientOnce(host, port, joinArgs(argc, argv, 4));
        }

        return face::runClient(host, port);
    }

    printUsage(argv[0]);
    return 1;
}
