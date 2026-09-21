// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
#include "framework.hpp"

#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

namespace lff::test {

Registry& Registry::instance() {
    static Registry registry;
    return registry;
}

void Registry::add(std::string suite, std::string name, TestFunction function) {
    entries_.push_back(Entry{std::move(suite), std::move(name), function});
}

Random::Random(std::uint64_t seed) : seed_(seed) {
    // SplitMix64 expansion so that even a zero seed produces a healthy state.
    std::uint64_t x = seed + 0x9E3779B97F4A7C15ull;
    for (std::size_t i = 0; i < 4; ++i) {
        x += 0x9E3779B97F4A7C15ull;
        std::uint64_t z = x;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        state_[i] = z ^ (z >> 31);
    }
}

std::uint64_t Random::rotate_left(std::uint64_t value, int shift) const {
    return (value << shift) | (value >> (64 - shift));
}

std::uint64_t Random::next_u64() {
    const std::uint64_t result = rotate_left(state_[1] * 5, 7) * 9;
    const std::uint64_t t = state_[1] << 17;
    state_[2] ^= state_[0];
    state_[3] ^= state_[1];
    state_[1] ^= state_[2];
    state_[0] ^= state_[3];
    state_[2] ^= t;
    state_[3] = rotate_left(state_[3], 45);
    return result;
}

std::uint32_t Random::next_u32() {
    return static_cast<std::uint32_t>(next_u64() >> 32);
}

std::uint64_t Random::below(std::uint64_t bound) {
    if (bound == 0) {
        return 0;
    }
    return next_u64() % bound;
}

std::uint32_t Random::below32(std::uint32_t bound) {
    if (bound == 0) {
        return 0;
    }
    return static_cast<std::uint32_t>(next_u64() % bound);
}

bool Random::chance(std::uint32_t numerator, std::uint32_t denominator) {
    if (denominator == 0) {
        return false;
    }
    return below32(denominator) < numerator;
}

std::string format_string(const char* format, ...) {
    char buffer[1024];
    va_list args;
    va_start(args, format);
    const int written = std::vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    if (written < 0) {
        return std::string(format);
    }
    return std::string(buffer);
}

std::string format_location(const char* file, int line, const std::string& message) {
    return std::string(file) + ":" + std::to_string(line) + ": " + message;
}

namespace {

struct Options {
    std::vector<std::string> suites;
    std::string filter;
    bool list{false};
};

Options parse_options(const std::vector<std::string>& arguments) {
    Options options;
    for (std::size_t i = 0; i < arguments.size(); ++i) {
        const std::string& token = arguments[i];
        if (token == "--list") {
            options.list = true;
        } else if (token == "--suite" && i + 1 < arguments.size()) {
            options.suites.push_back(arguments[++i]);
        } else if (token.rfind("--suite=", 0) == 0) {
            options.suites.push_back(token.substr(8));
        } else if (token == "--filter" && i + 1 < arguments.size()) {
            options.filter = arguments[++i];
        } else if (token.rfind("--filter=", 0) == 0) {
            options.filter = token.substr(9);
        }
    }
    return options;
}

bool matches(const Options& options, const Registry::Entry& entry) {
    if (!options.suites.empty()) {
        bool found = false;
        for (const std::string& suite : options.suites) {
            if (suite == entry.suite) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    if (!options.filter.empty() && entry.name.find(options.filter) == std::string::npos) {
        return false;
    }
    return true;
}

}  // namespace

int run_all(const std::vector<std::string>& arguments) {
    const Options options = parse_options(arguments);
    const std::vector<Registry::Entry>& entries = Registry::instance().entries();

    if (options.list) {
        for (const Registry::Entry& entry : entries) {
            std::cout << entry.suite << "." << entry.name << "\n";
        }
        return 0;
    }

    std::size_t passed = 0;
    std::size_t failed = 0;
    std::size_t skipped = 0;
    std::size_t selected = 0;
    const auto started = std::chrono::steady_clock::now();

    for (const Registry::Entry& entry : entries) {
        if (!matches(options, entry)) {
            continue;
        }
        ++selected;
        const auto case_started = std::chrono::steady_clock::now();
        try {
            entry.function();
            const auto finished = std::chrono::steady_clock::now();
            const auto micros = std::chrono::duration_cast<std::chrono::microseconds>(
                                    finished - case_started)
                                    .count();
            ++passed;
            std::cout << "PASS " << entry.suite << "." << entry.name << " (" << micros << " us)\n";
        } catch (const CaseSkip& skip) {
            ++skipped;
            std::cout << "SKIP " << entry.suite << "." << entry.name << ": " << skip.message
                      << "\n";
        } catch (const CaseFailure& failure) {
            ++failed;
            std::cout << "FAIL " << entry.suite << "." << entry.name << ": " << failure.message
                      << "\n";
        } catch (const std::exception& error) {
            ++failed;
            std::cout << "FAIL " << entry.suite << "." << entry.name
                      << ": unexpected exception: " << error.what() << "\n";
        } catch (...) {
            ++failed;
            std::cout << "FAIL " << entry.suite << "." << entry.name
                      << ": unexpected non-standard exception\n";
        }
        std::cout.flush();
    }

    const auto finished = std::chrono::steady_clock::now();
    const auto millis =
        std::chrono::duration_cast<std::chrono::milliseconds>(finished - started).count();
    std::cout << "summary selected=" << selected << " passed=" << passed << " failed=" << failed
              << " skipped=" << skipped << " elapsed_ms=" << millis << "\n";
    std::cout.flush();
    return failed == 0 ? 0 : 1;
}

}  // namespace lff::test

int main(int argc, char** argv) {
    std::vector<std::string> arguments(argv + 1, argv + argc);
    return lff::test::run_all(arguments);
}
