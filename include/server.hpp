#ifndef FACE_SERVER_HPP
#define FACE_SERVER_HPP

#include <atomic>
#include <functional>
#include <string>
#include <thread>

namespace face {

class AttendanceTcpServer {
public:
    typedef std::function<void(const std::string&)> LogCallback;

    AttendanceTcpServer();
    ~AttendanceTcpServer();

    AttendanceTcpServer(const AttendanceTcpServer&) = delete;
    AttendanceTcpServer& operator=(const AttendanceTcpServer&) = delete;

    bool start(int port, const std::string& dateOverride,
               const std::string& timeOverride, std::string& message,
               LogCallback logCallback = LogCallback());
    void stop();
    bool isRunning() const;
    int port() const;

private:
    void acceptLoop();
    void log(const std::string& message) const;

    std::atomic<int> serverFd_;
    std::atomic<bool> running_;
    int port_;
    std::thread worker_;
    LogCallback logCallback_;
};

int runServer(int port, const std::string& dateOverride = "",
              const std::string& timeOverride = "");

}  // namespace face

#endif
