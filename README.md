# CampusX (C++ edition)

Campus Search, Campus Feed, Project Partner Matching, Skill Gap & Learning and Emergency Navigation. C++17 backend (cpp-httplib + SQLite), hand-written data structures and algorithms, plain HTML/CSS/JS frontend served by the same program.

```
CampusX/
├── main.cpp             HTTP routes only (thin layer)
├── backend/
│   ├── dsa/             MinHeap, TopK, LRUCache, RateLimiter, SkillGap
│   ├── DSA.rit/         Graph (Dijkstra, BFS), EditDistance, Matcher (partner matching)
│   ├── dsa.ved/         Trie (search)
│   ├── services/        Auth, Search, Learning (skill gap), GeminiClient
│   ├── Services.rit/    NavService (maps + emergency)
│   ├── services.ved/    FeedService, PartnerService
│   ├── database/        SQLite RAII wrapper + schema
│   ├── seed/            SAMPLE campus data
│   └── util/            Json writer/reader, text helpers, SHA-256 / HMAC / PBKDF2
├── Frontend/
│   ├── HTML/            index, login, feed, search, navigate, emergency, partners, skills
│   ├── Js/              one script per page + common.js, mapLayout.js
│   ├── CSS/             style.css (glass theme, dark + light)
│   └── img/             icons.svg, favicon.svg
├── TEST/                test_main.cpp: 37 tests, 2424 checks, no network needed
├── scripts/             setup.sh / setup.ps1: download cpp-httplib + SQLite
└── CMakeLists.txt
```

## Where the DSA is (your viva map)

| Feature | Data structure / algorithm | File | Complexity | Why this choice |
|---|---|---|---|---|
| Search | **Trie** over every word of every place, person and event | `dsa/Trie.cpp` | insert O(L); prefix O(L + matches) | Search-as-you-type: "lib" → Library |
| Search | **Levenshtein DP on the trie** with row-minimum pruning | `Trie::fuzzy` | far below O(words × L²) in practice | Typos: "libary" → Library. Shared prefixes computed once |
| Search | **Top-K with a min-heap** | `dsa/TopK.h` | O(n log k) | Best 200 results without sorting everything |
| Search | **LRU cache** (hash map + doubly linked list) | `dsa/LRUCache.h` | get/put O(1) | Popular queries ("library") skip ranking |
| Feed | **Keyset pagination** on a B-tree index | `FeedService::list` | O(log n + page) per page | No duplicates when new posts arrive; page 100 as fast as page 1 |
| Feed | **Composite primary key** as a set | `likes` table | O(log n) | A user can like a post only once, enforced by the DB |
| Feed | **Hash map + Top-K** for trending | `FeedService::trending` | O(n log k) | Hot posts (time-decayed score) and top #hashtags |
| Partners | **Graph walk** project → skills → students (adjacency lists + hash set for duplicates) | `match::rank` | O(Σ students per required skill) | Only students sharing a required skill get scored |
| Partners | **Bitmask** availability (21 slots) + Kernighan bit count | `match::availabilityScore` | O(set bits) | Shared free time is `popcount(a & b)` |
| Partners | **Hash map** skill → level per student | `match::skillScore` | O(required skills) | Weighted score with partial credit for lower levels |
| Partners | **Max-heap** (MinHeap with a "ranks higher" comparator) | `match::rank` | O(n log n) | Best candidates first; ties: fewer active projects, then lower id |
| Partners | **Greedy** weighted set cover | `match::formTeam` | O(team × candidates × skills) | Balanced team: prefers students who add missing skills. A heuristic, not guaranteed optimal |
| Partners | **Partial unique index** `WHERE status = 'pending'` | `invitations` table | O(log n) | At most one pending invitation per student per project, enforced by the DB |
| Skill gap | **Hash map** skill → level, gap `G = max(0, T − C)`, priority `P = G × weight` | `learn::analyse` | O(required skills) | What to learn first; Gap% = Σ w·G / Σ w·T |
| Skill gap | **Hash set** of title keys + word-set Jaccard ≥ 0.7 | `learn::rankCandidates` | O(n²) on ≤ 10 candidates | Drops duplicate and near-identical video titles from Gemini |
| Skill gap | **Max-heap** ranking, ≤ 3 per skill, top 5 | `learn::rankCandidates` | O(n log n) | R = 0.35 skill + 0.25 level + 0.20 goal + 0.15 topics + 0.05 practical |
| Skill gap | **Recursive-descent JSON parser** | `util/JsonParse.cpp` | O(length) | Reads Gemini's response (depth-limited) |
| Emergency | **Weighted graph**, adjacency lists | `dsa/Graph.cpp` | O(V + E) memory | Indoor + outdoor in one graph; stairs cost extra |
| Emergency | **Dijkstra** with binary heap | `Graph::shortestPath` | O((V + E) log V) | Everyday directions from search results |
| Emergency | **Multi-source Dijkstra on the reversed graph** | `Graph::buildRoutingTable` | one run for ALL nodes | "Nearest exit" for every room at once; the table also works offline on the phone |
| Emergency | **BFS** connected components | `Graph::connectedComponents` | O(V + E) | Finds map-data mistakes (a room nobody can reach) |
| Emergency | **Greedy label placement** + Liang–Barsky line clipping | `Frontend/Js/mapLayout.js` | O(labels × 8 × (labels + nodes + lines)) | Map labels never overlap, never leave the frame, and avoid covering dots and route lines |
| Security | **Sliding-window rate limiter** (hash map of deques) | `dsa/RateLimiter.h` | amortised O(1) | Slows password guessing and spam |
| Security | **SHA-256, HMAC, PBKDF2** | `util/Crypto.cpp` | 120,000 iterations | Passwords are never stored in plain text |

