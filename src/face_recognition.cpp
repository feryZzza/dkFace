#include "face_recognition.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <climits>
#include <cstdlib>
#include <cstring>
#include <map>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/face.hpp>
#include <opencv2/opencv.hpp>

namespace face {
namespace {

const char* DATA_DIR_NAME = "data";
const char* PHOTO_DIR_NAME = "photos";
const char* CASCADE_PATH = "/usr/share/opencv4/haarcascades/haarcascade_frontalface_default.xml";
const int FACE_SIZE = 120;
const int STABLE_FRAME_COUNT = 8;
const int SAMPLE_INTERVAL_FRAMES = 6;
const int ENROLL_SAMPLE_COUNT = 5;
const int RECOGNIZE_SAMPLE_COUNT = 3;
const int MIN_MATCH_VOTES = 2;
const double MATCH_THRESHOLD = 80.0;

struct MatchVote {
    int count;
    double scoreSum;
    double bestScore;

    MatchVote() : count(0), scoreSum(0.0), bestScore(std::numeric_limits<double>::max()) {}
};

const char* BASE64_CHARS =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
    "abcdefghijklmnopqrstuvwxyz"
    "0123456789+/";

bool hasDisplay() {
    return std::getenv("DISPLAY") != NULL || std::getenv("WAYLAND_DISPLAY") != NULL;
}

bool pathExists(const std::string& path) {
    struct stat info;
    return stat(path.c_str(), &info) == 0;
}

bool isDirectory(const std::string& path) {
    struct stat info;
    if (stat(path.c_str(), &info) != 0) return false;
    return S_ISDIR(info.st_mode);
}

std::string parentDir(const std::string& path) {
    if (path.empty()) return "";
    std::size_t pos = path.find_last_of('/');
    if (pos == std::string::npos) return "";
    if (pos == 0) return "/";
    return path.substr(0, pos);
}

std::string joinPath(const std::string& left, const std::string& right) {
    if (left.empty()) return right;
    if (left[left.size() - 1] == '/') return left + right;
    return left + "/" + right;
}

std::string getWorkingDir() {
    char buffer[PATH_MAX] = {0};
    if (!getcwd(buffer, sizeof(buffer))) return "";
    return std::string(buffer);
}

std::string getExecutableDir() {
    char path[PATH_MAX] = {0};
    ssize_t len = readlink("/proc/self/exe", path, sizeof(path) - 1);
    if (len <= 0) return "";
    path[len] = '\0';
    return parentDir(std::string(path));
}

std::string findProjectRootFrom(const std::string& startDir) {
    std::string current = startDir;
    for (int depth = 0; depth < 10 && !current.empty(); ++depth) {
        if (pathExists(joinPath(current, "CMakeLists.txt")) &&
            isDirectory(joinPath(current, "src"))) {
            return current;
        }
        std::string next = parentDir(current);
        if (next == current) break;
        current = next;
    }
    return "";
}

std::string resolveProjectRoot() {
    std::string fromCwd = findProjectRootFrom(getWorkingDir());
    if (!fromCwd.empty()) return fromCwd;

    std::string fromExe = findProjectRootFrom(getExecutableDir());
    if (!fromExe.empty()) return fromExe;

    return getWorkingDir();
}

std::string dataDirPath() {
    return joinPath(resolveProjectRoot(), DATA_DIR_NAME);
}

std::string photoDirPath() {
    return joinPath(dataDirPath(), PHOTO_DIR_NAME);
}

void ensurePhotoDir() {
    std::string dataDir = dataDirPath();
    std::string photoDir = photoDirPath();
    mkdir(dataDir.c_str(), 0755);
    mkdir(photoDir.c_str(), 0755);
}

std::string base64Encode(const std::vector<unsigned char>& data) {
    std::string encoded;
    encoded.reserve(((data.size() + 2) / 3) * 4);

    unsigned int value = 0;
    int valb = -6;
    for (std::size_t i = 0; i < data.size(); ++i) {
        value = (value << 8) + data[i];
        valb += 8;
        while (valb >= 0) {
            encoded.push_back(BASE64_CHARS[(value >> valb) & 0x3F]);
            valb -= 6;
        }
    }
    if (valb > -6) {
        encoded.push_back(BASE64_CHARS[((value << 8) >> (valb + 8)) & 0x3F]);
    }
    while (encoded.size() % 4 != 0) encoded.push_back('=');
    return encoded;
}

bool base64Decode(const std::string& text, std::vector<unsigned char>& data) {
    int reverseTable[256];
    std::memset(reverseTable, -1, sizeof(reverseTable));
    for (int i = 0; i < 64; ++i) {
        reverseTable[static_cast<unsigned char>(BASE64_CHARS[i])] = i;
    }

    data.clear();
    unsigned int value = 0;
    int valb = -8;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const unsigned char ch = static_cast<unsigned char>(text[i]);
        if (ch == '=') break;
        if (ch == '\r' || ch == '\n' || ch == ' ' || ch == '\t') continue;
        if (reverseTable[ch] < 0) return false;

        value = (value << 6) + static_cast<unsigned int>(reverseTable[ch]);
        valb += 6;
        if (valb >= 0) {
            data.push_back(static_cast<unsigned char>((value >> valb) & 0xFF));
            valb -= 8;
        }
    }
    return true;
}

bool ensureDir(const std::string& path, std::string& message) {
    struct stat info;
    if (stat(path.c_str(), &info) == 0) {
        if (S_ISDIR(info.st_mode)) return true;
        message = "录入失败：路径已存在但不是目录: " + path;
        return false;
    }

    if (mkdir(path.c_str(), 0755) == 0) return true;
    message = "录入失败：无法创建目录: " + path;
    return false;
}

void closeCameraWindow(cv::VideoCapture& camera, bool showWindow) {
    camera.release();
    if (showWindow) {
        cv::destroyAllWindows();
        cv::waitKey(1);
    }
}

std::string templatePath(const std::string& employeeId) {
    return joinPath(photoDirPath(), employeeId + ".png");
}

std::string sampleDir(const std::string& employeeId) {
    return joinPath(photoDirPath(), employeeId);
}

std::string samplePath(const std::string& employeeId, int index) {
    std::ostringstream output;
    output << sampleDir(employeeId) << "/sample_" << index << ".png";
    return output.str();
}

std::vector<std::string> split(const std::string& text, char delimiter) {
    std::vector<std::string> parts;
    std::string current;
    std::istringstream input(text);
    while (std::getline(input, current, delimiter)) {
        parts.push_back(current);
    }
    if (!text.empty() && text[text.size() - 1] == delimiter) {
        parts.push_back("");
    }
    return parts;
}

std::string fileStem(const std::string& name) {
    std::size_t pos = name.find_last_of('.');
    return pos == std::string::npos ? name : name.substr(0, pos);
}

bool isPngFile(const std::string& name) {
    return name.size() > 4 && name.substr(name.size() - 4) == ".png";
}

bool loadFaceCascade(cv::CascadeClassifier& cascade, std::string& message) {
    if (cascade.load(CASCADE_PATH)) return true;
    message = "加载人脸检测模型失败: " + std::string(CASCADE_PATH);
    return false;
}

bool findLargestFace(const cv::Mat& frame, cv::CascadeClassifier& cascade, cv::Rect& face) {
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);
    cv::equalizeHist(gray, gray);

