// SAMPLE DATA ONLY.
// Coordinates, rooms, people and two phone numbers are placeholders. Replace
// them with surveyed data (estate office floor plans + GPS points walked on
// site + real security numbers) before any demo that claims to be real.
#include "seed/Seed.h"

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <unordered_map>

#include "util/Text.h"

namespace cx {

namespace {

constexpr double ORIGIN_LAT = 30.2685, ORIGIN_LNG = 77.9945;  // placeholder: measure your own gate
constexpr double M_PER_DEG_LAT = 111320.0, PI = 3.14159265358979323846;

struct SeedNode {
    const char* id;
    const char* name;
    const char* type;
    const char* building;  // nullptr = outdoor
    int floor;
    double a, b;  // outdoor: metres east/north of origin; indoor: x/y metres on plan
};

struct SeedEdge {
    const char* from;
    const char* to;
    const char* kind;
    double weight;  // < 0 = compute from coordinates
};

const SeedNode NODES[] = {
    // Outdoor (metres east, north of the main gate)
    {"OUT-GATE", "Main Gate", "outdoor", nullptr, 0, 0, 0},
    {"OUT-AP-B", "Assembly Point B (Front Lawn)", "assembly", nullptr, 0, 40, 20},
    {"OUT-MED", "Medical Centre", "medical", nullptr, 0, -90, 20},
    {"OUT-J1", "Central Junction", "outdoor", nullptr, 0, 0, 80},
    {"OUT-J2", "East Path", "outdoor", nullptr, 0, 90, 80},
    {"OUT-J3", "West Path", "outdoor", nullptr, 0, -90, 80},
    {"OUT-AP-A", "Assembly Point A (Sports Ground)", "assembly", nullptr, 0, -150, 120},
    {"OUT-ABA-FRONT", "Block A Forecourt", "outdoor", nullptr, 0, 0, 120},
    {"OUT-ABA-WEST", "Block A West Lane", "outdoor", nullptr, 0, -45, 145},
    {"OUT-LIB", "Central Library Entrance", "entrance", nullptr, 0, 90, 150},
    // Academic Block A, ground floor (x/y metres; plan origin top-left)
    {"ABA-0-MAIN", "Main Entrance", "exit", "ABA", 0, 30, 30},
    {"ABA-0-FIRE", "West Fire Exit", "exit", "ABA", 0, 0, 15},
    {"ABA-0-C1", "West Corridor", "corridor", "ABA", 0, 12, 15},
    {"ABA-0-C2", "Central Corridor", "corridor", "ABA", 0, 30, 15},
    {"ABA-0-C3", "East Corridor", "corridor", "ABA", 0, 48, 15},
    {"ABA-0-R001", "Room 001", "room", "ABA", 0, 12, 5},
    {"ABA-0-R002", "Room 002", "room", "ABA", 0, 48, 5},
    {"ABA-0-LAB", "Computer Lab 1", "room", "ABA", 0, 48, 25},
    {"ABA-0-ST", "East Stairwell", "stairs", "ABA", 0, 56, 15},
    {"ABA-0-STW", "West Stairwell", "stairs", "ABA", 0, 5, 25},
    {"ABA-0-LIFT", "Lift", "elevator", "ABA", 0, 30, 5},
    // Academic Block A, first floor
    {"ABA-1-C1", "West Corridor", "corridor", "ABA", 1, 12, 15},
    {"ABA-1-C2", "Central Corridor", "corridor", "ABA", 1, 30, 15},
    {"ABA-1-C3", "East Corridor", "corridor", "ABA", 1, 48, 15},
    {"ABA-1-R101", "CSE Department Office", "room", "ABA", 1, 12, 5},
    {"ABA-1-R102", "Room 102", "room", "ABA", 1, 48, 5},
    {"ABA-1-R103", "Seminar Hall", "room", "ABA", 1, 12, 25},
    {"ABA-1-ST", "East Stairwell", "stairs", "ABA", 1, 56, 15},
    {"ABA-1-STW", "West Stairwell", "stairs", "ABA", 1, 5, 25},
    {"ABA-1-LIFT", "Lift", "elevator", "ABA", 1, 30, 5},
    {"ABA-1-REFUGE", "Refuge Area (East)", "refuge", "ABA", 1, 56, 25},
};

const SeedEdge EDGES[] = {
    // Outdoor paths
    {"OUT-GATE", "OUT-AP-B", "walk", -1}, {"OUT-GATE", "OUT-J1", "walk", -1}, {"OUT-AP-B", "OUT-J1", "walk", -1},
    {"OUT-J1", "OUT-J2", "walk", -1}, {"OUT-J1", "OUT-J3", "walk", -1}, {"OUT-J3", "OUT-MED", "walk", -1},
    {"OUT-J3", "OUT-AP-A", "walk", -1}, {"OUT-J2", "OUT-LIB", "walk", -1}, {"OUT-J1", "OUT-ABA-FRONT", "walk", -1},
    {"OUT-ABA-WEST", "OUT-AP-A", "walk", -1}, {"OUT-ABA-WEST", "OUT-J3", "walk", -1},
    // Block A ground floor
    {"ABA-0-MAIN", "ABA-0-C2", "walk", -1}, {"ABA-0-FIRE", "ABA-0-C1", "walk", -1}, {"ABA-0-C1", "ABA-0-C2", "walk", -1},
    {"ABA-0-C2", "ABA-0-C3", "walk", -1}, {"ABA-0-C1", "ABA-0-R001", "walk", -1}, {"ABA-0-C3", "ABA-0-R002", "walk", -1},
    {"ABA-0-C3", "ABA-0-LAB", "walk", -1}, {"ABA-0-C3", "ABA-0-ST", "walk", -1}, {"ABA-0-C1", "ABA-0-STW", "walk", -1},
    {"ABA-0-C2", "ABA-0-LIFT", "walk", -1},
    // Block A first floor
    {"ABA-1-C1", "ABA-1-C2", "walk", -1}, {"ABA-1-C2", "ABA-1-C3", "walk", -1}, {"ABA-1-C1", "ABA-1-R101", "walk", -1},
    {"ABA-1-C3", "ABA-1-R102", "walk", -1}, {"ABA-1-C1", "ABA-1-R103", "walk", -1}, {"ABA-1-C3", "ABA-1-ST", "walk", -1},
    {"ABA-1-C1", "ABA-1-STW", "walk", -1}, {"ABA-1-C2", "ABA-1-LIFT", "walk", -1}, {"ABA-1-ST", "ABA-1-REFUGE", "walk", -1},
    {"ABA-1-C3", "ABA-1-REFUGE", "walk", -1},
    // Between floors. One flight of stairs is slower than flat walking, so it costs extra.
    {"ABA-0-ST", "ABA-1-ST", "stairs", 20}, {"ABA-0-STW", "ABA-1-STW", "stairs", 20},
    {"ABA-0-LIFT", "ABA-1-LIFT", "elevator", 10},
    // Doorways join the indoor (x/y) and outdoor (lat/lng) coordinate systems, so weights are explicit.
    {"ABA-0-MAIN", "OUT-ABA-FRONT", "walk", 10}, {"ABA-0-FIRE", "OUT-ABA-WEST", "walk", 15},
};

struct LatLng { double lat, lng; };
LatLng at(double east, double north) {
    return {ORIGIN_LAT + north / M_PER_DEG_LAT, ORIGIN_LNG + east / (M_PER_DEG_LAT * std::cos(ORIGIN_LAT * PI / 180.0))};
}

double haversine(LatLng p, LatLng q) {
    auto rad = [](double d) { return d * PI / 180.0; };
    double dLat = rad(q.lat - p.lat), dLng = rad(q.lng - p.lng);
    double h = std::sin(dLat / 2) * std::sin(dLat / 2) + std::cos(rad(p.lat)) * std::cos(rad(q.lat)) * std::sin(dLng / 2) * std::sin(dLng / 2);
    return 2 * 6371000.0 * std::asin(std::sqrt(h));
}

constexpr long long DAY_MS = 24LL * 60 * 60 * 1000;

}  // namespace

bool isDatabaseEmpty(Database& db) {
    auto lock = db.lock();
    auto st = db.prepare("SELECT COUNT(*) FROM nav_nodes");
    st.step();
    return st.getInt(0) == 0;
}

void seedDatabase(Database& db, AuthService& auth, const std::string& demoPassword) {
    // Users first: password hashing is slow and takes its own lock.
    {
        auto lock = db.lock();
        db.exec("DELETE FROM comments; DELETE FROM likes; DELETE FROM posts; DELETE FROM sessions; DELETE FROM hazards;"
                "DELETE FROM users; DELETE FROM nav_edges; DELETE FROM nav_nodes; DELETE FROM places;"
                "DELETE FROM building_floors; DELETE FROM faculty; DELETE FROM events; DELETE FROM contacts;");
    }
    long long admin = auth.createUser("Campus Admin", "admin@campusx.local", demoPassword, "admin");
    long long prof = auth.createUser("Dr. Ananya Rao", "faculty@campusx.local", demoPassword, "faculty");
    long long student = auth.createUser("Riya Student", "student@campusx.local", demoPassword, "student");

    auto lock = db.lock();
    Transaction tx(db);

    std::unordered_map<std::string, const SeedNode*> byId;
    {
        auto st = db.prepare("INSERT INTO nav_nodes (id, name, type, building, floor, x, y, lat, lng) VALUES (?,?,?,?,?,?,?,?,?)");
        for (const SeedNode& n : NODES) {
            byId[n.id] = &n;
            st.reset();
            st.bind(1, n.id).bind(2, n.name).bind(3, n.type);
            if (n.building) {
                st.bind(4, n.building).bind(5, n.floor).bind(6, n.a).bind(7, n.b).bindNull(8).bindNull(9);
            } else {
                LatLng p = at(n.a, n.b);
                st.bindNull(4).bindNull(5).bindNull(6).bindNull(7).bind(8, p.lat).bind(9, p.lng);
            }
            st.run();
        }
    }
    {
        auto st = db.prepare("INSERT INTO nav_edges (from_id, to_id, weight, kind) VALUES (?,?,?,?)");
        for (const SeedEdge& e : EDGES) {
            const SeedNode* a = byId.count(e.from) ? byId[e.from] : nullptr;
            const SeedNode* b = byId.count(e.to) ? byId[e.to] : nullptr;
            if (!a || !b) throw std::runtime_error(std::string("seed edge references missing node: ") + e.from + " -> " + e.to);
            double w = e.weight;
            if (w < 0) {
                bool sameFloor = a->building && b->building && std::string(a->building) == b->building && a->floor == b->floor;
                if (sameFloor) w = std::hypot(a->a - b->a, a->b - b->b);
                else if (!a->building && !b->building) w = haversine(at(a->a, a->b), at(b->a, b->b));
                else throw std::runtime_error(std::string("edge crosses coordinate systems, give a weight: ") + e.from + " -> " + e.to);
            }
            st.reset();
            st.bind(1, e.from).bind(2, e.to).bind(3, std::round(w * 10) / 10).bind(4, e.kind).run();
        }
    }

    // Places
    struct P { const char *name, *type, *code, *desc, *tags, *building; int floor; const char* node; double east, north; bool hasLoc; };
    const P places[] = {
        {"Academic Block A", "building", "ABA", "Classrooms, computer lab and CSE department office.", "block a academic classes", nullptr, -1, "ABA-0-MAIN", 0, 145, true},
        {"Central Library", "library", "LIB", "Reading halls, digital library and book bank.", "books study reading room", nullptr, -1, "OUT-LIB", 90, 150, true},
        {"Medical Centre", "facility", "MED", "First aid and on-campus doctor.", "doctor first aid health hospital", nullptr, -1, "OUT-MED", -90, 20, true},
        {"Computer Lab 1", "lab", "ABA-LAB1", "60-seat programming lab.", "lab computers programming", "ABA", 0, "ABA-0-LAB", 0, 0, false},
        {"Department of Computer Science & Engineering", "department", "CSE", "CSE department office.", "cse computer science hod office", "ABA", 1, "ABA-1-R101", 0, 0, false},
        {"Seminar Hall", "classroom", "ABA-SH", "120-seat seminar hall with projector.", "auditorium talks seminar", "ABA", 1, "ABA-1-R103", 0, 0, false},
        // No nav node on purpose: shows the "searchable but not mapped yet" case.
        {"Main Canteen", "canteen", "CAN", "Food court.", "food mess cafeteria", nullptr, -1, nullptr, 0, 0, false},
    };
    {
        auto st = db.prepare("INSERT INTO places (name, type, code, description, tags, building, floor, nav_node_id, lat, lng) VALUES (?,?,?,?,?,?,?,?,?,?)");
        for (const P& p : places) {
            st.reset();
            st.bind(1, p.name).bind(2, p.type).bind(3, p.code).bind(4, p.desc).bind(5, p.tags);
            if (p.building) st.bind(6, p.building).bind(7, p.floor); else st.bindNull(6).bindNull(7);
            if (p.node) st.bind(8, p.node); else st.bindNull(8);
            if (p.hasLoc) { LatLng l = at(p.east, p.north); st.bind(9, l.lat).bind(10, l.lng); } else st.bindNull(9).bindNull(10);
            st.run();
        }
        db.exec("INSERT INTO building_floors VALUES ('ABA', 0, 'Ground floor', 60, 30), ('ABA', 1, 'First floor', 60, 30);");
    }

    {
        auto st = db.prepare("INSERT INTO faculty (name, designation, department, email, cabin, nav_node_id, research_areas) VALUES (?,?,?,?,?,?,?)");
        st.bind(1, "Dr. Ananya Rao (sample)").bind(2, "Associate Professor").bind(3, "CSE").bind(4, "ananya.rao@example.edu")
            .bind(5, "CSE Department Office, Block A").bind(6, "ABA-1-R101").bind(7, "machine learning computer vision").run();
        st.reset();
        st.bind(1, "Dr. Vikram Negi (sample)").bind(2, "Professor").bind(3, "CSE").bind(4, "vikram.negi@example.edu")
            .bind(5, "Room 102, Block A").bind(6, "ABA-1-R102").bind(7, "networks iot").run();
        st.reset();
        st.bind(1, "Ms. Priya Bisht (sample)").bind(2, "Librarian").bind(3, "Library").bind(4, "priya.bisht@example.edu")
            .bind(5, "Central Library").bind(6, "OUT-LIB").bind(7, "information science").run();
    }

    long long now = nowMs();
    {
        auto st = db.prepare("INSERT INTO events (title, description, venue, nav_node_id, starts_at, ends_at, organizer, tags) VALUES (?,?,?,?,?,?,?,?)");
        st.bind(1, "Hackathon kickoff").bind(2, "24-hour build sprint. Teams of up to four.").bind(3, "Seminar Hall, Block A")
            .bind(4, "ABA-1-R103").bind(5, now + 3 * DAY_MS).bind(6, now + 4 * DAY_MS).bind(7, "Coding Club").bind(8, "hackathon coding club").run();
        st.reset();
        st.bind(1, "Fire safety drill briefing").bind(2, "Walkthrough of exits and assembly points.").bind(3, "Computer Lab 1")
            .bind(4, "ABA-0-LAB").bind(5, now + 7 * DAY_MS).bind(6, now + 7 * DAY_MS + 3600000).bind(7, "Estate & Safety Office").bind(8, "safety drill fire").run();
        st.reset();
        st.bind(1, "Library orientation (past)").bind(2, "Already happened; hidden from search.").bind(3, "Central Library")
            .bind(4, "OUT-LIB").bind(5, now - 10 * DAY_MS).bind(6, now - 10 * DAY_MS + 3600000).bind(7, "Library").bind(8, "orientation").run();
    }

    // Verify national numbers for your state before deploying. Campus numbers are PLACEHOLDERS.
    db.exec(
        "INSERT INTO contacts (name, phone, category, available_24x7, priority) VALUES "
        "('National Emergency (India)', '112', 'general', 1, 1),"
        "('Ambulance', '108', 'medical', 1, 2),"
        "('Fire', '101', 'fire', 1, 3),"
        "('Campus Security (placeholder)', '+910000000000', 'security', 1, 4),"
        "('Campus Medical Centre (placeholder)', '+910000000001', 'medical', 0, 5);");

    {
        auto st = db.prepare("INSERT INTO posts (author_id, content, category, is_official, created_at) VALUES (?,?,?,?,?)");
        st.bind(1, admin).bind(2, "Fire drill in Academic Block A next week. Learn your nearest exit from the Emergency tab. #firedrill #safety")
            .bind(3, "announcement").bind(4, 1).bind(5, now - 300 * 60000).run();
        st.reset();
        st.bind(1, prof).bind(2, "ML lab session moved to Computer Lab 1 this Friday. #ml").bind(3, "academic").bind(4, 1)
            .bind(5, now - 120 * 60000).run();
        st.reset();
        st.bind(1, student).bind(2, "Found a blue water bottle in the Seminar Hall. Collect it from the CSE office. #lostandfound")
            .bind(3, "lost_found").bind(4, 0).bind(5, now - 30 * 60000).run();
        st.reset();
        st.bind(1, student).bind(2, "Anyone forming a team for the hackathon? Need a frontend person. #hackathon #safety")
            .bind(3, "general").bind(4, 0).bind(5, now - 10 * 60000).run();
    }
    tx.commit();
}

}  // namespace cx
