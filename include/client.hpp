#ifndef FACE_CLIENT_HPP
#define FACE_CLIENT_HPP

#include <string>

namespace face {

bool isClientCommandName(const std::string& value);
int runClient(const std::string& host, int port);
int runClientOnce(const std::string& host, int port, const std::string& commandLine);

}  // namespace face

#endif