STL containers (`vector`, `unordered_map`, `deque`) are used for storage; the algorithms above are written by hand.

## Build and run

You need a C++17 compiler and CMake 3.16+.

**Linux / macOS**
```bash
./scripts/setup.sh                 # downloads httplib.h + SQLite into third_party/
cmake -S . -B build
cmake --build build -j
./build/campusx_tests              # should print "2424 checks, 0 failures"
./build/campusx                    # run from the project root, then open http://localhost:8080
```
On Ubuntu you can use system SQLite instead: `sudo apt install libsqlite3-dev`, then `./scripts/setup.sh --system-sqlite`.

**Windows (MSYS2 MinGW-w64, recommended)**: install MSYS2, then in the "MSYS2 UCRT64" shell:
```bash
pacman -S mingw-w64-ucrt-x86_64-gcc mingw-w64-ucrt-x86_64-cmake mingw-w64-ucrt-x86_64-ninja unzip
./scripts/setup.sh
cmake -S . -B build -G Ninja && cmake --build build
./build/campusx_tests && ./build/campusx
```
**Windows (Visual Studio 2022)**: in PowerShell, `powershell -ExecutionPolicy Bypass -File scripts\setup.ps1`, then `cmake -S . -B build` and `cmake --build build --config Release`, then run `build\Release\campusx.exe` from the project root.

If a download in the setup script fails, get the two files manually (links in `third_party/README.md`).

**First run** loads the SAMPLE campus automatically. Demo logins (password `ChangeMe@123`): `admin@campusx.local`, `faculty@campusx.local`. Demo student: `aryaman@gmail.com`, password `qwerty123` (its own fixed password; the old `student@campusx.local` login no longer exists, and an older sample database is moved to the new login on startup).

Options: `--seed` (wipe and reload sample data), `--port 8081`, `--db path.db`, `--public path`. Environment: `CAMPUSX_EMAIL_DOMAIN=geu.ac.in` restricts sign-ups to that domain; `CAMPUSX_SEED_PASSWORD` changes the demo password.

