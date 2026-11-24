#pragma once
// Minimal test framework without external dependencies.
// As the project grows, you can switch to GoogleTest or Catch2.

#include <cstdio>
#include <vector>

namespace peewee::test {

struct Failure {};
struct Skipped {};

struct Case {
    const char* name;
    void (*fn)();
};

inline std::vector<Case>& registry() {
    static std::vector<Case> r;
    return r;
}

struct Registrar {
    Registrar(const char* name, void (*fn)()) { registry().push_back({name, fn}); }
};

inline int run_all() {
    int passed = 0, failed = 0, skipped = 0;
    for (const Case& c : registry()) {
        try {
            c.fn();
            std::printf("[  OK  ] %s\n", c.name);
            ++passed;
        } catch (const Skipped&) {
            std::printf("[ SKIP ] %s\n", c.name);
            ++skipped;
        } catch (const Failure&) {
            std::printf("[ FAIL ] %s\n", c.name);
            ++failed;
        }
    }
    std::printf("\n%d ok, %d failed, %d skipped\n", passed, failed, skipped);
    return failed == 0 ? 0 : 1;
}

}  // namespace peewee::test

#define TEST(name)                                                         \
    static void name();                                                    \
    static ::peewee::test::Registrar registrar_##name(#name, &name);       \
    static void name()

#define CHECK(cond)                                                                        \
    do {                                                                                   \
        if (!(cond)) {                                                                     \
            std::printf("    %s:%d: CHECK(%s) failed\n", __FILE__, __LINE__, #cond);      \
            throw ::peewee::test::Failure{};                                               \
        }                                                                                  \
    } while (0)

#define CHECK_EQ(a, b) CHECK((a) == (b))

// Mark a test as pending implementation: remove this line when ready.
#define SKIP_UNTIL_IMPLEMENTED() throw ::peewee::test::Skipped{}

#define TEST_MAIN() \
    int main() { return ::peewee::test::run_all(); }
