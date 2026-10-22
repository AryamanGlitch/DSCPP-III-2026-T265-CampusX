// CampusX test runner. Build target `campusx_tests`, run it,
// exit code 0 = all passed. The tests themselves live in each owner's
// tests/ folder and register themselves through TEST() in TestHarness.h.
#include "TestHarness.h"

int main() {
    for (auto& [name, fn] : registry()) {
        int before = g_failures;
        try {
            fn();
        } catch (const std::exception& e) {
            ++g_failures;
            std::cerr << "  EXCEPTION in " << name << ": " << e.what() << "\n";
        }
        std::cout << (g_failures == before ? "PASS  " : "FAIL  ") << name << "\n";
    }
    std::cout << "\n" << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}