**Gemini (Skill Gap page):** put your API key in a file named `gemini.key` in the project root (one line, already in `.gitignore`), or set `GEMINI_API_KEY`. The key stays on the server. Optional: `GEMINI_MODELS=model1,model2` (tried in order; default `gemini-flash-lite-latest,gemini-3.1-flash-lite,gemini-3.8-flash`, which suits free-tier keys). Needs libcurl: built into macOS; Ubuntu `sudo apt install libcurl4-openssl-dev`; MSYS2 `pacman -S mingw-w64-ucrt-x86_64-curl`. Without a key or libcurl the page still works and shows clearly-labelled standard search suggestions. The startup log says `Gemini: on` or why it's off.

**Skill gap demo:** log in as `aryaman@gmail.com` → Skill Gap → pick "Build a Machine Learning project" → see the gap table and % → Get recommendations → Search on YouTube / Save / Mark completed → Progress updates.

**Partner matching demo:** log in as `aryaman@gmail.com` → Find Project Partner → the sample "AI-Based Crop Recommendation System" shows the ranked list and a suggested team that covers every required skill → invite someone → My projects shows the team. Nine SAMPLE student profiles (`*.sample@campusx.local`, same password) are added automatically to a sample database that doesn't have them yet.

**Demo in 2 minutes:** open the dashboard → toggle light/dark → search "libary" (typo on purpose) and click Directions → log in as admin → Emergency → choose Academic Block A, First floor, Room 102 → see the route → open "Manage hazards", block East Stairwell, watch the route switch to the West Stairwell → "Check map health" shows the BFS result.

## API

Requests use the query string or a form body (`application/x-www-form-urlencoded`). Responses are JSON; errors are `{"error": "..."}`. Logged-in calls send `Authorization: Bearer <token>`.

| Method | Path | Who | Purpose |
|---|---|---|---|
| POST | `/api/auth/register` | anyone | `name, email, password` → `{token, user}` (role is always student) |
| POST | `/api/auth/login` | anyone | `email, password` → `{token, user}` |
| POST | `/api/auth/logout` | user | Ends this session |
| GET | `/api/auth/me` | user | Current user |
| GET | `/api/search?q=&kind=all\|place\|faculty\|event&limit=` | anyone | Ranked fuzzy results, `tookMicros`, `cached` |
| POST | `/api/search/reindex` | admin | Rebuild the trie now |
| GET | `/api/feed?cursor=&limit=&category=` | user | Newest first + `nextCursor` |
| POST | `/api/feed` | user | `content, category` (announcement: faculty/admin only) |
| DELETE | `/api/feed/{id}` | owner/admin | Deletes post, comments and likes |
| POST | `/api/feed/{id}/like` | user | Toggle → `{liked, likeCount}` |
| GET / POST | `/api/feed/{id}/comments` | user | List / add (`content`) |
| GET | `/api/feed/trending?k=5` | user | Top-K hot posts and hashtags |
| GET | `/api/partners/meta` | user | Domains, time slots, levels, skill list, scoring weights |
| GET / POST / DELETE | `/api/partners/profile` | user | My partner profile. POST (students only): `department, semester, experience, interests` (domain keys), `availability` (slot numbers 0-20), `skills` (`Python:3,SQL:2`), `bio` |
| GET | `/api/partners/students/{id}` | user | A student's public partner profile (no email) |
| GET | `/api/partners/projects?scope=mine\|open` | user | My projects with team details, or open projects sorted by my fit |
| POST | `/api/partners/projects` | user | `title, description, domain, teamSize, durationWeeks, experience, availability, scheduleRequired, skills` (`name:level:importance:mandatory`), `extra` |
| GET | `/api/partners/projects/{id}` | user | Project, members, skill coverage (+ invitations for the creator) |
| PATCH | `/api/partners/projects/{id}/status` | creator | `status`: in_progress, completed, cancelled, recruiting |
| GET | `/api/partners/projects/{id}/matches?limit=` | creator | Ranked candidates with score breakdown and reasons, suggested balanced team, coverage, exclusion counts |
| POST | `/api/partners/projects/{id}/invite` | creator | `studentId, message` |
| POST | `/api/partners/projects/{id}/request` | student | Ask to join (`message`) |
| POST | `/api/partners/projects/{id}/leave` | member | Leave the team (a full team reopens) |
| GET | `/api/partners/invitations` | user | Received and sent invitations / join requests |
| POST | `/api/partners/invitations/{id}/respond` | receiver / sender | `action`: accept, reject (receiver) or withdraw (sender) |
| GET | `/api/learning/goals` | user | Built-in learning goals and whether Gemini is configured |
| GET | `/api/learning/analysis?goal=` | user | Skill gap rows, gap %, focus order, saved recommendations, progress |
| POST | `/api/learning/recommendations` | user | `goal` → asks Gemini, ranks in C++, stores and returns the top 5 (5 runs per 5 min per IP) |
| POST | `/api/learning/recommendations/{id}/status` | owner | `status`: new, saved, completed |
| GET | `/api/emergency/graph` | anyone | Nodes, edges, hazards, buildings, contacts, routing tables |
| GET | `/api/emergency/route?from=&to=&type=fire\|earthquake\|medical&accessible=1` | anyone | Route + plain-language steps |
| GET | `/api/emergency/nearest?lat=&lng=` | anyone | Snap a GPS fix to the nearest outdoor point |
| POST | `/api/emergency/hazards` | admin | `nodeId, description` (or `edgeFrom, edgeTo`) |
| PATCH | `/api/emergency/hazards/{id}/resolve` | admin | Clear a hazard |
| GET | `/api/emergency/health` | admin | BFS components + nodes that can't reach safety |

