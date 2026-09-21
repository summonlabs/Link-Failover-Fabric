cmake_minimum_required(VERSION 3.20)

project(LinkFailoverFabric
    VERSION 1.0.0
    DESCRIPTION "Authoritative link failover authority runtime"
    HOMEPAGE_URL "https://github.com/summonlabs/Link-Failover-Fabric"
    LANGUAGES CXX)

# ---------------------------------------------------------------------------
# Options
# ---------------------------------------------------------------------------
option(LFF_BUILD_TOOLS        "Build the lff command line tools"                 ON)
option(LFF_BUILD_TESTS        "Build the lff test suites"                        ON)
option(LFF_BUILD_EXAMPLES     "Build the lff examples"                           ON)
option(LFF_BUILD_BENCHMARKS   "Build the lff scale benchmarks"                   ON)
option(LFF_BUILD_SHARED       "Build the runtime as a shared library"            OFF)
option(LFF_WARNINGS_AS_ERRORS "Treat compiler warnings as errors"                ON)
option(LFF_ENABLE_FAULT_INJECTION
       "Compile the crash-point fault injection facility used by the crash-consistency proofs" ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(CTest)

# ---------------------------------------------------------------------------
# Warnings
# ---------------------------------------------------------------------------
# Applies this project's warning policy to one of its own targets. The policy is
# never exported: a downstream consumer of the installed package is not forced
# to adopt these flags.
function(lff_apply_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /permissive- /utf-8 /Zc:__cplusplus /Zc:preprocessor /EHsc
            /wd4251)  # std-container members in exported value types: same toolchain only
        if(LFF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
        if(CMAKE_BUILD_TYPE STREQUAL "Debug")
            # /RTC1 enables stack-frame and uninitialised-variable checks. /RTCc
            # is deliberately not used: it rejects conformant standard library
            # code and would make the Debug build unbuildable.
            target_compile_options(${target} PRIVATE /RTC1)
        endif()
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
            -Wold-style-cast -Wnon-virtual-dtor -Woverloaded-virtual -Wformat=2
            -Wundef -Wcast-align -Wnull-dereference)
        if(LFF_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

# ---------------------------------------------------------------------------
# Runtime library
# ---------------------------------------------------------------------------
set(LFF_SOURCES
    src/version.cpp
    src/outcome.cpp
    src/ids.cpp
    src/hash.cpp
    src/bytes.cpp
    src/evidence.cpp
    src/policy.cpp
    src/authority.cpp
    src/attempt.cpp
    src/decision.cpp
    src/selector.cpp
    src/journal.cpp
    src/store.cpp
    src/engine.cpp
    src/wire.cpp
    src/server.cpp
    src/client.cpp
    src/detail/fsutil.cpp
    src/detail/socket.cpp
    src/detail/fault.cpp
)

if(LFF_BUILD_SHARED)
    add_library(lff SHARED ${LFF_SOURCES})
    target_compile_definitions(lff PUBLIC LFF_SHARED_BUILD)
else()
    add_library(lff STATIC ${LFF_SOURCES})
endif()
add_library(lff::lff ALIAS lff)

target_compile_definitions(lff PRIVATE LFF_BUILDING_LIBRARY)
target_include_directories(lff
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src)
lff_apply_warnings(lff)

if(LFF_ENABLE_FAULT_INJECTION)
    target_compile_definitions(lff PRIVATE LFF_ENABLE_FAULT_INJECTION=1)
endif()

if(WIN32)
    target_link_libraries(lff PUBLIC ws2_32)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(lff PUBLIC Threads::Threads)
endif()

set_target_properties(lff PROPERTIES
    VERSION ${PROJECT_VERSION}
    SOVERSION ${PROJECT_VERSION_MAJOR}
    OUTPUT_NAME lff)

# ---------------------------------------------------------------------------
# Tools, examples, benchmarks
# ---------------------------------------------------------------------------
if(LFF_BUILD_TOOLS)
    foreach(tool lff-fabricd lff-worker lffctl)
        add_executable(${tool} tools/${tool}.cpp src/detail/tool_common.cpp)
        target_link_libraries(${tool} PRIVATE lff::lff)
        lff_apply_warnings(${tool})
        target_include_directories(${tool} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        set_target_properties(${tool} PROPERTIES RUNTIME_OUTPUT_DIRECTORY
            $<TARGET_FILE_DIR:lff>)
    endforeach()
endif()

if(LFF_BUILD_EXAMPLES)
    add_executable(lff-example-minimal examples/minimal_failover.cpp)
    target_link_libraries(lff-example-minimal PRIVATE lff::lff)
    lff_apply_warnings(lff-example-minimal)
    set_target_properties(lff-example-minimal PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        $<TARGET_FILE_DIR:lff>)
endif()

if(LFF_BUILD_BENCHMARKS)
    add_executable(lff-bench benchmarks/bench_fabric.cpp)
    target_link_libraries(lff-bench PRIVATE lff::lff)
    lff_apply_warnings(lff-bench)
    set_target_properties(lff-bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        $<TARGET_FILE_DIR:lff>)
endif()

# ---------------------------------------------------------------------------
# Tests
# ---------------------------------------------------------------------------
if(LFF_BUILD_TESTS)
    enable_testing()
    set(LFF_TEST_SOURCES
        tests/framework.cpp
        tests/support.cpp
        tests/t_unit_ids.cpp
        tests/t_unit_codec.cpp
        tests/t_unit_selector.cpp
        tests/t_unit_engine.cpp
        tests/t_unit_journal.cpp
        tests/t_unit_wire.cpp
        tests/t_adversarial.cpp
        tests/t_property.cpp
        tests/t_concurrency.cpp
        tests/t_integration.cpp
        tests/t_multiprocess.cpp
        tests/t_restart.cpp
        tests/t_scale.cpp
        tests/t_install_consumer.cpp
    )
    add_executable(lff-tests ${LFF_TEST_SOURCES})
    target_link_libraries(lff-tests PRIVATE lff::lff)
    lff_apply_warnings(lff-tests)
    target_include_directories(lff-tests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/src
        ${CMAKE_CURRENT_SOURCE_DIR}/tests)
    target_compile_definitions(lff-tests PRIVATE
        LFF_TOOL_DIR="$<TARGET_FILE_DIR:lff>"
        LFF_SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
        LFF_BINARY_DIR="${CMAKE_CURRENT_BINARY_DIR}"
        LFF_CMAKE_COMMAND="${CMAKE_COMMAND}"
        LFF_GENERATOR="${CMAKE_GENERATOR}"
        LFF_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
    set_target_properties(lff-tests PROPERTIES RUNTIME_OUTPUT_DIRECTORY
        $<TARGET_FILE_DIR:lff>)

    add_test(NAME lff.unit     COMMAND lff-tests --suite unit)
    add_test(NAME lff.protocol COMMAND lff-tests --suite protocol)
    add_test(NAME lff.persistence COMMAND lff-tests --suite persistence)
    add_test(NAME lff.integration COMMAND lff-tests --suite integration)
    add_test(NAME lff.property COMMAND lff-tests --suite property)
    add_test(NAME lff.adversarial COMMAND lff-tests --suite adversarial)
    add_test(NAME lff.concurrency COMMAND lff-tests --suite concurrency)
    add_test(NAME lff.multiprocess COMMAND lff-tests --suite multiprocess)
    add_test(NAME lff.restart  COMMAND lff-tests --suite restart)
    add_test(NAME lff.scale    COMMAND lff-tests --suite scale)
    add_test(NAME lff.consumer COMMAND lff-tests --suite consumer)
endif()

# ---------------------------------------------------------------------------
# Install / package
# ---------------------------------------------------------------------------
install(TARGETS lff
    EXPORT LinkFailoverFabricTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

if(LFF_BUILD_TOOLS)
    install(TARGETS lff-fabricd lff-worker lffctl
        EXPORT LinkFailoverFabricTargets
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

install(DIRECTORY include/lff
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
    FILES_MATCHING PATTERN "*.hpp")

install(EXPORT LinkFailoverFabricTargets
    NAMESPACE lff::
    FILE LinkFailoverFabricTargets.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LinkFailoverFabric)

configure_package_config_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/cmake/LinkFailoverFabricConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/LinkFailoverFabricConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LinkFailoverFabric)

write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/LinkFailoverFabricConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion)

install(FILES
    ${CMAKE_CURRENT_BINARY_DIR}/LinkFailoverFabricConfig.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/LinkFailoverFabricConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/LinkFailoverFabric)
