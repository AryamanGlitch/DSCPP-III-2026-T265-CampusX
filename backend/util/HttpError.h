#pragma once
#include <stdexcept>
#include <string>

namespace cx {

// Thrown by services; the HTTP layer turns it into {"error": message} with this status.
struct HttpError : std::runtime_error {
    int status;
    HttpError(int s, const std::string& message) : std::runtime_error(message), status(s) {}
};

}  // namespace cx
