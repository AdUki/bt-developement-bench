# Tests of the HCI monitor (app/src/hci). Added by btbenchd's app/CMakeLists.txt, or by
# app/src/hci with -DBTB_HCI_TESTS=ON in a standalone build. ctest names: hci_*.

function(add_hci_test name)
  add_executable(test_${name} test_${name}.cpp)
  target_link_libraries(test_${name} PRIVATE btb_hci)
  target_include_directories(test_${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_options(test_${name} PRIVATE -Wall -Wextra $<$<BOOL:${BTB_HCI_WERROR}>:-Werror>)
  add_test(NAME ${name} COMMAND test_${name})
  set_tests_properties(${name} PROPERTIES TIMEOUT 60)
endfunction()

add_hci_test(hci_decode)
add_hci_test(hci_pcap)
add_hci_test(hci_capture)
add_hci_test(hci_replay)

# Not a test: runs a real capture through the decoder and prints a summary (docs/monitor.md).
add_executable(hci_snoop_summary hci_snoop_summary.cpp)
target_link_libraries(hci_snoop_summary PRIVATE btb_hci)