## Design decisions

**Emergency navigation**
- Indoor nodes use floor-plan x/y in metres, outdoor nodes use GPS; doorway edges join the two. One flight of stairs costs 20 m.
- Safety rules live in the server, not the UI. Fire and earthquake routes never use lifts. "I can't use stairs" in a fire routes to a **refuge area** instead of returning "no route". Medical routes may use lifts. If nothing is reachable, the app says to shelter in place and call.
- After every hazard change the server runs multi-source Dijkstra once per mode (4 modes) and sends the resulting next-hop tables with the map. **If the network fails, the phone follows the saved table**, so it still gives the same route the server would. A test checks this for all 124 location/mode combinations.
- Indoor location comes from a room picker or a QR code in each room linking to `/emergency.html?from=<nodeId>`. GPS is used outdoors only, because it can't tell floors apart.
- No login is needed for anything emergency-related.

**Search**: the trie is rebuilt from SQLite at startup, every 5 minutes, or on `/reindex`, and swapped in under a write lock while searches keep running on the old one. Ended events are filtered on every request, so the cache can't show them.

**Project partner matching**
- Score = 0.50 skill + 0.25 availability + 0.15 experience + 0.10 interest, each 0-100 (weights in `match::Weights`). Skill score is weighted by importance (1-5); a lower level than wanted earns level/wanted credit.
- Filters before scoring: creator and current members, students missing a **must-have** skill, no shared slot when the schedule is mandatory, students already in 3 active projects. "Must have" applies to every recommended student, so use it sparingly.
- The suggested team is greedy: each round picks the best 0.5 × score + 0.5 × (share of the still-missing skill importance this student adds). With the sample data the top 3 by score all know Python/ML, but the suggested team is Kabir (Data Analysis), Neha (SQL) and Rohan (Frontend): a test checks this.
- Nobody joins without consent: invitations (creator → student) and join requests (student → creator) both need the other side to accept. A declined invitation can't be re-sent. A full team closes the remaining pending invitations.
- Profiles are opt-in (students only); recommendations are recomputed on every request, so profile changes show up immediately.
- Adapted from the feature spec: SQLite instead of MySQL (one database for the whole app); availability is 7 days × morning/afternoon/evening instead of free-form start/end times; "Open" and "Recruiting" are one status. Not built: blocking users, notifications outside the Invitations tab.

