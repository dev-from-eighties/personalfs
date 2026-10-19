cmake_minimum_required(VERSION 3.24)
project(personalfs VERSION 2.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

add_library(personalfs_core
    src/storage/Format.cpp
    src/storage/Store.cpp
)

target_include_directories(personalfs_core PUBLIC
    src
    ${FUSE3_INCLUDE_DIRS}
)

target_link_libraries(personalfs_core PUBLIC
    ${FUSE3_LIBRARIES}
)

target_compile_options(personalfs_core PRIVATE
    ${FUSE3_CFLAGS_OTHER}
    -Wall -Wextra -Wpedantic
)

add_executable(personalfs src/main.cpp)
target_sources(personalfs PRIVATE src/fuse/FuseMain.cpp)
target_link_libraries(personalfs PRIVATE personalfs_core)
target_compile_options(personalfs PRIVATE -Wall -Wextra -Wpedantic)
find_package(Threads REQUIRED)
target_link_libraries(personalfs_core PUBLIC Threads::Threads)
target_compile_definitions(personalfs_core PUBLIC _FILE_OFFSET_BITS=64)
include(CTest)
if(BUILD_TESTING)
    add_executable(personalfs_tests tests/StoreTests.cpp)
    add_library(personalfs_test_core src/storage/Format.cpp src/storage/Store.cpp)
    target_include_directories(personalfs_test_core PUBLIC src)
    target_compile_definitions(personalfs_test_core PRIVATE PERSONALFS_TESTING=1)
    target_compile_definitions(personalfs_test_core PUBLIC _FILE_OFFSET_BITS=64)
    target_compile_options(personalfs_test_core PRIVATE -Wall -Wextra -Wpedantic)
    target_link_libraries(personalfs_test_core PUBLIC Threads::Threads)
    target_link_libraries(personalfs_tests PRIVATE personalfs_test_core)
    target_compile_options(personalfs_tests PRIVATE -Wall -Wextra -Wpedantic)
    add_test(NAME v2_store COMMAND personalfs_tests)
    add_test(NAME v2_crash_boundaries COMMAND personalfs_tests --exhaustive)
    set_tests_properties(v2_crash_boundaries PROPERTIES TIMEOUT 600)
    find_package(Python3 COMPONENTS Interpreter REQUIRED)
    add_test(NAME v2_cli COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/CliTests.py $<TARGET_FILE:personalfs>)
endif()

