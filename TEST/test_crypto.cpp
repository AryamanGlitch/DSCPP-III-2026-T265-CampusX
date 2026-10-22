// Tests: hashing (SHA-256, HMAC, PBKDF2) against reference vectors.
#include "TestHarness.h"

TEST(sha256_and_pbkdf2_match_reference_vectors) {
    CHECK(crypto::sha256Hex("abc") == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    CHECK(crypto::sha256Hex("") == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    CHECK(crypto::sha256Hex(std::string(1000, 'a')) == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
    // Generated with Python: hashlib.pbkdf2_hmac('sha256', ...)
    CHECK(crypto::toHex(crypto::pbkdf2Sha256("passwd", {'s', 'a', 'l', 't'}, 1, 64)) ==
          "55ac046e56e3089fec1691c22544b605f94185216dde0465e68b9d57c20dacbc"
          "49ca9cccf179b645991664b39d77ef317c71b845b1e30bd509112041d3a19783");
    CHECK(crypto::toHex(crypto::pbkdf2Sha256("ChangeMe@123", crypto::fromHex("00112233445566778899aabbccddeeff"), 1000, 32)) ==
          "048aa15d07352d553512dad8e51b466071eaa43092f917b57719569b5f112f56");
    std::string stored = crypto::hashPassword("secret123", 500);
    CHECK(crypto::verifyPassword("secret123", stored));
    CHECK(!crypto::verifyPassword("secret124", stored));
    CHECK(!crypto::verifyPassword("secret123", "garbage"));
}
