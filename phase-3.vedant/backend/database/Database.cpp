#include "Database.h"

#include <sqlite3.h>

#include <stdexcept>

#include "../util/HttpError.h"

namespace cx {

Statement::Statement(sqlite3* db, const std::string& sql) : db_(db) {
    int rc = sqlite3_prepare_v2(db, sql.c_str(), static_cast<int>(sql.size()), &stmt_, nullptr);
    if (rc != SQLITE_OK) {
        throw std::runtime_error(std::string("SQL prepare failed: ") + sqlite3_errmsg(db) + "\n  in: " + sql);
    }
}

Statement::~Statement() {
    if (stmt_) sqlite3_finalize(stmt_);
}

Statement::Statement(Statement&& other) noexcept : db_(other.db_), stmt_(other.stmt_) { other.stmt_ = nullptr; }

void Statement::check(int rc, const char* what) const {
    if (rc != SQLITE_OK) throw std::runtime_error(std::string(what) + ": " + sqlite3_errmsg(db_));
}

Statement& Statement::bind(int i, long long v) { check(sqlite3_bind_int64(stmt_, i, v), "bind"); return *this; }
Statement& Statement::bind(int i, double v) { check(sqlite3_bind_double(stmt_, i, v), "bind"); return *this; }
Statement& Statement::bind(int i, const std::string& v) {
    check(sqlite3_bind_text(stmt_, i, v.c_str(), static_cast<int>(v.size()), SQLITE_TRANSIENT), "bind");
    return *this;
}
Statement& Statement::bindNull(int i) { check(sqlite3_bind_null(stmt_, i), "bind"); return *this; }

bool Statement::step() {
    int rc = sqlite3_step(stmt_);
    if (rc == SQLITE_ROW) return true;
    if (rc == SQLITE_DONE) return false;
    // Constraint failures (UNIQUE email, CHECK, FOREIGN KEY) are the caller's fault, not ours.
    if ((sqlite3_extended_errcode(db_) & 0xff) == SQLITE_CONSTRAINT) {
        throw HttpError(409, std::string("Conflict: ") + sqlite3_errmsg(db_));
    }
    throw std::runtime_error(std::string("SQL step failed: ") + sqlite3_errmsg(db_));
}

void Statement::reset() { sqlite3_reset(stmt_); }

long long Statement::getInt(int col) const { return sqlite3_column_int64(stmt_, col); }
double Statement::getDouble(int col) const { return sqlite3_column_double(stmt_, col); }
std::string Statement::getText(int col) const {
    const unsigned char* t = sqlite3_column_text(stmt_, col);
    return t ? std::string(reinterpret_cast<const char*>(t), sqlite3_column_bytes(stmt_, col)) : std::string();
}
bool Statement::isNull(int col) const { return sqlite3_column_type(stmt_, col) == SQLITE_NULL; }

Database::Database(const std::string& path) {
    int rc = sqlite3_open_v2(path.c_str(), &db_, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_FULLMUTEX, nullptr);
    if (rc != SQLITE_OK) {
        std::string msg = db_ ? sqlite3_errmsg(db_) : "out of memory";
        if (db_) sqlite3_close_v2(db_);
        throw std::runtime_error("Cannot open database " + path + ": " + msg);
    }
    sqlite3_busy_timeout(db_, 5000);
    exec("PRAGMA foreign_keys = ON;");  // off by default in SQLite, needed for ON DELETE CASCADE
    if (path != ":memory:") exec("PRAGMA journal_mode = WAL;");
}

Database::~Database() {
    if (db_) sqlite3_close_v2(db_);
}

void Database::exec(const std::string& sql) {
    char* err = nullptr;
    if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &err) != SQLITE_OK) {
        std::string msg = err ? err : "unknown error";
        sqlite3_free(err);
        throw std::runtime_error("SQL exec failed: " + msg);
    }
}

Statement Database::prepare(const std::string& sql) { return Statement(db_, sql); }
long long Database::lastInsertId() const { return sqlite3_last_insert_rowid(db_); }
int Database::changes() const { return sqlite3_changes(db_); }

Transaction::Transaction(Database& db) : db_(db) { db_.exec("BEGIN IMMEDIATE;"); }
Transaction::~Transaction() {
    if (!done_) {
        try { db_.exec("ROLLBACK;"); } catch (...) {}
    }
}
void Transaction::commit() {
    db_.exec("COMMIT;");
    done_ = true;
}

