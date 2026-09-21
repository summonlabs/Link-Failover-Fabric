// Copyright 2026 Summon Software Labs.
// Licensed under the Apache License, Version 2.0.
//
// Install and downstream-consumer proof. The package is installed into a
// throwaway prefix, an independent project *outside* the source tree is
// configured with find_package(... CONFIG REQUIRED), built against that prefix,
// and executed. Nothing from the build tree is on its include or library path.
#include "framework.hpp"
#include "support.hpp"

#include <string>
#include <vector>

// The declarations under test live in lff / lff::test; the cases themselves are
// registered at global scope, so the directives are repeated here.
using namespace lff;
using namespace lff::test;

namespace {

using namespace lff;

void copy_tree(const std::filesystem::path& from, const std::filesystem::path& to) {
    std::error_code error;
    std::filesystem::create_directories(to, error);
    for (const auto& entry : std::filesystem::recursive_directory_iterator(from, error)) {
        const std::filesystem::path relative = std::filesystem::relative(entry.path(), from, error);
        const std::filesystem::path target = to / relative;
        if (entry.is_directory()) {
            std::filesystem::create_directories(target, error);
        } else {
            std::filesystem::create_directories(target.parent_path(), error);
            std::filesystem::copy_file(entry.path(), target,
                                       std::filesystem::copy_options::overwrite_existing, error);
        }
    }
}

std::string require_cmake() {
#if defined(LFF_CMAKE_COMMAND)
    return std::string(LFF_CMAKE_COMMAND);
#else
    return std::string("cmake");
#endif
}

}  // namespace

LFF_TEST(consumer, installed_package_is_consumable_outside_the_source_tree) {
    TempDir workspace("consumer");
    const std::filesystem::path prefix = workspace.file("prefix");
    const std::filesystem::path build = workspace.file("build");
    const std::filesystem::path project = workspace.file("downstream");

    // 1. Install the built package into a clean prefix.
    const ProcessResult installed =
        run({require_cmake(), "--install", std::string(LFF_BINARY_DIR), "--prefix",
             prefix.string()});
    LFF_CHECK_MSG(installed.started && installed.exit_code == 0,
                  "cmake --install failed: " + installed.output);
    LFF_CHECK(file_exists(prefix / "lib" / "cmake" / "LinkFailoverFabric" /
                          "LinkFailoverFabricConfig.cmake"));
    LFF_CHECK(file_exists(prefix / "include" / "lff" / "engine.hpp"));
    LFF_CHECK(file_exists(prefix / "include" / "lff" / "server.hpp"));

    // 2. Copy the consumer project outside the source tree.
    copy_tree(std::filesystem::path(LFF_SOURCE_DIR) / "consumer", project);
    LFF_CHECK(file_exists(project / "CMakeLists.txt"));
    LFF_CHECK(file_exists(project / "main.cpp"));
    LFF_CHECK(!file_exists(project / ".." / "CMakeLists.txt"));

    // 3. Configure against the installed prefix only.
    // The downstream project must be built with the same configuration as the
    // artefacts it links: mixing /MD and /MDd fails at link time by design.
    std::string build_type = "Release";
#if defined(LFF_BUILD_TYPE)
    if (std::string(LFF_BUILD_TYPE).size() > 0) {
        build_type = std::string(LFF_BUILD_TYPE);
    }
#endif
    std::vector<std::string> configure = {require_cmake(),
                                          "-S", project.string(),
                                          "-B", build.string(),
                                          "-DCMAKE_BUILD_TYPE=" + build_type,
                                          "-DCMAKE_PREFIX_PATH=" + prefix.string()};
#if defined(LFF_GENERATOR)
    if (std::string(LFF_GENERATOR).find("Ninja") != std::string::npos ||
        std::string(LFF_GENERATOR).find("Makefiles") != std::string::npos) {
        configure.push_back("-G");
        configure.push_back(std::string(LFF_GENERATOR));
    }
#endif
    const ProcessResult configured = run(configure);
    LFF_CHECK_MSG(configured.started && configured.exit_code == 0,
                  "downstream configure failed: " + configured.output);

    // 4. Build and run it.
    const ProcessResult built = run({require_cmake(), "--build", build.string()});
    LFF_CHECK_MSG(built.started && built.exit_code == 0,
                  "downstream build failed: " + built.output);

    std::filesystem::path executable = build / "lff-consumer";
#if defined(_WIN32)
    executable += ".exe";
#endif
    LFF_CHECK(file_exists(executable));
    const ProcessResult ran = run({executable.string(), workspace.file("state").string()});
    LFF_CHECK_MSG(ran.started && ran.exit_code == 0,
                  "downstream consumer failed: " + ran.output);
    LFF_CHECK_MSG(ran.output.find("decision=Grant") != std::string::npos,
                  "consumer output: " + ran.output);
    LFF_CHECK(ran.output.find("closed=ok") != std::string::npos);
    LFF_CHECK(ran.output.find("version=1.0.0") != std::string::npos);
}