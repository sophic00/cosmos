cmake_minimum_required(VERSION 3.25)
project(cosmos VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(COSMOS_BUILD_TESTS "Build Cosmos unit tests" ON)

# SiteId and FaultKind are append-only, and every switch over them must stay total: a new site
# that nobody adds to site_slot()/class_of() would silently become "unknown" forever and could
# never hold a rule. A warning is not enough to catch that.
if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    add_compile_options(-Werror=switch)
endif()

add_subdirectory(src/cosmos)
add_subdirectory(examples)

if(COSMOS_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
