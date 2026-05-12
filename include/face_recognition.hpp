#ifndef FACE_FACE_RECOGNITION_HPP
#define FACE_FACE_RECOGNITION_HPP

#include <string>

namespace face {

bool enrollFace(const std::string& employeeId, std::string& message);
bool recognizeFace(std::string& employeeId, double& score, std::string& message);

}  // namespace face

#endif
