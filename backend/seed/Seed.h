#pragma once
#include <string>

#include "../database/Database.h"
#include "../services/AuthService.h"

namespace cx {

// The demo student. Unlike the other sample accounts it has its own fixed password,
// not the shared demo password (CAMPUSX_SEED_PASSWORD does not change it).
inline constexpr const char* DEMO_STUDENT_NAME = "Aryaman";
inline constexpr const char* DEMO_STUDENT_EMAIL = "aryaman@gmail.com";
inline constexpr const char* DEMO_STUDENT_PASSWORD = "qwerty123";

bool isDatabaseEmpty(Database& db);

// A sample database made before the demo student got its own login still has
// student@campusx.local with the shared password. Moves that account (same id,
// same data) to the DEMO_STUDENT_* login, so the old one stops working. Returns true if it did.
bool upgradeLegacyDemoStudent(Database& db, AuthService& auth);

// Wipes all tables and loads the SAMPLE campus. Development only.
void seedDatabase(Database& db, AuthService& auth, const std::string& demoPassword);

// SAMPLE student profiles + projects for partner matching. seedDatabase() calls it;
// main() also calls it for an older sample database that predates the feature.
bool partnerSampleMissing(Database& db);
void seedPartnerSample(Database& db, AuthService& auth, const std::string& demoPassword);

}  // namespace cx
