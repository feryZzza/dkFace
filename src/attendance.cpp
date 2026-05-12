#include "attendance.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <ctime>
#include <fstream>
#include <iomanip>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace face {
namespace {

const int MAX_ABSENCES = 5;
const char* DATA_FILE = "kaoqin.csv";
const char* PHOTO_DIR = "photos";

// CSV 中保存的员工状态。普通计划和激励计划的奖惩分开累计：
// 普通计划固定按勤奋+500、缺勤-300结算；激励计划固定按+500/-1000结算。
struct Employee {
    std::string id;
    std::string name;
    int normalAbsenceCount;
    int normalRemedyCount;
    int normalDiligentCount;
    int hardworkAbsenceCount;
    int hardworkRemedyCount;
    int hardworkDiligentCount;
    int baseSalary;
    std::string plan;
    std::string lastDate;
    std::string lastTime;

    Employee()
        : normalAbsenceCount(0),
          normalRemedyCount(0),
          normalDiligentCount(0),
          hardworkAbsenceCount(0),
          hardworkRemedyCount(0),
          hardworkDiligentCount(0),
          baseSalary(5000),
          plan("NORMAL") {}
};

struct Policy {
    int deadlineMinute;
    int remedyBeforeMinute;
    int diligentBeforeMinute;
    int baseSalary;
    int diligentReward;
    int absencePenalty;
};

struct Request {
    std::string type;
    std::string id;
    std::string name;
    std::string timeText;
};

std::mutex g_dataMutex;
std::string g_dateOverride;
std::string g_timeOverride;

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
    if (!value.empty() && value[value.size() - 1] == delimiter) {
        parts.push_back("");
    }
    return parts;
}

int toInt(const std::string& value, int fallback) {
    std::istringstream input(value);
    int result = fallback;
    input >> result;
    return input.fail() ? fallback : result;
}

bool parseMinuteOfDay(const std::string& text, int& minute) {
    std::vector<std::string> parts = split(trim(text), ':');
    if (parts.size() != 2) return false;

    int hour = toInt(parts[0], -1);
    int minutes = toInt(parts[1], -1);
    if (hour < 0 || hour > 23 || minutes < 0 || minutes > 59) return false;

    minute = hour * 60 + minutes;
    return true;
}

