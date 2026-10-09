cmake_minimum_required(VERSION 3.16)
project(btbenchd CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
# clangd and the editor read this; `make btbenchd` links it to app/compile_commands.json.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# The Zero W is one ARM11 core with 512 MB: -O2 rather than -O3 (nothing here is a hot loop, and
# the binary stays smaller), and -DNDEBUG repeated because overriding the flags drops CMake's
# default — without it every assert in the header-only third-party libraries stays in the binary.
set(CMAKE_CXX_FLAGS_RELEASE "-O2 -DNDEBUG")

find_package(PkgConfig REQUIRED)
# sd-bus for BlueZ's D-Bus API, sd-journal for the journal follow. libsystemd is on every image
# (systemd is the init). On a dev box:  sudo apt install libsystemd-dev
pkg_check_modules(SYSTEMD REQUIRED IMPORTED_TARGET libsystemd)
find_package(Threads REQUIRED)

# third_party/* are git submodules pinned at a commit. All header-only: nothing here is built or
# linked, this just points the include path into their trees.
if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib/httplib.h)
  # The command goes on its own line: message() hard-wraps, and wrapping it mid-command would hand
  # the reader something that does not copy-paste.
  message(FATAL_ERROR "third_party submodules are empty. Run:\n"
                      "    git submodule update --init\n")
endif()

# CLI11 publishes the amalgamated CLI11.hpp only as a release asset, so the submodule carries the
# multi-header include/CLI layout. file(GENERATE), not file(WRITE): WRITE re-touches the header on
# every configure and main.cpp (which pulls all of CLI11) would rebuild each time for nothing.
set(CLI11_SHIM ${CMAKE_CURRENT_BINARY_DIR}/cli11-shim)
file(GENERATE OUTPUT ${CLI11_SHIM}/CLI11.hpp CONTENT "#include <CLI/CLI.hpp>\n")

# Render the API reference (docs/api.md) to HTML at build time and drop it into www/, so GET /api
# serves a pre-rendered page and only the HTML ships to the board. The converter is md2html: in a
# Yocto build it comes from md2html-native (on PATH); in a desktop build it is the repo script.
set(BTB_API_MD ${CMAKE_CURRENT_SOURCE_DIR}/../docs/api.md)
if(NOT EXISTS ${BTB_API_MD})
  message(FATAL_ERROR "GET /api renders docs/api.md, but it is not at ${BTB_API_MD}.\n"
                      "In a Yocto build, btbenchd_1.0.bb must have this in SRC_URI:\n"
                      "    file://docs\n")
endif()
find_program(MD2HTML NAMES md2html)
if(MD2HTML)
  set(MD2HTML_CMD ${MD2HTML})
  set(MD2HTML_DEP ${MD2HTML})
else()
  find_program(PYTHON3 NAMES python3 python REQUIRED)
  set(MD2HTML_TOOL ${CMAKE_CURRENT_SOURCE_DIR}/../tools/md2html)
  set(MD2HTML_CMD ${PYTHON3} ${MD2HTML_TOOL})
  set(MD2HTML_DEP ${MD2HTML_TOOL})
endif()
# Generated into the source www/ (git-ignored) rather than the build tree, so `make deploy-www`
# — which copies app/www — carries it to a running board.
set(BTB_API_HTML ${CMAKE_CURRENT_SOURCE_DIR}/www/api.html)
add_custom_command(
  OUTPUT ${BTB_API_HTML}
  COMMAND ${MD2HTML_CMD} ${BTB_API_MD} ${BTB_API_HTML} --title "btbenchd HTTP API"
  DEPENDS ${BTB_API_MD} ${MD2HTML_DEP}
  COMMENT "Rendering docs/api.md -> www/api.html"
  VERBATIM)
add_custom_target(api_html ALL DEPENDS ${BTB_API_HTML})

add_library(third_party INTERFACE)
target_include_directories(third_party SYSTEM INTERFACE
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/httplib
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/json/single_include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/spdlog/include
  ${CMAKE_CURRENT_SOURCE_DIR}/third_party/cli11/include
  ${CLI11_SHIM}
)
# Header-only spdlog: no library to link, no fmt to build. CLI11 is header-only as long as
# CLI11_COMPILE stays undefined. The thread pool's floor is set in webserver.cpp; this is only
# httplib's own default, kept as small as the board for anything that builds a server without it.
target_compile_definitions(third_party INTERFACE SPDLOG_HEADER_ONLY CPPHTTPLIB_THREAD_POOL_COUNT=3)

add_library(btbench_core STATIC
  src/a2dp_codec.cpp
  src/adv.cpp
  src/api_bluetooth.cpp
  src/api_le.cpp
  src/api_system.cpp
  src/bluetooth.cpp
  src/bluez_model.cpp
  src/dbus_util.cpp
  src/gatt_server.cpp
  src/jobs.cpp
  src/journal.cpp
  src/scripts.cpp
  src/system.cpp
  src/util/exec.cpp
  src/util/log.cpp
  src/util/strings.cpp
  src/util/sysinfo.cpp
  src/uuids.cpp
  src/webserver.cpp
  src/ws_hub.cpp
)
target_include_directories(btbench_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_link_libraries(btbench_core PUBLIC third_party PkgConfig::SYSTEMD Threads::Threads)
target_compile_options(btbench_core PRIVATE -Wall -Wextra)

# The HCI monitor (src/hci/, its own area: docs/contracts.md) is a subdirectory building btb_hci.
# Guarded so the daemon builds before it lands; BTB_HAVE_HCI tells main.cpp to wire it in.
if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/src/hci/CMakeLists.txt)
  add_subdirectory(src/hci)
  set(BTB_HAVE_HCI ON)
  message(STATUS "HCI monitor: src/hci")
else()
  set(BTB_HAVE_HCI OFF)
  message(STATUS "HCI monitor: src/hci not present, built without it")
endif()

add_executable(btbenchd src/main.cpp)
target_link_libraries(btbenchd PRIVATE btbench_core)
if(BTB_HAVE_HCI)
  target_link_libraries(btbenchd PRIVATE btb_hci)
  target_compile_definitions(btbenchd PRIVATE BTB_HAVE_HCI=1)
endif()

include(GNUInstallDirs)
install(TARGETS btbenchd RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
# api.html is generated into www/ before install runs (the api_html target is part of ALL).
install(DIRECTORY www/ DESTINATION ${CMAKE_INSTALL_DATADIR}/btbenchd/www)

# Off in the Yocto build: the tests run on the build host (make test), never on the board.
option(BTB_TESTS "Build the unit tests" ON)
if(BTB_TESTS)
  enable_testing()
  add_subdirectory(tests)
  if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/hci/CMakeLists.txt AND BTB_HAVE_HCI)
    add_subdirectory(tests/hci)
  endif()
endif()
