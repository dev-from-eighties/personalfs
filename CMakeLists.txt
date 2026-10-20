cmake_minimum_required(VERSION 3.24)
project(personalfs LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(PkgConfig REQUIRED)
pkg_check_modules(FUSE3 REQUIRED fuse3)

add_library(personalfs_core
    src/app/App.cpp
    src/app/Config.cpp
    src/fuse/FuseMain.cpp
    src/fuse/Operations.cpp
    src/storage/BackendPaths.cpp
    src/storage/ImageManager.cpp
    src/util/Command.cpp
    src/util/Error.cpp
    src/util/Logger.cpp
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
target_link_libraries(personalfs PRIVATE personalfs_core)

include(CTest)
if(BUILD_TESTING)
    add_executable(shrink_regression tests/ShrinkRegression.cpp)
    target_link_libraries(shrink_regression PRIVATE personalfs_core)
    target_link_options(shrink_regression PRIVATE -Wl,--wrap=ioctl -Wl,--wrap=statvfs)
    add_test(NAME shrink_regression COMMAND shrink_regression)
endif()
