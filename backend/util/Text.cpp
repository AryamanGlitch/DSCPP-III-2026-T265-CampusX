#include "util/Text.h"

#include <cctype>

namespace cx {

std::string trim(const std::string& s) {
    std::size_t b = 0, e = s.size();
    while (b < e && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
    while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
    return s.substr(b, e - b);
}

std::string toLowerAscii(std::string s) {
    for (char& c : s) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return s;
}

std::vector<std::string> tokenize(const std::string& text) {
    std::vector<std::string> tokens;
    std::string cur;
    for (unsigned char c : text) {
        if (std::isalnum(c) || c >= 0x80) {
            cur += static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        } else if (!cur.empty()) {
            tokens.push_back(std::move(cur));
            cur.clear();
        }
    }
    if (!cur.empty()) tokens.push_back(std::move(cur));
    return tokens;
}

static int hexValue(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string urlDecode(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        if (s[i] == '+') {
            out += ' ';
        } else if (s[i] == '%' && i + 2 < s.size() && hexValue(s[i + 1]) >= 0 && hexValue(s[i + 2]) >= 0) {
            out += static_cast<char>(hexValue(s[i + 1]) * 16 + hexValue(s[i + 2]));
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

std::unordered_map<std::string, std::string> parseForm(const std::string& body) {
    std::unordered_map<std::string, std::string> out;
    std::size_t start = 0;
    while (start <= body.size()) {
        std::size_t amp = body.find('&', start);
        if (amp == std::string::npos) amp = body.size();
        std::string pair = body.substr(start, amp - start);
        if (!pair.empty()) {
            std::size_t eq = pair.find('=');
            std::string key = urlDecode(pair.substr(0, eq));
            std::string value = eq == std::string::npos ? "" : urlDecode(pair.substr(eq + 1));
            out[key] = value;
        }
        start = amp + 1;
    }
    return out;
}

bool isDigits(const std::string& s) {
    if (s.empty() || s.size() > 18) return false;
    for (char c : s) if (c < '0' || c > '9') return false;
    return true;
}

long long toInt(const std::string& s, long long fallback) {
    return isDigits(s) ? std::stoll(s) : fallback;
}

}  // namespace cx
