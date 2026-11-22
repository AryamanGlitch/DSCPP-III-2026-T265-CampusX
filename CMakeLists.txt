cmake_minimum_required(VERSION 3.16)
project(CampusX LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(Threads REQUIRED)

# ---------- SQLite ----------
# Option 1 (works everywhere, incl. Windows): bundled amalgamation in third_party/sqlite
# Option 2: system package (Ubuntu: sudo apt install libsqlite3-dev, macOS: preinstalled)
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/sqlite/sqlite3.c")
  message(STATUS "SQLite: using bundled third_party/sqlite")
  add_library(sqlite3 STATIC third_party/sqlite/sqlite3.c)
  target_include_directories(sqlite3 PUBLIC third_party/sqlite)
  target_compile_definitions(sqlite3 PRIVATE SQLITE_THREADSAFE=1 SQLITE_OMIT_LOAD_EXTENSION)
  target_link_libraries(sqlite3 PUBLIC Threads::Threads)
  set(CAMPUSX_SQLITE sqlite3)
else()
  find_package(SQLite3 REQUIRED)
  message(STATUS "SQLite: using system library")
  set(CAMPUSX_SQLITE SQLite::SQLite3)
endif()

function(campusx_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /utf-8 /permissive-)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endif()
endfunction()

# ---------- Core: DSA + database + services (no networking) ----------
add_library(campusx_core STATIC
  backend/DSA.rit/Graph.cpp
  backend/DSA.rit/Matcher.cpp
  backend/dsa/SkillGap.cpp
  backend/dsa.ved/Trie.cpp
  backend/util/Crypto.cpp
  backend/util/Json.cpp
  backend/util/JsonParse.cpp
  backend/util/Text.cpp
  backend/database/Database.cpp
  backend/services/AuthService.cpp
  backend/services/SearchService.cpp
  backend/services.ved/FeedService.cpp
  backend/Services.rit/NavService.cpp
  backend/services.ved/PartnerService.cpp
  backend/services/GeminiClient.cpp
  backend/services/LearningService.cpp
  backend/seed/Seed.cpp
)
target_include_directories(campusx_core PUBLIC ${CMAKE_SOURCE_DIR})
target_link_libraries(campusx_core PUBLIC ${CAMPUSX_SQLITE} Threads::Threads)

# ---------- libcurl (optional): Gemini API calls for learning recommendations ----------
# macOS: built in. Ubuntu: sudo apt install libcurl4-openssl-dev. MSYS2: mingw-w64-ucrt-x86_64-curl.
# Without it everything still builds; the Skill Gap page shows standard search suggestions instead.
find_package(CURL QUIET)
if(CURL_FOUND)
  message(STATUS "libcurl: found, Gemini recommendations enabled")
  target_link_libraries(campusx_core PUBLIC CURL::libcurl)
  target_compile_definitions(campusx_core PRIVATE CAMPUSX_HAVE_CURL)
else()
  message(WARNING "libcurl not found: Gemini recommendations are off (the rest of CampusX works).")
endif()
campusx_warnings(campusx_core)

# ---------- Tests (always buildable) ----------
add_executable(campusx_tests TEST/test_main.cpp)
target_link_libraries(campusx_tests PRIVATE campusx_core)
campusx_warnings(campusx_tests)
enable_testing()
add_test(NAME campusx_tests COMMAND campusx_tests)

# ---------- Web server (needs third_party/httplib.h) ----------
if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/httplib.h")
  add_executable(campusx main.cpp)
  target_include_directories(campusx PRIVATE third_party)
  target_link_libraries(campusx PRIVATE campusx_core)
  campusx_warnings(campusx)
  if(WIN32)
    # cpp-httplib needs Windows 10+ APIs and Winsock.
    target_compile_definitions(campusx PRIVATE _WIN32_WINNT=0x0A00 NOMINMAX)
    target_link_libraries(campusx PRIVATE ws2_32)
  endif()
else()
  message(WARNING "third_party/httplib.h not found: only the tests will build. "
                  "Run scripts/setup.sh (Linux/macOS) or scripts/setup.ps1 (Windows).")
endif()
