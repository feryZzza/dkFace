#ifndef FACE_SERVER_HPP
#define FACE_SERVER_HPP

#include <string>

namespace face {

int runServer(int port, const std::string& dateOverride = "",
              const std::string& timeOverride = "");

}  // namespace face

#endif
