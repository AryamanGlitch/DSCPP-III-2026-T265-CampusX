#pragma once
// Password hashing and session tokens.
//
// SHA-256 (FIPS 180-4), HMAC-SHA256 (RFC 2104) and PBKDF2 (RFC 8018) are
// written by hand here so the project has no OpenSSL dependency and you can
// explain them in a viva. They are verified against Python's hashlib in the
// tests. For a production system, use a vetted library (libsodium/OpenSSL).
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace cx::crypto {

using Bytes = std::vector<std::uint8_t>;

std::array<std::uint8_t, 32> sha256(const std::uint8_t* data, std::size_t len);
std::string sha256Hex(const std::string& text);
std::array<std::uint8_t, 32> hmacSha256(const Bytes& key, const Bytes& message);
Bytes pbkdf2Sha256(const std::string& password, const Bytes& salt, int iterations, std::size_t keyLen);

Bytes randomBytes(std::size_t n);
std::string toHex(const std::uint8_t* data, std::size_t len);
std::string toHex(const Bytes& b);
Bytes fromHex(const std::string& hex);  // throws on invalid input

// Compares every byte even after a mismatch, so response time doesn't
// reveal how many leading characters were correct.
bool constantTimeEquals(const std::string& a, const std::string& b);

// Stored format: pbkdf2_sha256$<iterations>$<salt hex>$<hash hex>
std::string hashPassword(const std::string& password, int iterations);
bool verifyPassword(const std::string& password, const std::string& stored);

}  // namespace cx::crypto