    std::vector<cv::Rect> faces;
    cascade.detectMultiScale(gray, faces, 1.1, 3, 0, cv::Size(80, 80));
    if (faces.empty()) return false;

    face = faces[0];
    for (std::size_t i = 1; i < faces.size(); ++i) {
        if (faces[i].area() > face.area()) face = faces[i];
    }
    return true;
}

cv::Mat normalizeFace(const cv::Mat& frame, const cv::Rect& faceRect) {
    cv::Mat gray;
    cv::cvtColor(frame, gray, cv::COLOR_BGR2GRAY);

    cv::Mat face = gray(faceRect).clone();
    cv::resize(face, face, cv::Size(FACE_SIZE, FACE_SIZE));
    cv::equalizeHist(face, face);
    cv::GaussianBlur(face, face, cv::Size(3, 3), 0);
    return face;
}

bool captureFaceSamples(std::vector<cv::Mat>& samples, int sampleCount, std::string& message) {
    cv::CascadeClassifier cascade;
    if (!loadFaceCascade(cascade, message)) return false;

    cv::VideoCapture camera;
    if (!camera.open(0, cv::CAP_V4L2)) {
        camera.open(0);
    }
    if (!camera.isOpened()) {
        message = "无法打开摄像头，请检查摄像头权限或设备连接";
        return false;
    }
    camera.set(cv::CAP_PROP_BUFFERSIZE, 1);
    camera.set(cv::CAP_PROP_FRAME_WIDTH, 640);
    camera.set(cv::CAP_PROP_FRAME_HEIGHT, 480);

    bool showWindow = hasDisplay();
    if (showWindow) {
        cv::namedWindow("face attendance", cv::WINDOW_AUTOSIZE);
        cv::startWindowThread();
    }

    for (int i = 0; i < 10; ++i) {
        cv::Mat warmupFrame;
        camera >> warmupFrame;
        if (showWindow) cv::waitKey(20);
    }

    int stableCount = 0;
    int framesSinceSample = SAMPLE_INTERVAL_FRAMES;

    for (int i = 0; i < 240 && static_cast<int>(samples.size()) < sampleCount; ++i) {
        cv::Mat frame;
        camera >> frame;
        if (frame.empty()) continue;

        cv::Rect faceRect;
        if (findLargestFace(frame, cascade, faceRect)) {
            ++stableCount;
            ++framesSinceSample;
            cv::Mat faceTemplate = normalizeFace(frame, faceRect);
            if (stableCount >= STABLE_FRAME_COUNT &&
                framesSinceSample >= SAMPLE_INTERVAL_FRAMES) {
                samples.push_back(faceTemplate);
                framesSinceSample = 0;
            }

            if (showWindow) {
                cv::rectangle(frame, faceRect, cv::Scalar(0, 255, 0), 2);
                std::ostringstream text;
                text << "Samples " << samples.size() << "/" << sampleCount;
                cv::putText(frame, text.str(), cv::Point(20, 35),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 255, 0), 2);
            }
        } else {
            stableCount = 0;
            framesSinceSample = SAMPLE_INTERVAL_FRAMES;
            if (showWindow) {
                cv::putText(frame, "Look at camera", cv::Point(20, 35),
                            cv::FONT_HERSHEY_SIMPLEX, 0.8, cv::Scalar(0, 0, 255), 2);
            }
        }

        if (showWindow) {
            cv::imshow("face attendance", frame);
            if (cv::waitKey(30) == 27) {
                message = "已取消人脸采集";
                closeCameraWindow(camera, showWindow);
                return false;
            }
        }
    }

    closeCameraWindow(camera, showWindow);
    if (samples.empty()) {
        message = "未检测到人脸，请调整光线并正对摄像头";
        return false;
    }
    if (static_cast<int>(samples.size()) < sampleCount) {
        std::ostringstream output;
        output << "人脸采集不足：需要 " << sampleCount << " 张，实际采集 "
               << samples.size() << " 张，请保持正对摄像头";
        message = output.str();
        return false;
    }
    return true;
}

