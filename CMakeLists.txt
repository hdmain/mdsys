cmake_minimum_required(VERSION 3.16)
project(mdsys VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Curses REQUIRED)

add_executable(mdsys
    src/main.cpp
)

target_include_directories(mdsys PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(mdsys PRIVATE ${CURSES_LIBRARIES})
