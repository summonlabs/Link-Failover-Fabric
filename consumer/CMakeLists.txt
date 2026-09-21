cmake_minimum_required(VERSION 3.20)
project(LinkFailoverFabricConsumer LANGUAGES CXX)

# An independent downstream project. It knows nothing about the Link Failover
# Fabric source tree: it only consumes the installed package.
find_package(LinkFailoverFabric CONFIG REQUIRED)

add_executable(lff-consumer main.cpp)
target_link_libraries(lff-consumer PRIVATE lff::lff)
set_target_properties(lff-consumer PROPERTIES CXX_STANDARD 20 CXX_STANDARD_REQUIRED ON)

message(STATUS "LinkFailoverFabric consumer configured against ${LinkFailoverFabric_DIR}")
