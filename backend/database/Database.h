#pragma once
// Thin RAII wrapper over the SQLite C API.
//  - Statement finalizes itself; always use bind() with ? placeholders,
//    never string concatenation, so user input can't inject SQL.
//  - One shared connection guarded by a recursive mutex. Services take
//    db.lock() for the whole operation, which also makes multi-statement
//    updates (like toggling a like) atomic. Fine at campus scale; a
//    connection pool would be the next step for heavy write traffic.
#include <mutex>
#include <string>

struct sqlite3;
struct sqlite3_stmt;

namespace cx {

class Statement {
public:
    Statement(sqlite3* db, const std::string& sql);
    ~Statement();
    Statement(Statement&& other) noexcept;
    Statement& operator=(Statement&&) = delete;
    Statement(const Statement&) = delete;
    Statement& operator=(const Statement&) = delete;

    // Parameters are 1-based, matching SQLite's ?1, ?2 ...
    Statement& bind(int i, long long v);
    Statement& bind(int i, int v) { return bind(i, static_cast<long long>(v)); }
    Statement& bind(int i, double v);
    Statement& bind(int i, const std::string& v);
    Statement& bind(int i, const char* v) { return bind(i, std::string(v)); }
    Statement& bindNull(int i);
    Statement& bindOptional(int i, const std::string& v) { return v.empty() ? bindNull(i) : bind(i, v); }

    bool step();  // true = a row is available; false = done. Throws on error.
    void run() { while (step()) {} }
    void reset();

    // Columns are 0-based.
    long long getInt(int col) const;
    double getDouble(int col) const;
    std::string getText(int col) const;
    bool isNull(int col) const;

private:
    void check(int rc, const char* what) const;
    sqlite3* db_;
    sqlite3_stmt* stmt_ = nullptr;
};

class Database {
public:
    explicit Database(const std::string& path);  // ":memory:" for tests
    ~Database();
    Database(const Database&) = delete;
    Database& operator=(const Database&) = delete;

    void exec(const std::string& sql);
    Statement prepare(const std::string& sql);
    long long lastInsertId() const;
    int changes() const;
    std::unique_lock<std::recursive_mutex> lock() { return std::unique_lock<std::recursive_mutex>(mu_); }

private:
    sqlite3* db_ = nullptr;
    std::recursive_mutex mu_;
};

// BEGIN IMMEDIATE ... COMMIT; rolls back automatically if commit() is never reached.
class Transaction {
public:
    explicit Transaction(Database& db);
    ~Transaction();
    void commit();

private:
    Database& db_;
    bool done_ = false;
};

void createSchema(Database& db);

}  // namespace cx
