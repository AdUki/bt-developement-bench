# clog: the log colorizer the owner wrote for StreamSDK and brought here (app/clog/README.md).
# Configured by regex groups chosen from the file name (clog.conf: bluez, pipewire, wireplumber,
# wpasupplicant, and nsdk for the rest), which is why btbench-logd names /tmp/logs/* after them.
#
# std::regex: the upstream build uses Qt's (faster), which the board does not have. The config is
# compiled in as well as installed, so the binary works anywhere (the PC included) without one.
cmake_minimum_required(VERSION 3.16)
project(clog CXX)

add_custom_command(
  OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/compiled_in_config.cpp
  COMMAND ${CMAKE_COMMAND} -DIN=${CMAKE_CURRENT_SOURCE_DIR}/clog.conf -DOUT=${CMAKE_CURRENT_BINARY_DIR}/compiled_in_config.cpp
          -P ${CMAKE_CURRENT_SOURCE_DIR}/embed_config.cmake
  DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/clog.conf ${CMAKE_CURRENT_SOURCE_DIR}/embed_config.cmake
  VERBATIM)

add_executable(clog clog.cpp config.cpp filter.cpp items.cpp json_formatter.cpp parser.cpp
                    ${CMAKE_CURRENT_BINARY_DIR}/compiled_in_config.cpp)
target_include_directories(clog PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(clog PRIVATE CONFIG_STRING_VAR=ClogCompiledInConfig)
set_target_properties(clog PROPERTIES CXX_STANDARD 17 CXX_STANDARD_REQUIRED ON)

include(GNUInstallDirs)
install(TARGETS clog DESTINATION ${CMAKE_INSTALL_BINDIR})
install(FILES clog.conf DESTINATION ${CMAKE_INSTALL_DATADIR}/clog)