bool parseAttendanceDate(const std::string& text) {
    std::string value = trim(text);
    if (value.size() != 10 || value[4] != '-' || value[7] != '-') return false;
    for (std::size_t i = 0; i < value.size(); ++i) {
        if (i == 4 || i == 7) continue;
        if (value[i] < '0' || value[i] > '9') return false;
    }

    int year = toInt(value.substr(0, 4), -1);
    int month = toInt(value.substr(5, 2), -1);
    int day = toInt(value.substr(8, 2), -1);
    if (year != 2026 || month < 1 || month > 12) return false;

    const int daysInMonth[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    return day >= 1 && day <= daysInMonth[month - 1];
}

std::string currentDate() {
    std::time_t now = std::time(NULL);
    std::tm localTime;
    localtime_r(&now, &localTime);

    std::ostringstream output;
    output << std::setfill('0') << std::setw(4) << (localTime.tm_year + 1900)
           << '-' << std::setw(2) << (localTime.tm_mon + 1)
           << '-' << std::setw(2) << localTime.tm_mday;
    return output.str();
}

std::string currentTimeText() {
    std::time_t now = std::time(NULL);
    std::tm localTime;
    localtime_r(&now, &localTime);

    std::ostringstream output;
    output << std::setfill('0') << std::setw(2) << localTime.tm_hour
           << ':' << std::setw(2) << localTime.tm_min;
    return output.str();
}

Request parseRequest(const std::string& line) {
    std::vector<std::string> fields = split(trim(line), '|');
    Request request;
    if (fields.size() > 0) request.type = trim(fields[0]);
    if (fields.size() > 1) request.id = trim(fields[1]);
    if (fields.size() > 2) request.name = trim(fields[2]);
    if (request.type == "CLIENT_MARK" && fields.size() > 4) {
        // 打卡日期由服务端统一决定。若旧客户端仍发送日期字段，只取最后的时间字段。
        request.timeText = trim(fields[4]);
    } else if (fields.size() > 3) {
        request.timeText = trim(fields[3]);
    }
    return request;
}

Policy policyFor(const Employee& employee) {
    if (employee.plan == "HARDWORK") {
        // 激励计划员工7点前打卡，6:30前补救/勤奋，
        // 基础工资8000，勤奋奖励和缺勤惩罚均按1000计算。
        Policy policy = {7 * 60, 6 * 60 + 30, 6 * 60 + 30, 8000, 500, 1000};
        return policy;
    }

    // 普通计划员工9点前打卡，8点前补救，
    // 7:30前勤奋奖励，工资按5000 + 勤奋数*500 - 缺勤数*300计算。
    Policy policy = {9 * 60, 8 * 60, 7 * 60 + 30, 5000, 500, 300};
    return policy;
}

int totalAbsenceCount(const Employee& employee) {
    return employee.normalAbsenceCount + employee.hardworkAbsenceCount;
}

int totalRemedyCount(const Employee& employee) {
    return employee.normalRemedyCount + employee.hardworkRemedyCount;
}

int totalDiligentCount(const Employee& employee) {
    return employee.normalDiligentCount + employee.hardworkDiligentCount;
}

int& currentAbsenceCount(Employee& employee) {
    return employee.plan == "HARDWORK" ? employee.hardworkAbsenceCount
                                       : employee.normalAbsenceCount;
}

int& currentRemedyCount(Employee& employee) {
    return employee.plan == "HARDWORK" ? employee.hardworkRemedyCount
                                       : employee.normalRemedyCount;
}

int& currentDiligentCount(Employee& employee) {
    return employee.plan == "HARDWORK" ? employee.hardworkDiligentCount
                                       : employee.normalDiligentCount;
}

std::map<std::string, Employee> loadEmployees() {
    std::map<std::string, Employee> employees;
    std::ifstream input(DATA_FILE);
    std::string line;

    std::getline(input, line);  // header
    while (std::getline(input, line)) {
        std::vector<std::string> fields = split(line, ',');
        if (fields.size() < 2 || fields[0].empty()) continue;

        Employee employee;
        employee.id = fields[0];
        employee.name = fields[1];
        if (fields.size() > 5) employee.baseSalary = toInt(fields[5], 5000);
        if (fields.size() > 6 && !fields[6].empty()) employee.plan = fields[6];
        if (fields.size() > 7) employee.lastDate = fields[7];
        if (fields.size() > 8) employee.lastTime = fields[8];

        if (fields.size() > 14) {
            employee.normalAbsenceCount = toInt(fields[9], 0);
            employee.normalRemedyCount = toInt(fields[10], 0);
            employee.normalDiligentCount = toInt(fields[11], 0);
            employee.hardworkAbsenceCount = toInt(fields[12], 0);
            employee.hardworkRemedyCount = toInt(fields[13], 0);
            employee.hardworkDiligentCount = toInt(fields[14], 0);
        } else {
            // 兼容旧CSV：旧版本没有阶段明细，按普通计划金额保留，避免加入激励后重算。
            if (fields.size() > 2) employee.normalAbsenceCount = toInt(fields[2], 0);
            if (fields.size() > 3) employee.normalRemedyCount = toInt(fields[3], 0);
            if (fields.size() > 4) employee.normalDiligentCount = toInt(fields[4], 0);
        }
        employees[employee.id] = employee;
    }

    return employees;
}

void saveEmployees(const std::map<std::string, Employee>& employees) {
    std::ofstream output(DATA_FILE);
    output << "employee_id,name,absence_count,remedy_count,diligent_count,"
           << "base_salary,plan,last_date,last_time,"
           << "normal_absence_count,normal_remedy_count,normal_diligent_count,"
           << "hardwork_absence_count,hardwork_remedy_count,hardwork_diligent_count\n";

    for (std::map<std::string, Employee>::const_iterator it = employees.begin();
         it != employees.end(); ++it) {
        const Employee& e = it->second;
        output << e.id << ',' << e.name << ','
               << totalAbsenceCount(e) << ',' << totalRemedyCount(e) << ','
               << totalDiligentCount(e) << ',' << e.baseSalary << ','
               << e.plan << ',' << e.lastDate << ',' << e.lastTime << ','
               << e.normalAbsenceCount << ',' << e.normalRemedyCount << ','
               << e.normalDiligentCount << ',' << e.hardworkAbsenceCount << ','
               << e.hardworkRemedyCount << ',' << e.hardworkDiligentCount << '\n';
    }
}

Employee& findOrCreateEmployee(std::map<std::string, Employee>& employees,
                               const Request& request) {
    Employee& employee = employees[request.id];
    if (employee.id.empty()) {
        employee.id = request.id;
        employee.name = request.name.empty() ? ("employee_" + request.id) : request.name;
    } else if (!request.name.empty()) {
        employee.name = request.name;
    }
    return employee;
}

void mark_attendance(Employee& employee, const std::string& dateText,
                     const std::string& timeText) {
    // 对应任务书中的 mark_attendance()，这里只记录最近一次有效打卡时间。
    employee.lastDate = dateText;
    employee.lastTime = timeText;
}

int computeSalary(const Employee& employee) {
    // 工资分段结算：加入激励计划前的普通勤奋/缺勤永远按+500/-300，
    // 加入激励计划后的勤奋/缺勤单独按+500/-1000，不回头重算旧记录。
    return policyFor(employee).baseSalary +
           employee.normalDiligentCount * 500 -
           employee.normalAbsenceCount * 300 +
           employee.hardworkDiligentCount * 500 -
           employee.hardworkAbsenceCount * 1000;
}

std::string employeeSummary(const Employee& employee) {
    std::ostringstream output;
    int normalSettlement = employee.normalDiligentCount * 500 -
                           employee.normalAbsenceCount * 300;
    int hardworkSettlement = employee.hardworkDiligentCount * 500 -
                             employee.hardworkAbsenceCount * 1000;
    output << "工号: " << employee.id
           << "，姓名: " << employee.name
           << "，计划: " << (employee.plan == "HARDWORK" ? "激励计划" : "普通计划")
           << "，缺勤: " << totalAbsenceCount(employee)
           << "(普通" << employee.normalAbsenceCount
           << "，激励" << employee.hardworkAbsenceCount << ")"
           << "，补救: " << totalRemedyCount(employee)
           << "(普通" << employee.normalRemedyCount
           << "，激励" << employee.hardworkRemedyCount << ")"
           << "，勤奋: " << totalDiligentCount(employee)
           << "(普通" << employee.normalDiligentCount
           << "，激励" << employee.hardworkDiligentCount << ")"
           << "，基础工资: " << policyFor(employee).baseSalary
           << "，普通结算: " << normalSettlement
           << "，激励结算: " << hardworkSettlement
           << "，本月工资: " << computeSalary(employee);
    if (!employee.lastDate.empty()) {
        output << "，最近打卡: " << employee.lastDate << ' ' << employee.lastTime;
    }
    return output.str();
}

std::string handleRegister(std::map<std::string, Employee>& employees,
                           const Request& request) {
    if (request.id.empty()) return "注册失败：工号不能为空";

    Employee& employee = findOrCreateEmployee(employees, request);
    employee.baseSalary = policyFor(employee).baseSalary;
    return "员工注册/更新成功：" + employeeSummary(employee);
}

std::string handleMark(std::map<std::string, Employee>& employees,
                       const Request& request) {
    if (request.id.empty()) return "打卡失败：工号不能为空";

    std::string dateText = g_dateOverride.empty() ? currentDate() : g_dateOverride;
    std::string timeText = g_timeOverride.empty()
                               ? (request.timeText.empty() ? currentTimeText()
                                                           : request.timeText)
                               : g_timeOverride;

    if (!parseAttendanceDate(dateText)) {
        return "打卡失败：服务器打卡日期应为 2026-MM-DD，年份必须为2026";
    }

    int minute = 0;
    if (!parseMinuteOfDay(timeText, minute)) return "打卡失败：时间格式应为 HH:MM";

    Employee& employee = findOrCreateEmployee(employees, request);
    Policy policy = policyFor(employee);

    // 缺勤次数达到5次后，服务端拒绝继续打卡。
    if (totalAbsenceCount(employee) >= MAX_ABSENCES) {
        return "您本月已缺勤5次，无法继续打卡";
    }

    int& planAbsenceCount = currentAbsenceCount(employee);
    int& planRemedyCount = currentRemedyCount(employee);
    int& planDiligentCount = currentDiligentCount(employee);

    if (minute < policy.diligentBeforeMinute) {
        // 当前计划内的勤奋/缺勤只影响当前计划结算桶，不改变另一阶段的金额。
        if (planAbsenceCount > 0) --planAbsenceCount;
        ++planDiligentCount;
        mark_attendance(employee, dateText, timeText);

        std::ostringstream output;
        output << "勤奋打卡成功！目前累计缺勤次数为" << totalAbsenceCount(employee)
               << "，勤奋奖励次数为" << totalDiligentCount(employee);
        return output.str();
    }

    if (planAbsenceCount > 0 && minute < policy.remedyBeforeMinute) {
        // 补救也只补救当前计划产生的缺勤，避免激励计划补救改动普通阶段-300记录。
        --planAbsenceCount;
        ++planRemedyCount;
        mark_attendance(employee, dateText, timeText);

        std::ostringstream output;
        output << "补救缺勤成功！目前累计缺勤次数为" << totalAbsenceCount(employee);
        return output.str();
    }

    if (minute < policy.deadlineMinute) {
        // 未超过打卡截止时间时才执行正常打卡记录。
        mark_attendance(employee, dateText, timeText);
        return "打卡成功";
    }

    // 超过打卡时间时不执行正常打卡，缺勤次数加一并返回提示。
    ++planAbsenceCount;
    std::ostringstream output;
    output << "超过打卡时间，记缺勤一次，本月已经累计缺勤"
           << totalAbsenceCount(employee) << "次";
    if (totalAbsenceCount(employee) >= MAX_ABSENCES) {
        output << "；您本月已缺勤5次，无法继续打卡";
    }
    return output.str();
}

std::string handleQuery(const std::map<std::string, Employee>& employees,
                        const Request& request) {
    std::map<std::string, Employee>::const_iterator it = employees.find(request.id);
    if (request.id.empty()) return "查询失败：工号不能为空";
    if (it == employees.end()) return "查询失败：未找到该员工";

    // CLIENT_QUERY 返回员工本月工资额 salary。
    std::ostringstream output;
    output << "本月工资额 salary = " << computeSalary(it->second)
           << "；" << employeeSummary(it->second);
    return output.str();
}

std::string handleHardwork(std::map<std::string, Employee>& employees,
                           const Request& request) {
    if (request.id.empty()) return "加入激励计划失败：工号不能为空";

    Employee& employee = findOrCreateEmployee(employees, request);
    // CLIENT_HARDWORK 只改变当前打卡规则和基础工资；普通阶段已产生的
    // 勤奋+500、缺勤-300继续固定保留，后续激励阶段奖惩单独累计。
    employee.plan = "HARDWORK";
    employee.baseSalary = 8000;
    return "已加入激励计划：每日打卡时间调整为7点，基础工资调整为8000；普通阶段奖惩按+500/-300保留，激励阶段奖惩单独按1000结算";
}

std::string handleNormal(std::map<std::string, Employee>& employees,
                         const Request& request) {
    if (request.id.empty()) return "退出激励计划失败：工号不能为空";

    Employee& employee = findOrCreateEmployee(employees, request);
    // CLIENT_NORMAL 退出激励计划只恢复当前规则；激励阶段已产生的-1000/+500保留。
    employee.plan = "NORMAL";
    employee.baseSalary = 5000;
    return "已退出激励计划：打卡时间和基础工资恢复为日常标准，历史普通/激励奖惩分段保留";
}

int deleteEmployeePhotos(const std::string& employeeId) {
    DIR* dir = opendir(PHOTO_DIR);
    if (!dir) return 0;

    int removed = 0;
    for (dirent* entry = readdir(dir); entry; entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;
        if (name.find(employeeId) != 0) continue;

        std::string path = std::string(PHOTO_DIR) + "/" + name;
        struct stat info;
        if (stat(path.c_str(), &info) != 0) continue;

        if (S_ISREG(info.st_mode) && remove(path.c_str()) == 0) {
            ++removed;
        } else if (S_ISDIR(info.st_mode) && name == employeeId) {
            DIR* employeeDir = opendir(path.c_str());
            for (dirent* file = employeeDir ? readdir(employeeDir) : NULL;
                 file;
                 file = readdir(employeeDir)) {
                std::string fileName = file->d_name;
                if (fileName == "." || fileName == "..") continue;
                if (remove((path + "/" + fileName).c_str()) == 0) ++removed;
            }
            if (employeeDir) closedir(employeeDir);
            rmdir(path.c_str());
        }
    }

    closedir(dir);
    return removed;
}

std::string handleDelete(std::map<std::string, Employee>& employees,
                         const Request& request) {
    if (request.id.empty()) return "删除失败：工号不能为空";

    // CLIENT_DELETE 删除CSV中的员工记录，并删除本地人脸照片模板。
    employees.erase(request.id);
    std::ostringstream output;
    output << "个人消息已经全部删除! 已删除照片文件"
           << deleteEmployeePhotos(request.id) << "个";
    return output.str();
}

std::string handleList(const std::map<std::string, Employee>& employees) {
    if (employees.empty()) return "当前没有员工考勤记录";

    std::ostringstream output;
    for (std::map<std::string, Employee>::const_iterator it = employees.begin();
         it != employees.end(); ++it) {
        if (it != employees.begin()) output << '\n';
        output << employeeSummary(it->second);
    }
    return output.str();
}

}  // namespace

