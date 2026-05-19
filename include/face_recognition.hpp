#ifndef FACE_FACE_RECOGNITION_HPP
#define FACE_FACE_RECOGNITION_HPP

#include <string>
#include <vector>

#include <opencv2/core.hpp>
#include <opencv2/objdetect.hpp>

namespace face {

std::string faceDataRootDir();
std::string faceStorageRootDir();
int enrollFaceSampleCount();
int recognizeFaceSampleCount();
bool captureFaceSamplesForNetwork(int sampleCount, std::vector<cv::Mat>& samples,
                                  std::string& message);
bool encodeFaceSamplesForNetwork(const std::vector<cv::Mat>& samples,
                                 std::string& payload, std::string& message);
bool decodeFaceSamplesFromNetwork(const std::string& payload,
                                  std::vector<cv::Mat>& samples,
                                  std::string& message);
bool loadFaceCascadeForCapture(cv::CascadeClassifier& cascade, std::string& message);
bool findLargestFaceForCapture(const cv::Mat& frame, cv::CascadeClassifier& cascade,
                               cv::Rect& face);
cv::Mat normalizeFaceForCapture(const cv::Mat& frame, const cv::Rect& faceRect);
bool saveFaceEnrollmentSamples(const std::string& employeeId,
                               const std::vector<cv::Mat>& faceSamples,
                               std::string& message);
bool recognizeFaceSamples(const std::vector<cv::Mat>& currentFaces,
                          std::string& employeeId, double& score,
                          std::string& message);
bool enrollFace(const std::string& employeeId, std::string& message);
bool recognizeFace(std::string& employeeId, double& score, std::string& message);

}  // namespace face

#endif
