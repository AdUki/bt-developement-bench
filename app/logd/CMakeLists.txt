# btbench-logd (logd.h): the journal as one native-format text file per program, for clog.
# The formatting is a library of its own so tests can link it without the journal loop.
add_library(btb_logd STATIC format.cpp)
target_include_directories(btb_logd PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_options(btb_logd PRIVATE -Wall -Wextra)

add_executable(btbench-logd main.cpp)
target_link_libraries(btbench-logd PRIVATE btb_logd third_party PkgConfig::SYSTEMD)
target_compile_options(btbench-logd PRIVATE -Wall -Wextra)
install(TARGETS btbench-logd DESTINATION ${CMAKE_INSTALL_BINDIR})
