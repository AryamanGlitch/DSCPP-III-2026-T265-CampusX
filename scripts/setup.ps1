# Windows: downloads cpp-httplib and the SQLite amalgamation into third_party\.
# Run from PowerShell:  powershell -ExecutionPolicy Bypass -File scripts\setup.ps1
$ErrorActionPreference = "Stop"
Set-Location (Join-Path $PSScriptRoot "..")
New-Item -ItemType Directory -Force -Path third_party\sqlite | Out-Null

$tag = if ($env:HTTPLIB_TAG) { $env:HTTPLIB_TAG } else { "v0.18.1" }
$sqliteUrl = if ($env:SQLITE_URL) { $env:SQLITE_URL } else { "https://www.sqlite.org/2024/sqlite-amalgamation-3460100.zip" }

Write-Host "Downloading cpp-httplib $tag..."
try {
  Invoke-WebRequest "https://raw.githubusercontent.com/yhirose/cpp-httplib/$tag/httplib.h" -OutFile third_party\httplib.h
} catch {
  Write-Host "  Tag $tag not found, falling back to the latest master."
  Invoke-WebRequest "https://raw.githubusercontent.com/yhirose/cpp-httplib/master/httplib.h" -OutFile third_party\httplib.h
}

Write-Host "Downloading SQLite amalgamation..."
$tmp = Join-Path $env:TEMP ("sqlite-" + [guid]::NewGuid())
New-Item -ItemType Directory -Path $tmp | Out-Null
try {
  Invoke-WebRequest $sqliteUrl -OutFile "$tmp\sqlite.zip"
  Expand-Archive "$tmp\sqlite.zip" -DestinationPath $tmp
  Copy-Item "$tmp\sqlite-amalgamation-*\sqlite3.c", "$tmp\sqlite-amalgamation-*\sqlite3.h" third_party\sqlite\
} catch {
  Write-Host "  Could not download SQLite. Download the amalgamation zip from https://www.sqlite.org/download.html"
  Write-Host "  and copy sqlite3.c and sqlite3.h into third_party\sqlite\."
} finally {
  Remove-Item -Recurse -Force $tmp
}
Write-Host "Done. Next: cmake -S . -B build; cmake --build build --config Release"
