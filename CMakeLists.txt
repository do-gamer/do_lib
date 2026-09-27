cmake_minimum_required(VERSION 3.10)

project (DarkTanos)

set(API_VERSION 12)

set(CMAKE_CXX_STANDARD 17)
# Default to an optimized build; previously this unconditionally forced Debug (-O0),
# silently overriding -DCMAKE_BUILD_TYPE=Release passed by build.sh.
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_INCLUDE_CURRENT_DIR ON)
set(SUBHOOK_STATIC ON)
set(SUBHOOK_TESTS OFF)

include_directories(${CMAKE_SOURCE_DIR}/tools)

# Portability across distributions: don't depend on the build host's libstdc++/libgcc
# version (GLIBCXX_3.4.xx), and keep their symbols out of our export tables.
add_link_options(-static-libstdc++ -static-libgcc -Wl,--exclude-libs,ALL -Wl,-z,noexecstack)

add_subdirectory(client/)
add_subdirectory(do_lib/)
add_subdirectory(third_party/)

option(BUILD_TESTS "Build native tests" OFF)
if(BUILD_TESTS)
    add_subdirectory(tests/)
endif()
