#!/usr/bin/env bash
# Downloads the two third-party dependencies into third_party/.
#   cpp-httplib : single-header HTTP server (MIT licence)
#   SQLite      : amalgamation (public domain). Skip with --system-sqlite if
#                 you installed libsqlite3-dev / brew sqlite instead.
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p third_party

HTTPLIB_TAG="${HTTPLIB_TAG:-v0.18.1}"
SQLITE_URL="${SQLITE_URL:-https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip}"

fetch() { curl -fsSL "$1" -o "$2"; }

echo "Downloading cpp-httplib ${HTTPLIB_TAG}..."
if ! fetch "https://raw.githubusercontent.com/yhirose/cpp-httplib/${HTTPLIB_TAG}/httplib.h" third_party/httplib.h; then
  echo "  Tag ${HTTPLIB_TAG} not found, falling back to the latest master."
  fetch "https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h" third_party/httplib.h
fi

if [[ "${1:-}" != "--system-sqlite" ]]; then
  echo "Downloading SQLite amalgamation..."
  tmp="$(mktemp -d)"
  if fetch "$SQLITE_URL" "$tmp/sqlite.zip"; then
    unzip -q "$tmp/sqlite.zip" -d "$tmp"
    mkdir -p third_party/sqlite
    cp "$tmp"/sqlite-amalgamation-*/sqlite3.c "$tmp"/sqlite-amalgamation-*/sqlite3.h third_party/sqlite/
  else
    echo "  Could not download SQLite. Either install libsqlite3-dev (Ubuntu) / sqlite (Homebrew),"
    echo "  or download the amalgamation zip from https://www.sqlite.org/download.html"
    echo "  and copy sqlite3.c + sqlite3.h into third_party/sqlite/."
  fi
  rm -rf "$tmp"
fi
echo "Done. Next: cmake -S . -B build && cmake --build build"
