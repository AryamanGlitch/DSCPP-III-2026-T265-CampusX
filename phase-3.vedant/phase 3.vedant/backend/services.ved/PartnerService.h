#pragma once
#include <string>
#include <vector>

#include "../database/Database.h"
#include "../DSA.rit/Matcher.h"
#include "../util/Json.h"
#include "../services/AuthService.h"

namespace cx {

// Raw form fields; the service validates them (main.cpp stays a thin layer).
struct ProfileInput {
    std::string department, semester, experience;
    std::string interests;     // comma-separated domain keys: "ai_ml,data_science"
    std::string availability;  // comma-separated slot numbers 0..20
    std::string skills;        // "Python:3,SQL:2" (name:level)
    std::string bio;
};

struct ProjectInput {
    std::string title, description, domain, teamSize, durationWeeks, experience;
    std::string availability, scheduleRequired;
    std::string skills;  // "Python:2:5:1,SQL:1:3:0" (name:level:importance:mandatory)
    std::string extra;
};

// Project partner matching: profiles, projects, recommendations, invitations.
// Recommendations are computed on every request from current data, so a
// changed profile or project is reflected immediately (no stale cache).
class PartnerService {
public:
    static constexpr int MAX_ACTIVE_PROJECTS = 3;  // recruiting, team complete or in progress
    static constexpr int MAX_PENDING_INVITES = 10; // per project, limits invitation spam

    explicit PartnerService(Database& db);

    struct ProjectRow {
        long long id = 0, creatorId = 0, createdAt = 0;
        std::string title, description, domain, extra, status, creatorName;
        int teamSize = 0, durationWeeks = 0, experience = 1;
        unsigned availability = 0;
        bool scheduleRequired = false;
        std::vector<match::SkillReq> skills;
        std::vector<std::string> skillNames;  // parallel to skills
    };

    // Display data that the matcher doesn't need, parallel to the profiles vector.
    struct StudentInfo {
        std::string name, department, bio;
        int semester = 0;
        std::vector<std::pair<std::string, int>> skills;  // name, level
    };

    Json meta();  // domains, time slots, levels, skill catalogue

    Json myProfile(const User& u);
    Json saveProfile(const User& u, const ProfileInput& in);
    void deleteProfile(const User& u);
    Json studentProfile(long long userId);

    Json createProject(const User& creator, const ProjectInput& in);
    Json listProjects(const User& u, const std::string& scope);  // "mine" or "open"
    Json project(const User& u, long long projectId);
    Json setStatus(const User& u, long long projectId, const std::string& status);

    // Ranked candidates + a greedy balanced team suggestion. Creator only.
    Json matches(const User& u, long long projectId, int limit);

    Json invite(const User& creator, long long projectId, long long studentId, const std::string& message);
    Json requestToJoin(const User& student, long long projectId, const std::string& message);
    Json respond(const User& u, long long invitationId, const std::string& action);  // accept|reject|withdraw
    Json invitations(const User& u);
    void leave(const User& u, long long projectId);

private:
    ProjectRow loadProject(long long id);  // throws 404
    std::vector<long long> memberIds(long long projectId);
    int activeProjectCount(long long userId);
    bool hasProfile(long long userId);
    // userId = 0: every profile; otherwise just that student's.
    std::vector<match::Profile> loadProfiles(std::vector<StudentInfo>& info, long long userId = 0);
    int skillIdFor(const std::string& name);  // finds or creates
    Json projectJson(const ProjectRow& p, long long viewerId);
    Json membersJson(const ProjectRow& p, std::vector<bool>* covered);
    Json invitationJson(long long id);
    void closePendingIfNotRecruiting(long long projectId);

    Database& db_;
};

}  // namespace cx
