#pragma once
#include <string>

#include "db/Database.h"
#include "services/AuthService.h"

namespace cx {

bool isDatabaseEmpty(Database& db);

// Wipes all tables and loads the SAMPLE campus. Development only.
void seedDatabase(Database& db, AuthService& auth, const std::string& demoPassword);

}  // namespace cx
