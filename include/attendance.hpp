#ifndef FACE_ATTENDANCE_HPP
#define FACE_ATTENDANCE_HPP

#include <string>

namespace face {

const int DEFAULT_PORT = 9102;

bool setAttendanceDateOverride(const std::string& dateText, std::string& message);
bool setAttendanceTimeOverride(const std::string& timeText, std::string& message);
bool setAttendanceDateTimeOverride(const std::string& dateText,
                                   const std::string& timeText,
                                   std::string& message);
std::string listAttendanceRecords();
std::string queryAttendanceRecord(const std::string& employeeId);
std::string processRequest(const std::string& line);

}  // namespace face

#endif
