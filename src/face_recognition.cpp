#include "face_recognition.hpp"

#include <dirent.h>
#include <sys/stat.h>

#include <cstdlib>
#include <map>
#include <limits>
#include <sstream>
#include <string>
#include <vector>

#include <opencv2/face.hpp>
#include <opencv2/opencv.hpp>

namespace face {
namespace {

const char* PHOTO_DIR = "photos";
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

bool hasDisplay() {
    return std::getenv("DISPLAY") != NULL || std::getenv("WAYLAND_DISPLAY") != NULL;
}

void ensurePhotoDir() {
    mkdir(PHOTO_DIR, 0755);
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
    return std::string(PHOTO_DIR) + "/" + employeeId + ".png";
}

std::string sampleDir(const std::string& employeeId) {
    return std::string(PHOTO_DIR) + "/" + employeeId;
}

std::string samplePath(const std::string& employeeId, int index) {
    std::ostringstream output;
    output << sampleDir(employeeId) << "/sample_" << index << ".png";
    return output.str();
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
    DIR* dir = opendir(PHOTO_DIR);
    if (!dir) return;

    for (dirent* entry = readdir(dir); entry; entry = readdir(dir)) {
        std::string name = entry->d_name;
        if (name == "." || name == "..") continue;

        std::string path = std::string(PHOTO_DIR) + "/" + name;
        struct stat info;
        if (stat(path.c_str(), &info) != 0) continue;

        if (S_ISREG(info.st_mode) && isPngFile(name)) {
            addTemplateImage(fileStem(name), path, images, labels, employeeIds);
        } else if (S_ISDIR(info.st_mode)) {
            loadSampleDirectory(name, path, images, labels, employeeIds);
        }
    }
}

}  // namespace

int enrollFaceSampleCount() {
    return ENROLL_SAMPLE_COUNT;
}

int recognizeFaceSampleCount() {
    return RECOGNIZE_SAMPLE_COUNT;
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
