# btb_hci: the HCI monitor (docs/monitor.md). A static library with one public header,
# hci_monitor.h; btbenchd links it and calls btb::hci::start() / register_routes().
#
# Header-only dependencies only (nlohmann/json, cpp-httplib, spdlog) plus pthreads. Inside the
# btbenchd tree the parent's `third_party` interface target supplies them — the same include
# paths and, importantly, the same compile definitions (SPDLOG_HEADER_ONLY,
# CPPHTTPLIB_THREAD_POOL_COUNT), so every translation unit sees one httplib. Built on its own (a
# scratch harness), BTB_THIRD_PARTY points at a directory with those three checkouts.

cmake_minimum_required(VERSION 3.16)

add_library(btb_hci STATIC
  btsnoop.cpp
  capture.cpp
  codec.cpp
  decoder.cpp
  dissect.cpp
  monitor.cpp
  packet_view.cpp
  pcap.cpp
  routes.cpp
)
set_target_properties(btb_hci PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)
target_include_directories(btb_hci PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
if(TARGET third_party)
  target_link_libraries(btb_hci PUBLIC third_party Threads::Threads)
else()
  if(NOT DEFINED BTB_THIRD_PARTY)
    set(BTB_THIRD_PARTY ${CMAKE_CURRENT_SOURCE_DIR}/../../third_party)
  endif()
  # json: single_include/ as the daemon uses; include/ (the multi-header tree) works as well.
  if(EXISTS ${BTB_THIRD_PARTY}/json/single_include/nlohmann/json.hpp)
    set(BTB_JSON_INCLUDE ${BTB_THIRD_PARTY}/json/single_include)
  else()
    set(BTB_JSON_INCLUDE ${BTB_THIRD_PARTY}/json/include)
  endif()
  target_include_directories(btb_hci SYSTEM PUBLIC
    ${BTB_JSON_INCLUDE}
    ${BTB_THIRD_PARTY}/httplib
    ${BTB_THIRD_PARTY}/spdlog/include
  )
  target_compile_definitions(btb_hci PUBLIC SPDLOG_HEADER_ONLY CPPHTTPLIB_THREAD_POOL_COUNT=3)
  target_link_libraries(btb_hci PUBLIC Threads::Threads)
endif()

# 64-bit file offsets on 32-bit ARM for the files that read the capture directory, so
# readdir()/stat()/pread() never fail with EOVERFLOW. Only there, and deliberately not for the
# whole library: anything that includes httplib.h must see the same off_t/time_t as the daemon's
# own translation units, or the header-only Server/Request layouts differ between them (ODR).
# packet_view.h, which routes.cpp includes, therefore holds no off_t: offsets in it are uint64_t.
# (scarthgap's SDK already passes -D_FILE_OFFSET_BITS=64 -D_TIME_BITS=64 to everything.)
set_source_files_properties(capture.cpp btsnoop.cpp packet_view.cpp PROPERTIES COMPILE_DEFINITIONS
                            _FILE_OFFSET_BITS=64)

option(BTB_HCI_WERROR "Build btb_hci with -Werror" OFF)
target_compile_options(btb_hci PRIVATE -Wall -Wextra $<$<BOOL:${BTB_HCI_WERROR}>:-Werror>)

# The tests live in app/tests/hci. btbenchd's app/CMakeLists.txt adds that directory itself;
# a standalone build (no parent) can ask for them here.
option(BTB_HCI_TESTS "Also build app/tests/hci from here (standalone builds)" OFF)
if(BTB_HCI_TESTS AND BUILD_TESTING)
  add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../tests/hci ${CMAKE_CURRENT_BINARY_DIR}/tests)
endif()
