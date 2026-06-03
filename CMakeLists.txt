cmake_minimum_required(VERSION 3.16)
project(mdsys VERSION 0.2.3 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Curses REQUIRED)
find_package(Threads REQUIRED)

add_executable(mdsys src/main.cpp)
target_compile_definitions(mdsys PRIVATE MDSYS_VERSION="${PROJECT_VERSION}")
target_include_directories(mdsys PRIVATE ${CURSES_INCLUDE_DIR})
target_link_libraries(mdsys PRIVATE ${CURSES_LIBRARIES} Threads::Threads)

# ── Install ──────────────────────────────────────────────────────────────────
include(GNUInstallDirs)
install(TARGETS mdsys RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

# ── CPack / DEB ──────────────────────────────────────────────────────────────
set(CPACK_GENERATOR "DEB")
set(CPACK_PACKAGE_NAME            "mdsys")
set(CPACK_PACKAGE_VERSION         "${PROJECT_VERSION}")
set(CPACK_PACKAGE_CONTACT         "hdmain <noreply@github.com>")
set(CPACK_PACKAGE_DESCRIPTION_SUMMARY
    "TUI systemd service manager for Linux/WSL")
set(CPACK_PACKAGE_DESCRIPTION
    "mdsys is a terminal UI manager for systemd user and system services.\n"
    "Features: service list with RAM usage, pin/unpin, restart/start/stop,\n"
    "live log viewer via journalctl, system/user mode toggle.")

set(CPACK_DEBIAN_PACKAGE_SECTION    "admin")
set(CPACK_DEBIAN_PACKAGE_PRIORITY   "optional")
set(CPACK_DEBIAN_PACKAGE_DEPENDS    "libncurses6 (>= 6), systemd, less")
set(CPACK_DEBIAN_PACKAGE_ARCHITECTURE "amd64")
set(CPACK_DEBIAN_FILE_NAME          "DEB-DEFAULT")

include(CPack)
