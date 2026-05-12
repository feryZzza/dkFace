#include "client.hpp"

#include "face_recognition.hpp"

#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace face {
namespace {

struct BuiltRequest {
    bool known;
    bool ready;
    std::string request;
};

BuiltRequest unknownCommand() {
    BuiltRequest result;
    result.known = false;
    result.ready = false;
    return result;
}

BuiltRequest failedCommand() {
    BuiltRequest result;
    result.known = true;
    result.ready = false;
    return result;
}

BuiltRequest readyCommand(const std::string& request) {
    BuiltRequest result;
    result.known = true;
    result.ready = true;
    result.request = request;
    return result;
}

std::string trim(const std::string& value) {
    std::size_t first = value.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return "";
    std::size_t last = value.find_last_not_of(" \t\r\n");
    return value.substr(first, last - first + 1);
}

std::vector<std::string> split(const std::string& value, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream input(value);
    while (std::getline(input, current, delimiter)) {
        parts.push_back(current);
    }
    return parts;
}

int toInt(const std::string& value, int fallback) {
    std::istringstream input(value);
    int result = fallback;
    input >> result;
    return input.fail() ? fallback : result;
}

bool looksLikeTime(const std::string& value) {
    std::vector<std::string> parts = split(value, ':');
    if (parts.size() != 2) return false;

    int hour = toInt(parts[0], -1);
    int minute = toInt(parts[1], -1);
    return hour >= 0 && hour <= 23 && minute >= 0 && minute <= 59;
}

std::string promptLine(const std::string& label) {
    std::cout << label;
    std::string value;
    std::getline(std::cin, value);
    return trim(value);
}

bool promptRequired(const std::string& label, std::string& value) {
    value = promptLine(label);
    if (!value.empty()) return true;

    std::cout << "输入不能为空" << std::endl;
    return false;
}

bool validOptionalTime(const std::string& timeText) {
    if (timeText.empty()) return true;
    if (looksLikeTime(timeText)) return true;

    std::cout << "时间格式错误，应为 HH:MM，例如 08:50；也可以直接回车使用当前时间"
              << std::endl;
    return false;
}

std::string markRequest(const std::string& id, const std::string& name,
                        const std::string& timeText) {
    return "CLIENT_MARK|" + id + "|" + name + "|" + timeText;
}

bool isSensitiveRequest(const std::string& request) {
    std::vector<std::string> fields = split(request, '|');
    if (fields.empty()) return false;

    std::string type = trim(fields[0]);
    return type == "CLIENT_HARDWORK" ||
           type == "CLIENT_NORMAL" ||
           type == "CLIENT_DELETE" ||
           type == "CLIENT_LIST";
}

bool confirmFaceForEmployee(const std::string& expectedId,
                            const std::string& actionName,
                            std::string& confirmedId) {
    if (expectedId.empty()) {
        std::cout << actionName << "失败：工号不能为空" << std::endl;
        return false;
    }

    std::cout << "请正对摄像头，刷脸确认" << actionName << "，按 Esc 可取消。"
              << std::endl;
    std::string message;
    double score = 0.0;
    if (!recognizeFace(confirmedId, score, message)) {
        std::cout << message << std::endl;
        return false;
    }
    std::cout << message << std::endl;

    if (confirmedId != expectedId) {
        std::cout << actionName << "失败：识别工号 " << confirmedId
                  << " 与输入工号 " << expectedId << " 不一致" << std::endl;
        return false;
    }

    return true;
}

BuiltRequest requestFromClientCommand(const std::string& commandLine) {
    std::istringstream input(commandLine);
    std::string command;
    input >> command;

    if (command == "raw") {
        std::string rest;
        std::getline(input, rest);
        rest = trim(rest);
        if (rest.empty()) return failedCommand();
        if (isSensitiveRequest(rest)) {
            std::cout << "敏感或服务端专用操作不能通过客户端 raw 发送；激励/删除请走刷脸确认，查看记录请在服务端输入 list"
                      << std::endl;
            return failedCommand();
        }
        return readyCommand(rest);
    }
    if (command == "register") {
        std::string id;
        std::string name;
        input >> id >> name;
        return readyCommand("CLIENT_REGISTER|" + id + "|" + name + "|");
    }
    if (command == "mark") {
        std::string id;
        std::string name;
        std::string timeText;
        input >> id;

        std::string value;
        while (input >> value) {
            if (looksLikeTime(value)) {
                timeText = value;
            } else if (name.empty()) {
                name = value;
            } else {
                std::cout << "打卡命令格式错误，应为 mark <工号> [姓名] [HH:MM]；打卡日期在服务端设置"
                          << std::endl;
                return failedCommand();
            }
        }

        if (!validOptionalTime(timeText)) {
            return failedCommand();
        }
        return readyCommand(markRequest(id, name, timeText));
    }
    if (command == "query") {
        std::cout << "已删除通过工号查询工资功能，请使用刷脸查询工资" << std::endl;
        return failedCommand();
    }
    if (command == "hardwork") {
        std::string id;
        input >> id;
        std::string confirmedId;
        if (!confirmFaceForEmployee(id, "加入激励计划", confirmedId)) {
            return failedCommand();
        }
        return readyCommand("CLIENT_HARDWORK|" + confirmedId + "||");
    }
    if (command == "normal") {
        std::string id;
        input >> id;
        std::string confirmedId;
        if (!confirmFaceForEmployee(id, "退出激励计划", confirmedId)) {
            return failedCommand();
        }
        return readyCommand("CLIENT_NORMAL|" + confirmedId + "||");
    }
    if (command == "delete") {
        std::string id;
        input >> id;
        std::string confirmedId;
        if (!confirmFaceForEmployee(id, "删除员工", confirmedId)) {
            return failedCommand();
        }
        return readyCommand("CLIENT_DELETE|" + confirmedId + "||");
    }
    if (command == "list") {
        std::cout << "查看全部考勤记录只能在服务器端执行，请在服务端输入 list"
                  << std::endl;
        return failedCommand();
    }
    if (command == "face_register") {
        std::string id;
        std::string name;
        input >> id >> name;

        std::string message;
        if (!enrollFace(id, message)) {
            std::cout << message << std::endl;
            return failedCommand();
        }
        std::cout << message << std::endl;
        return readyCommand("CLIENT_REGISTER|" + id + "|" + name + "|");
    }
    if (command == "face_mark") {
        std::string timeText;
        std::string value;
        while (input >> value) {
            if (looksLikeTime(value)) {
                timeText = value;
            } else {
                std::cout << "刷脸打卡命令格式错误，应为 face_mark [HH:MM]；打卡日期在服务端设置"
                          << std::endl;
                return failedCommand();
            }
        }
        if (!validOptionalTime(timeText)) {
            return failedCommand();
        }

        std::string id;
        std::string message;
        double score = 0.0;
        if (!recognizeFace(id, score, message)) {
            std::cout << message << std::endl;
            return failedCommand();
        }
        std::cout << message << std::endl;
        return readyCommand(markRequest(id, "", timeText));
    }
    if (command == "face_query") {
        std::string id;
        std::string message;
        double score = 0.0;
        if (!recognizeFace(id, score, message)) {
            std::cout << message << std::endl;
            return failedCommand();
        }
        std::cout << message << std::endl;
        return readyCommand("CLIENT_QUERY|" + id + "||");
    }

    return unknownCommand();
}

void printMenu() {
    std::cout
        << "\n========== 人脸考勤系统 ==========\n"
        << "1. 员工注册\n"
        << "2. 手动打卡\n"
        << "3. 录入人脸\n"
        << "4. 刷脸打卡（9:00起缺勤，8:00前补救，7:30前勤奋）\n"
        << "5. 刷脸查询工资\n"
        << "6. 加入激励计划（需刷脸确认）\n"
        << "7. 退出激励计划（需刷脸确认）\n"
        << "8. 删除员工（需刷脸确认）\n"
        << "0. 退出\n"
        << "==================================\n";
}

BuiltRequest requestFromMenuChoice(const std::string& choice) {
    std::string id;
    std::string name;
    std::string timeText;

    if (choice == "1") {
        if (!promptRequired("工号: ", id)) return failedCommand();
        name = promptLine("姓名(可空): ");
        return readyCommand("CLIENT_REGISTER|" + id + "|" + name + "|");
    }

    if (choice == "2") {
        if (!promptRequired("工号: ", id)) return failedCommand();
        name = promptLine("姓名(可空): ");
        timeText = promptLine("打卡时间 HH:MM(可空，默认当前时间；日期由服务端设置): ");
        if (!validOptionalTime(timeText)) return failedCommand();
        return readyCommand(markRequest(id, name, timeText));
    }

    if (choice == "3") {
        if (!promptRequired("工号: ", id)) return failedCommand();
        name = promptLine("姓名(可空): ");

        std::string message;
        std::cout << "请正对摄像头，检测成功后会自动保存人脸模板，按 Esc 可取消。"
                  << std::endl;
        if (!enrollFace(id, message)) {
            std::cout << message << std::endl;
            return failedCommand();
        }
        std::cout << message << std::endl;
        return readyCommand("CLIENT_REGISTER|" + id + "|" + name + "|");
    }

    if (choice == "4") {
        timeText = promptLine("打卡时间 HH:MM(可空，默认当前时间；日期由服务端设置): ");
        if (!validOptionalTime(timeText)) return failedCommand();

        std::string message;
        double score = 0.0;
        std::cout << "请正对摄像头，识别成功后会自动打卡，按 Esc 可取消。"
                  << std::endl;
        if (!recognizeFace(id, score, message)) {
            std::cout << message << std::endl;
            return failedCommand();
        }
        std::cout << message << std::endl;
        return readyCommand(markRequest(id, "", timeText));
    }

    if (choice == "5") {
        std::string message;
        double score = 0.0;
        std::cout << "请正对摄像头，识别成功后会自动查询工资，按 Esc 可取消。"
                  << std::endl;
        if (!recognizeFace(id, score, message)) {
            std::cout << message << std::endl;
            return failedCommand();
        }
        std::cout << message << std::endl;
        return readyCommand("CLIENT_QUERY|" + id + "||");
    }

    if (choice == "6") {
        if (!promptRequired("工号: ", id)) return failedCommand();
        std::string confirmedId;
        if (!confirmFaceForEmployee(id, "加入激励计划", confirmedId)) {
            return failedCommand();
        }
        return readyCommand("CLIENT_HARDWORK|" + confirmedId + "||");
    }

    if (choice == "7") {
        if (!promptRequired("工号: ", id)) return failedCommand();
        std::string confirmedId;
        if (!confirmFaceForEmployee(id, "退出激励计划", confirmedId)) {
            return failedCommand();
        }
        return readyCommand("CLIENT_NORMAL|" + confirmedId + "||");
    }

    if (choice == "8") {
        if (!promptRequired("工号: ", id)) return failedCommand();
        std::string confirm = promptLine("确认删除该员工全部信息？输入 y 确认: ");
        if (confirm != "y" && confirm != "Y") {
            std::cout << "已取消删除" << std::endl;
            return failedCommand();
        }
        std::string confirmedId;
        if (!confirmFaceForEmployee(id, "删除员工", confirmedId)) {
            return failedCommand();
        }
        return readyCommand("CLIENT_DELETE|" + confirmedId + "||");
    }

    return unknownCommand();
}

bool sendAll(int socketFd, const std::string& data) {
    const char* current = data.c_str();
    std::size_t left = data.size();
    while (left > 0) {
        ssize_t sent = send(socketFd, current, left, 0);
        if (sent <= 0) return false;
        current += sent;
        left -= static_cast<std::size_t>(sent);
    }
    return true;
}

void setSocketTimeout(int socketFd) {
    timeval timeout;
    timeout.tv_sec = 5;
    timeout.tv_usec = 0;
    setsockopt(socketFd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(socketFd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

std::string sendRequestToServer(const std::string& host, int port,
                                const std::string& request) {
    int socketFd = socket(AF_INET, SOCK_STREAM, 0);
    if (socketFd < 0) throw std::runtime_error("创建 socket 失败");
    setSocketTimeout(socketFd);

    sockaddr_in serverAddress;
    std::memset(&serverAddress, 0, sizeof(serverAddress));
    serverAddress.sin_family = AF_INET;
    serverAddress.sin_port = htons(static_cast<uint16_t>(port));

    if (inet_pton(AF_INET, host.c_str(), &serverAddress.sin_addr) != 1) {
        close(socketFd);
        throw std::runtime_error("服务器地址格式错误");
    }
    if (connect(socketFd, reinterpret_cast<sockaddr*>(&serverAddress),
                sizeof(serverAddress)) != 0) {
        close(socketFd);
        throw std::runtime_error("连接服务器失败");
    }

    // 每个客户端命令对应一条以换行结尾的简单文本协议消息。
    if (!sendAll(socketFd, request + "\n")) {
        close(socketFd);
        throw std::runtime_error("发送请求失败");
    }
    shutdown(socketFd, SHUT_WR);

    std::string response;
    char buffer[1024];
    ssize_t received = 0;
    while ((received = recv(socketFd, buffer, sizeof(buffer), 0)) > 0) {
        response.append(buffer, buffer + received);
    }
    close(socketFd);
    if (response.empty()) throw std::runtime_error("等待服务器响应超时或服务器无响应");
    return trim(response);
}

void printClientHelp() {
    std::cout
        << "命令：\n"
        << "  register <工号> [姓名]\n"
        << "  mark <工号> [姓名] [HH:MM]     # 日期由服务端设置\n"
        << "  face_register <工号> [姓名]\n"
        << "  face_mark [HH:MM]              # 日期由服务端设置\n"
        << "  face_query\n"
        << "  hardwork <工号>     # 需刷脸确认\n"
        << "  normal <工号>       # 需刷脸确认\n"
        << "  delete <工号>       # 需刷脸确认\n"
        << "  raw <CLIENT_...|...>\n"
        << "  quit\n";
}

}  // namespace

bool isClientCommandName(const std::string& value) {
    return value == "raw" ||
           value == "register" ||
           value == "mark" ||
           value == "face_register" ||
           value == "face_mark" ||
           value == "face_query" ||
           value == "query" ||
           value == "hardwork" ||
           value == "normal" ||
           value == "delete" ||
           value == "list";
}

int runClient(const std::string& host, int port) {
    std::cout << "连接考勤服务器 " << host << ':' << port << std::endl;

    std::string line;
    while (true) {
        printMenu();
        std::cout << "请选择功能编号: ";
        if (!std::getline(std::cin, line)) break;
        line = trim(line);
        if (line == "0" || line == "quit" || line == "exit") break;
        if (line == "help") {
            printMenu();
            continue;
        }
        if (line.empty()) continue;

        BuiltRequest built = requestFromMenuChoice(line);
        if (!built.known) {
            built = requestFromClientCommand(line);
        }
        if (!built.known) {
            std::cout << "无效选择，请输入菜单编号" << std::endl;
            continue;
        }
        if (!built.ready) continue;

        try {
            std::cout << sendRequestToServer(host, port, built.request) << std::endl;
        } catch (const std::exception& error) {
            std::cout << error.what() << std::endl;
        }
    }
    return 0;
}

int runClientOnce(const std::string& host, int port, const std::string& commandLine) {
    BuiltRequest built = requestFromClientCommand(commandLine);
    if (!built.known) {
        std::cerr << "未知客户端命令: " << commandLine << std::endl;
        return 1;
    }
    if (!built.ready) return 1;

    try {
        std::cout << sendRequestToServer(host, port, built.request) << std::endl;
        return 0;
    } catch (const std::exception& error) {
        std::cerr << error.what() << std::endl;
        return 1;
    }
}

}  // namespace face