**Skill gap and learning**
- Skills come from the student's partner profile (one skill list for the app, levels 1-3: Beginner, Intermediate, Advanced; 0 = not in profile). Goals and their target levels/weights are built into `LearningService` (6 goals). Simplified from the spec: no custom goals, no separate MySQL tables.
- C++ computes the gap and priority; Gemini only suggests up to 10 video titles for the top 3 gap skills; C++ filters (empty, unrelated, above the target level, duplicates, already saved) and ranks them. Only the goal and skill levels are sent to Gemini, no personal data.
- Titles are suggestions, not verified videos: the page links to a YouTube search. Marking a video completed doesn't change the student's level; they update it in their profile.
- If Gemini is busy, rate-limited or not configured, the page says so and shows standard search suggestions ranked by the same engine.

**Feed**: one SQLite connection behind a mutex makes multi-step updates (like toggling) atomic; that's enough at campus scale. Posts are rendered with text nodes only, never `innerHTML`, which blocks script injection from post content.

**Auth**: random 256-bit session tokens; the database stores only their SHA-256, so a stolen database file can't be used to log in. Login takes the same time for unknown emails and wrong passwords.

## Known limitations (tell your mentor before they find them)

1. **All seed data is placeholder:** coordinates, rooms, faculty and two campus phone numbers. Replace with surveyed data and real numbers, and verify 112/108/101 for your state. Never present placeholder emergency numbers as real.
2. **Builds verified on macOS** (Apple clang 21, CMake 4.4, cpp-httplib from `third_party/`) with no warnings under `-Wall -Wextra -Wpedantic`, and on Windows with MinGW (the original `build/` folder). Other compilers are untested; send the exact error if a build fails.
3. **Offline mode is partial.** The map and routing tables are cached after one online visit, but the page itself needs a service worker to open with no network at all. Hazards reported after the cache was saved are not known offline (the page says so).
4. **Floor plans are schematic,** drawn from node coordinates; real plans need an image under the SVG.
5. **Map data is entered in `seed/Seed.cpp`.** A real campus needs an admin editor.
6. Crypto is hand-written for learning (verified against Python's `hashlib`). Production systems should use libsodium or OpenSSL. Use HTTPS (e.g. behind nginx) when deployed, otherwise tokens travel in plain text.
7. Behind a reverse proxy, rate limiting sees the proxy's IP; read `X-Forwarded-For` there.
8. Hazard updates reach phones by 30 s polling, not push.

## Testing

`./build/campusx_tests` covers:
- every data structure against a brute-force reference (heap vs `std::sort`, trie fuzzy vs edit distance on every word, routing table vs single-source Dijkstra from every node)
- SHA-256 and PBKDF2 against reference vectors
- auth: duplicates, wrong passwords, logout, domain restriction
- search: typos, prefixes, multi-word queries, filters, ended events, caching
- feed: idempotent likes, pagination with no gaps or duplicates over 27 posts, roles, cascade deletes, trending
- skill gap: gap/priority/gap% formulas, filtering (duplicates, near-duplicates, unrelated, too advanced, already saved), ranking order and per-skill cap, JSON parser, Gemini request/response and error handling, the full service with a fake Gemini (no network)
- partner matching: spec examples (84.5%, 75%, 66.7%), ranking vs a brute-force sort on random data, greedy team covers missing skills, validation, invitation rules (consent, duplicates, full team, leave, status changes), limits, must-have skills, mandatory schedule
- navigation: no lifts in fire, re-routing around hazards, refuge routing, trapped case, map health

Manual checks before a review: phone tap-to-call works; stop the server and confirm the Emergency page still routes ("Offline copy" pill); try a 2000-character post and a post containing `<script>` (it should show as plain text).

## Questions to prepare for

Answers are in the code comments, so read those first, then try answering without them:
1. Why multi-source Dijkstra on the *reversed* graph instead of running Dijkstra from every room?
2. Why does Dijkstra fail with negative edge weights, and could this graph ever have one?
3. Why a min-heap of size K for the *largest* K items?
4. How does trie pruning keep fuzzy search fast, and what is the worst case?
5. Why keyset pagination instead of `LIMIT ... OFFSET`?
6. Why store a hash of the session token instead of the token?
7. What happens if two people like the same post at the same instant?
8. How would you measure whether this beats the printed evacuation map on each floor? (A drill trial: time students with and without the app.)