void createSchema(Database& db) {
    db.exec(R"SQL(
CREATE TABLE IF NOT EXISTS users (
  id            INTEGER PRIMARY KEY,
  name          TEXT NOT NULL,
  email         TEXT NOT NULL UNIQUE,
  password_hash TEXT NOT NULL,
  role          TEXT NOT NULL DEFAULT 'student' CHECK (role IN ('student','faculty','club','admin')),
  created_at    INTEGER NOT NULL
);
-- Only a SHA-256 of each session token is stored, so a leaked DB file can't be used to log in.
CREATE TABLE IF NOT EXISTS sessions (
  token_hash TEXT PRIMARY KEY,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  expires_at INTEGER NOT NULL
);

CREATE TABLE IF NOT EXISTS places (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, type TEXT NOT NULL, code TEXT UNIQUE,
  description TEXT, tags TEXT, building TEXT, floor INTEGER, nav_node_id TEXT, lat REAL, lng REAL
);
CREATE TABLE IF NOT EXISTS building_floors (
  building TEXT NOT NULL, level INTEGER NOT NULL, name TEXT NOT NULL,
  width REAL NOT NULL, height REAL NOT NULL, PRIMARY KEY (building, level)
);
CREATE TABLE IF NOT EXISTS faculty (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, designation TEXT, department TEXT,
  email TEXT, cabin TEXT, nav_node_id TEXT, research_areas TEXT
);
CREATE TABLE IF NOT EXISTS events (
  id INTEGER PRIMARY KEY, title TEXT NOT NULL, description TEXT, venue TEXT, nav_node_id TEXT,
  starts_at INTEGER NOT NULL, ends_at INTEGER NOT NULL, organizer TEXT, tags TEXT
);

CREATE TABLE IF NOT EXISTS posts (
  id            INTEGER PRIMARY KEY,
  author_id     INTEGER REFERENCES users(id) ON DELETE SET NULL,
  content       TEXT NOT NULL,
  category      TEXT NOT NULL,
  is_official   INTEGER NOT NULL DEFAULT 0,
  like_count    INTEGER NOT NULL DEFAULT 0,
  comment_count INTEGER NOT NULL DEFAULT 0,
  created_at    INTEGER NOT NULL
);
-- B-tree indexes matching the feed's ORDER BY, so paging never sorts the whole table.
CREATE INDEX IF NOT EXISTS idx_posts_time     ON posts (created_at DESC, id DESC);
CREATE INDEX IF NOT EXISTS idx_posts_cat_time ON posts (category, created_at DESC, id DESC);
-- Composite primary key = a user can like a post at most once, enforced by the database.
CREATE TABLE IF NOT EXISTS likes (
  post_id INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
  user_id INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  PRIMARY KEY (post_id, user_id)
);
CREATE TABLE IF NOT EXISTS comments (
  id         INTEGER PRIMARY KEY,
  post_id    INTEGER NOT NULL REFERENCES posts(id) ON DELETE CASCADE,
  author_id  INTEGER REFERENCES users(id) ON DELETE SET NULL,
  content    TEXT NOT NULL,
  created_at INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_comments_post ON comments (post_id, created_at);

CREATE TABLE IF NOT EXISTS nav_nodes (
  id TEXT PRIMARY KEY, name TEXT NOT NULL, type TEXT NOT NULL,
  building TEXT, floor INTEGER, x REAL, y REAL, lat REAL, lng REAL
);
CREATE TABLE IF NOT EXISTS nav_edges (
  from_id TEXT NOT NULL REFERENCES nav_nodes(id),
  to_id   TEXT NOT NULL REFERENCES nav_nodes(id),
  weight  REAL NOT NULL CHECK (weight >= 0),
  kind    TEXT NOT NULL DEFAULT 'walk' CHECK (kind IN ('walk','stairs','elevator','ramp')),
  one_way INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (from_id, to_id)
);
CREATE TABLE IF NOT EXISTS hazards (
  id          INTEGER PRIMARY KEY,
  node_id     TEXT REFERENCES nav_nodes(id),
  edge_from   TEXT REFERENCES nav_nodes(id),
  edge_to     TEXT REFERENCES nav_nodes(id),
  description TEXT NOT NULL,
  active      INTEGER NOT NULL DEFAULT 1,
  reported_by INTEGER REFERENCES users(id) ON DELETE SET NULL,
  created_at  INTEGER NOT NULL,
  resolved_at INTEGER
);
CREATE TABLE IF NOT EXISTS contacts (
  id INTEGER PRIMARY KEY, name TEXT NOT NULL, phone TEXT NOT NULL, category TEXT NOT NULL,
  available_24x7 INTEGER NOT NULL DEFAULT 1, priority INTEGER NOT NULL DEFAULT 10
);

-- Project partner matching. A profile is opt-in: students without one are never recommended.
-- availability = 21-bit mask (bit day*3 + period, Monday morning = bit 0); interests = domain bits.
CREATE TABLE IF NOT EXISTS partner_profiles (
  user_id      INTEGER PRIMARY KEY REFERENCES users(id) ON DELETE CASCADE,
  department   TEXT NOT NULL,
  semester     INTEGER NOT NULL CHECK (semester BETWEEN 1 AND 10),
  experience   INTEGER NOT NULL CHECK (experience BETWEEN 1 AND 3),
  interests    INTEGER NOT NULL DEFAULT 0,
  availability INTEGER NOT NULL DEFAULT 0,
  bio          TEXT NOT NULL DEFAULT '',
  updated_at   INTEGER NOT NULL
);
-- name_key is the lower-cased name, so "python" and "Python" are one skill.
CREATE TABLE IF NOT EXISTS skills (
  id       INTEGER PRIMARY KEY,
  name     TEXT NOT NULL,
  name_key TEXT NOT NULL UNIQUE
);
CREATE TABLE IF NOT EXISTS student_skills (
  user_id  INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  skill_id INTEGER NOT NULL REFERENCES skills(id) ON DELETE CASCADE,
  level    INTEGER NOT NULL CHECK (level BETWEEN 1 AND 3),
  PRIMARY KEY (user_id, skill_id)
);
CREATE INDEX IF NOT EXISTS idx_student_skills_skill ON student_skills (skill_id);
CREATE TABLE IF NOT EXISTS projects (
  id                INTEGER PRIMARY KEY,
  creator_id        INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  title             TEXT NOT NULL,
  description       TEXT NOT NULL,
  domain            TEXT NOT NULL,
  team_size         INTEGER NOT NULL CHECK (team_size BETWEEN 2 AND 8),
  duration_weeks    INTEGER NOT NULL CHECK (duration_weeks BETWEEN 1 AND 52),
  experience        INTEGER NOT NULL CHECK (experience BETWEEN 1 AND 3),
  availability      INTEGER NOT NULL DEFAULT 0,
  schedule_required INTEGER NOT NULL DEFAULT 0,
  extra             TEXT NOT NULL DEFAULT '',
  status            TEXT NOT NULL DEFAULT 'recruiting'
                    CHECK (status IN ('recruiting','team_complete','in_progress','completed','cancelled')),
  created_at        INTEGER NOT NULL
);
CREATE INDEX IF NOT EXISTS idx_projects_status ON projects (status, created_at DESC);
CREATE TABLE IF NOT EXISTS project_skills (
  project_id     INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  skill_id       INTEGER NOT NULL REFERENCES skills(id),
  required_level INTEGER NOT NULL CHECK (required_level BETWEEN 1 AND 3),
  importance     INTEGER NOT NULL CHECK (importance BETWEEN 1 AND 5),
  mandatory      INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (project_id, skill_id)
);
CREATE TABLE IF NOT EXISTS project_members (
  project_id INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  user_id    INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  role       TEXT NOT NULL CHECK (role IN ('creator','member')),
  joined_at  INTEGER NOT NULL,
  PRIMARY KEY (project_id, user_id)
);
CREATE INDEX IF NOT EXISTS idx_members_user ON project_members (user_id);
-- kind 'invite' = creator asked the student; 'request' = student asked to join.
-- student_id is always the non-creator side, so one pending row per (project, student) covers both.
CREATE TABLE IF NOT EXISTS invitations (
  id           INTEGER PRIMARY KEY,
  project_id   INTEGER NOT NULL REFERENCES projects(id) ON DELETE CASCADE,
  student_id   INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  kind         TEXT NOT NULL CHECK (kind IN ('invite','request')),
  status       TEXT NOT NULL DEFAULT 'pending' CHECK (status IN ('pending','accepted','rejected','withdrawn')),
  message      TEXT NOT NULL DEFAULT '',
  created_at   INTEGER NOT NULL,
  responded_at INTEGER
);
-- Partial unique index: at most one PENDING invitation per student per project, enforced by the DB.
CREATE UNIQUE INDEX IF NOT EXISTS idx_invitations_pending ON invitations (project_id, student_id) WHERE status = 'pending';
CREATE INDEX IF NOT EXISTS idx_invitations_student ON invitations (student_id, created_at DESC);

-- Skill gap learning recommendations. Goals and their required skills are built into
-- LearningService; a student's skills are the ones in their partner profile.
-- status: 'new' (latest suggestions, replaced on the next run), 'saved', 'completed'.
-- UNIQUE(user, goal, title_key) stops the same suggestion being stored twice.
CREATE TABLE IF NOT EXISTS learning_recommendations (
  id          INTEGER PRIMARY KEY,
  user_id     INTEGER NOT NULL REFERENCES users(id) ON DELETE CASCADE,
  goal_key    TEXT NOT NULL,
  skill       TEXT NOT NULL,
  title       TEXT NOT NULL,
  title_key   TEXT NOT NULL,
  description TEXT NOT NULL DEFAULT '',
  level       TEXT NOT NULL DEFAULT '',
  topics      TEXT NOT NULL DEFAULT '',
  score       REAL NOT NULL,
  source      TEXT NOT NULL CHECK (source IN ('gemini','fallback')),
  status      TEXT NOT NULL DEFAULT 'new' CHECK (status IN ('new','saved','completed')),
  created_at  INTEGER NOT NULL,
  updated_at  INTEGER NOT NULL,
  UNIQUE (user_id, goal_key, title_key)
);
)SQL");
}

}  // namespace cx