bool setAttendanceDateOverride(const std::string& dateText, std::string& message) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    std::string value = trim(dateText);
    if (value.empty()) {
        g_dateOverride.clear();
        message = "使用服务器系统日期";
        return true;
    }

    if (!parseAttendanceDate(value)) {
        message = "服务器打卡日期格式应为 2026-MM-DD，年份必须为2026";
        return false;
    }

    g_dateOverride = value;
    message = "服务器打卡日期已设置为 " + g_dateOverride;
    return true;
}

bool setAttendanceTimeOverride(const std::string& timeText, std::string& message) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    std::string value = trim(timeText);
    if (value.empty()) {
        g_timeOverride.clear();
        message = "使用客户端时间或服务器当前时间";
        return true;
    }

    int minute = 0;
    if (!parseMinuteOfDay(value, minute)) {
        message = "服务器打卡时间格式应为 HH:MM";
        return false;
    }

    g_timeOverride = value;
    message = "服务器打卡时间已设置为 " + g_timeOverride;
    return true;
}

bool setAttendanceDateTimeOverride(const std::string& dateText,
                                   const std::string& timeText,
                                   std::string& message) {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    std::string dateValue = trim(dateText);
    std::string timeValue = trim(timeText);
    if (dateValue.empty() || timeValue.empty()) {
        message = "now 命令格式：now 2026-MM-DD HH:MM";
        return false;
    }

    if (!parseAttendanceDate(dateValue)) {
        message = "服务器打卡日期格式应为 2026-MM-DD，年份必须为2026";
        return false;
    }

    int minute = 0;
    if (!parseMinuteOfDay(timeValue, minute)) {
        message = "服务器打卡时间格式应为 HH:MM";
        return false;
    }

    g_dateOverride = dateValue;
    g_timeOverride = timeValue;
    message = "服务器打卡日期时间已设置为 " + g_dateOverride + " " + g_timeOverride;
    return true;
}

