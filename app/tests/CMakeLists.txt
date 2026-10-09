# Plain executables using the CHECK/CHECK_EQ macros in check.h (no framework); add one with
# add_btb_test(name). Each covers a part that is a decision rather than traffic: the D-Bus side
# is exercised against tools/fake-bluez (make run BT=fake), which a unit test cannot stand up.
function(add_btb_test name)
  add_executable(${name} ${name}.cpp)
  target_link_libraries(${name} PRIVATE btbench_core)
  add_test(NAME ${name} COMMAND ${name})
endfunction()

add_btb_test(test_strings)
add_btb_test(test_ws_hub)
add_btb_test(test_uuids)
add_btb_test(test_codec)
add_btb_test(test_bluez_model)
add_btb_test(test_bluetooth)
add_btb_test(test_le)
add_btb_test(test_scripts)
add_btb_test(test_exec)
# The UUID table is read from the source tree, never copied.
target_compile_definitions(test_uuids PRIVATE BTB_WWW_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../www")
target_compile_definitions(test_bluez_model PRIVATE BTB_WWW_DIR="${CMAKE_CURRENT_SOURCE_DIR}/../www")