cv::Mat loadTemplateImage(const std::string& path) {
    cv::Mat image = cv::imread(path, cv::IMREAD_GRAYSCALE);
    if (image.empty()) return image;
    if (image.size() != cv::Size(FACE_SIZE, FACE_SIZE)) {
        cv::resize(image, image, cv::Size(FACE_SIZE, FACE_SIZE));
    }
    return image;
}

int labelForEmployee(const std::string& employeeId, std::vector<std::string>& employeeIds) {
    for (std::size_t i = 0; i < employeeIds.size(); ++i) {
        if (employeeIds[i] == employeeId) return static_cast<int>(i);
    }

    employeeIds.push_back(employeeId);
    return static_cast<int>(employeeIds.size() - 1);
}

void addTemplateImage(const std::string& employeeId, const std::string& path,
                      std::vector<cv::Mat>& images, std::vector<int>& labels,
                      std::vector<std::string>& employeeIds) {
    cv::Mat image = loadTemplateImage(path);
    if (image.empty()) return;

    int label = labelForEmployee(employeeId, employeeIds);
    images.push_back(image);
    labels.push_back(label);
}

void loadSampleDirectory(const std::string& employeeId, const std::string& path,
                         std::vector<cv::Mat>& images, std::vector<int>& labels,
                         std::vector<std::string>& employeeIds) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;

    for (dirent* entry = readdir(dir); entry; entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (!isPngFile(name)) continue;

        addTemplateImage(employeeId, path + "/" + name, images, labels, employeeIds);
    }
    closedir(dir);
}

void loadTrainingSet(std::vector<cv::Mat>& images, std::vector<int>& labels,
                     std::vector<std::string>& employeeIds) {
    const std::string photoDir = photoDirPath();
    DIR* dir = opendir(photoDir.c_str());
    if (!dir) return;

    for (dirent* entry = readdir(dir); entry; entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string path = joinPath(photoDir, name);
        struct stat info;
        if (stat(path.c_str(), &info) != 0) continue;

        if (S_ISREG(info.st_mode) && isPngFile(name)) {
            addTemplateImage(fileStem(name), path, images, labels, employeeIds);
        } else if (S_ISDIR(info.st_mode)) {
            loadSampleDirectory(name, path, images, labels, employeeIds);
        }
    }
    closedir(dir);
}

}  // namespace