std::string listAttendanceRecords() {
    std::lock_guard<std::mutex> lock(g_dataMutex);
    return handleList(loadEmployees());
}

std::string processRequest(const std::string& line) {
    Request request = parseRequest(line);
    // 服务端可能同时处理多个客户端连接，文件读写用互斥锁保护。
    std::lock_guard<std::mutex> lock(g_dataMutex);
    std::map<std::string, Employee> employees = loadEmployees();

    bool changed = false;
    std::string response;

    if (request.type == "CLIENT_REGISTER") {
        response = handleRegister(employees, request);
        changed = true;
    } else if (request.type == "CLIENT_MARK") {
        response = handleMark(employees, request);
        changed = true;
    } else if (request.type == "CLIENT_QUERY") {
        response = handleQuery(employees, request);
    } else if (request.type == "CLIENT_HARDWORK") {
        response = handleHardwork(employees, request);
        changed = true;
    } else if (request.type == "CLIENT_NORMAL") {
        response = handleNormal(employees, request);
        changed = true;
    } else if (request.type == "CLIENT_DELETE") {
        response = handleDelete(employees, request);
        changed = true;
    } else if (request.type == "CLIENT_LIST") {
        response = "查看全部考勤记录只能在服务器端执行";
    } else {
        response = "请求失败：未知消息类型 " + request.type;
    }

    // 查询和列表不会修改数据，其它消息处理后写回 kaoqin.csv。
    if (changed) saveEmployees(employees);
    return response;
}

}  // namespace face
