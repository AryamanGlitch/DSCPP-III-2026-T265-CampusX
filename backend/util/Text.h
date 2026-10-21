#pragma once
#include <chrono>
#include <string>
#include <unordered_map>
#include <vector>

namespace cx {

inline long long nowMs() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

std::string trim(const std::string& s);
std::string toLowerAscii(std::string s);

// Lower-cased runs of letters/digits. Bytes >= 0x80 count as letters so
// UTF-8 words (e.g. Hindi) stay searchable as whole tokens.
std::vector<std::string> tokenize(const std::string& text);

std::string urlDecode(const std::string& s);
std::unordered_map<std::string, std::string> parseForm(const std::string& body);

bool isDigits(const std::string& s);
long long toInt(const std::string& s, long long fallback);

}  // namespace cx