std::string faceDataRootDir() {
    return dataDirPath();
}

std::string faceStorageRootDir() {
    return photoDirPath();
}

int enrollFaceSampleCount() {
    return ENROLL_SAMPLE_COUNT;
}

int recognizeFaceSampleCount() {
    return RECOGNIZE_SAMPLE_COUNT;
}

bool captureFaceSamplesForNetwork(int sampleCount, std::vector<cv::Mat>& samples,
                                  std::string& message) {
    samples.clear();
    if (sampleCount <= 0) {
        message = "采集失败：样本数量必须大于0";
        return false;
    }
    return captureFaceSamples(samples, sampleCount, message);
}

bool encodeFaceSamplesForNetwork(const std::vector<cv::Mat>& samples,
                                 std::string& payload, std::string& message) {
    if (samples.empty()) {
        message = "编码失败：人脸样本为空";
        return false;
    }

    std::ostringstream output;
    output << samples.size();
    for (std::size_t i = 0; i < samples.size(); ++i) {
        std::vector<unsigned char> bytes;
        if (!cv::imencode(".png", samples[i], bytes)) {
            message = "编码失败：无法压缩人脸样本";
            return false;
        }
        output << ';' << base64Encode(bytes);
    }

    payload = output.str();
    message = "编码成功";
    return true;
}

bool decodeFaceSamplesFromNetwork(const std::string& payload,
                                  std::vector<cv::Mat>& samples,
                                  std::string& message) {
    std::vector<std::string> parts = split(payload, ';');
    if (parts.empty() || parts[0].empty()) {
        message = "解码失败：负载为空";
        return false;
    }

    std::istringstream countInput(parts[0]);
    int expected = 0;
    countInput >> expected;
    if (countInput.fail() || expected <= 0) {
        message = "解码失败：样本数量无效";
        return false;
    }
    if (static_cast<int>(parts.size()) != expected + 1) {
        message = "解码失败：样本数量与负载不一致";
        return false;
    }

    samples.clear();
    for (int i = 0; i < expected; ++i) {
        std::vector<unsigned char> bytes;
        if (!base64Decode(parts[i + 1], bytes)) {
            message = "解码失败：Base64 数据损坏";
            return false;
        }

        cv::Mat raw(1, static_cast<int>(bytes.size()), CV_8UC1,
                    bytes.empty() ? NULL : &bytes[0]);
        cv::Mat decoded = cv::imdecode(raw, cv::IMREAD_GRAYSCALE);
        if (decoded.empty()) {
            message = "解码失败：无法解析人脸图像";
            return false;
        }
        if (decoded.size() != cv::Size(FACE_SIZE, FACE_SIZE)) {
            cv::resize(decoded, decoded, cv::Size(FACE_SIZE, FACE_SIZE));
        }
        samples.push_back(decoded);
    }

    message = "解码成功";
    return true;
}

bool loadFaceCascadeForCapture(cv::CascadeClassifier& cascade, std::string& message) {
    return loadFaceCascade(cascade, message);
}

bool findLargestFaceForCapture(const cv::Mat& frame, cv::CascadeClassifier& cascade,
                               cv::Rect& face) {
    return findLargestFace(frame, cascade, face);
}

cv::Mat normalizeFaceForCapture(const cv::Mat& frame, const cv::Rect& faceRect) {
    return normalizeFace(frame, faceRect);
}

bool saveFaceEnrollmentSamples(const std::string& employeeId,
                               const std::vector<cv::Mat>& faceSamples,
                               std::string& message) {
    if (employeeId.empty()) {
        message = "录入失败：工号不能为空";
        return false;
    }
    if (static_cast<int>(faceSamples.size()) < ENROLL_SAMPLE_COUNT) {
        std::ostringstream output;
        output << "人脸采集不足：需要 " << ENROLL_SAMPLE_COUNT << " 张，实际采集 "
               << faceSamples.size() << " 张，请保持正对摄像头";
        message = output.str();
        return false;
    }

    ensurePhotoDir();
    if (!ensureDir(sampleDir(employeeId), message)) return false;

    if (!cv::imwrite(templatePath(employeeId), faceSamples[0])) {
        message = "录入失败：无法保存人脸模板";
        return false;
    }

    for (std::size_t i = 1; i < faceSamples.size(); ++i) {
        if (!cv::imwrite(samplePath(employeeId, static_cast<int>(i)), faceSamples[i])) {
            message = "录入失败：无法保存多帧人脸样本";
            return false;
        }
    }

    std::ostringstream output;
    output << "人脸录入成功，模板文件: " << templatePath(employeeId)
           << "，样本数: " << faceSamples.size();
    message = output.str();
    return true;
}

