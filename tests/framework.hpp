// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// A deliberately small test framework. It has no third-party dependency, no
// timeouts and no retries: a hanging test is a defect that must be diagnosed,
// not masked behind a watchdog.
#pragma once

#include <cstdint>
#include <exception>
#include <string>
#include <vector>

namespace lff::test {

struct CaseFailure : std::exception {
    explicit CaseFailure(std::string text) : message(std::move(text)) {}
    const char* what() const noexcept override { return message.c_str(); }
    std::string message;
};

struct CaseSkip : std::exception {
    explicit CaseSkip(std::string text) : message(std::move(text)) {}
    const char* what() const noexcept override { return message.c_str(); }
    std::string message;
};

using TestFunction = void (*)();

class Registry {
public:
    struct Entry {
        std::string suite;
        std::string name;
        TestFunction function;
    };

    static Registry& instance();
    void add(std::string suite, std::string name, TestFunction function);
    const std::vector<Entry>& entries() const noexcept { return entries_; }

private:
    std::vector<Entry> entries_;
};

struct Registrar {
    Registrar(const char* suite, const char* name, TestFunction function) {
        Registry::instance().add(suite, name, function);
    }
};

// Deterministic pseudo-random generator: xoshiro256**. Every property test
// prints the seed it used, and every failing case can be reproduced by passing
// that seed back.
class Random {
public:
    explicit Random(std::uint64_t seed);
    std::uint64_t next_u64();
    std::uint32_t next_u32();
    std::uint64_t below(std::uint64_t bound);
    std::uint32_t below32(std::uint32_t bound);
    bool chance(std::uint32_t numerator, std::uint32_t denominator);
    std::uint64_t seed() const noexcept { return seed_; }

private:
    std::uint64_t rotate_left(std::uint64_t value, int shift) const;
    std::uint64_t seed_;
    std::uint64_t state_[4];
};

std::string format_string(const char* format, ...);

// Builds "<file>:<line>: <message>" without passing a std::string through a
// variadic argument list.
std::string format_location(const char* file, int line, const std::string& message);

int run_all(const std::vector<std::string>& arguments);

}  // namespace lff::test

#define LFF_TEST(suite, name)                                                          \
    static void lff_test_case_##suite##_##name();                                      \
    static const ::lff::test::Registrar lff_test_registrar_##suite##_##name(           \
        #suite, #name, &lff_test_case_##suite##_##name);                               \
    static void lff_test_case_##suite##_##name()

#define LFF_FAIL(message)                                                              \
    throw ::lff::test::CaseFailure(                                                    \
        ::lff::test::format_location(__FILE__, __LINE__, std::string(message)))

#define LFF_CHECK(condition)                                                           \
    do {                                                                               \
        if (!(condition)) {                                                            \
            LFF_FAIL("check failed: " #condition);                                     \
        }                                                                              \
    } while (false)

#define LFF_CHECK_EQ(actual, expected)                                                 \
    do {                                                                               \
        const auto& lff_actual = (actual);                                             \
        const auto& lff_expected = (expected);                                         \
        if (!(lff_actual == lff_expected)) {                                           \
            LFF_FAIL(std::string("check failed: " #actual " == " #expected));          \
        }                                                                              \
    } while (false)

#define LFF_CHECK_MSG(condition, message)                                              \
    do {                                                                               \
        if (!(condition)) {                                                            \
            LFF_FAIL(std::string("check failed: " #condition " — ") + (message));      \
        }                                                                              \
    } while (false)

#define LFF_SKIP(message) throw ::lff::test::CaseSkip(message)