bool recognizeFaceSamples(const std::vector<cv::Mat>& currentFaces,
                          std::string& employeeId, double& score,
                          std::string& message) {
    if (static_cast<int>(currentFaces.size()) < RECOGNIZE_SAMPLE_COUNT) {
        std::ostringstream output;
        output << "人脸采集不足：需要 " << RECOGNIZE_SAMPLE_COUNT << " 张，实际采集 "
               << currentFaces.size() << " 张，请保持正对摄像头";
        message = output.str();
        return false;
    }

    std::vector<cv::Mat> trainingImages;
    std::vector<int> labels;
    std::vector<std::string> employeeIds;
    loadTrainingSet(trainingImages, labels, employeeIds);
    if (trainingImages.empty()) {
        message = "识别失败：还没有录入任何人脸模板";
        return false;
    }

    cv::Ptr<cv::face::LBPHFaceRecognizer> recognizer =
        cv::face::LBPHFaceRecognizer::create(1, 8, 8, 8, std::numeric_limits<double>::max());
    recognizer->train(trainingImages, labels);

    std::map<int, MatchVote> votes;
    int bestLabel = -1;
    double bestScore = std::numeric_limits<double>::max();

    for (std::size_t i = 0; i < currentFaces.size(); ++i) {
        int predictedLabel = -1;
        double distance = std::numeric_limits<double>::max();
        recognizer->predict(currentFaces[i], predictedLabel, distance);
        if (predictedLabel < 0) continue;

        MatchVote& vote = votes[predictedLabel];
        vote.count += 1;
        vote.scoreSum += distance;
        if (distance < vote.bestScore) vote.bestScore = distance;
        if (distance < bestScore) {
            bestScore = distance;
            bestLabel = predictedLabel;
        }
    }

    if (bestLabel < 0 || bestLabel >= static_cast<int>(employeeIds.size())) {
        message = "识别失败：没有可用的人脸模板";
        return false;
    }

    int acceptedLabel = -1;
    int acceptedVotes = 0;
    double acceptedAverage = std::numeric_limits<double>::max();
    double acceptedBestScore = std::numeric_limits<double>::max();
    for (std::map<int, MatchVote>::const_iterator it = votes.begin(); it != votes.end(); ++it) {
        const MatchVote& vote = it->second;
        double average = vote.scoreSum / vote.count;
        if (vote.count > acceptedVotes ||
            (vote.count == acceptedVotes && average < acceptedAverage)) {
            acceptedLabel = it->first;
            acceptedVotes = vote.count;
            acceptedAverage = average;
            acceptedBestScore = vote.bestScore;
        }
    }

    if (acceptedLabel < 0 || acceptedLabel >= static_cast<int>(employeeIds.size())) {
        message = "识别失败：没有可用的人脸模板";
        return false;
    }

    if (acceptedVotes < MIN_MATCH_VOTES || acceptedAverage > MATCH_THRESHOLD) {
        std::ostringstream output;
        output << "识别失败：最相似工号为 " << employeeIds[acceptedLabel]
               << "，平均匹配分数 " << acceptedAverage
               << "，有效帧 " << acceptedVotes << "/" << currentFaces.size()
               << "，阈值 " << MATCH_THRESHOLD;
        message = output.str();
        return false;
    }

    employeeId = employeeIds[acceptedLabel];
    score = acceptedAverage;

    std::ostringstream output;
    output << "人脸识别成功，工号: " << employeeId
           << "，平均匹配分数: " << score
           << "，最佳单帧: " << acceptedBestScore
           << "，有效帧: " << acceptedVotes << "/" << currentFaces.size();
    message = output.str();
    return true;
}

bool enrollFace(const std::string& employeeId, std::string& message) {
    if (employeeId.empty()) {
        message = "录入失败：工号不能为空";
        return false;
    }

    std::vector<cv::Mat> faceSamples;
    if (!captureFaceSamples(faceSamples, ENROLL_SAMPLE_COUNT, message)) return false;

    return saveFaceEnrollmentSamples(employeeId, faceSamples, message);
}

bool recognizeFace(std::string& employeeId, double& score, std::string& message) {
    std::vector<cv::Mat> currentFaces;
    if (!captureFaceSamples(currentFaces, RECOGNIZE_SAMPLE_COUNT, message)) return false;

    return recognizeFaceSamples(currentFaces, employeeId, score, message);
}

}  // namespace face
