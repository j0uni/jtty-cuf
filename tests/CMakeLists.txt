find_package(Threads REQUIRED)

add_executable(jtty_tests jtty_tests.cpp)
target_link_libraries(jtty_tests PRIVATE jtty::jtty Threads::Threads)
add_test(NAME jtty_tests COMMAND jtty_tests)
set_tests_properties(jtty_tests PROPERTIES TIMEOUT 600)

if(JTTY_BUILD_EXAMPLES)
  # End to end through the example tools: text -> WAV file -> text.
  add_test(NAME jtty_encode_wav
    COMMAND jtty_encode "K1ABC HELLO" ${CMAKE_CURRENT_BINARY_DIR}/sample.wav 1700)
  add_test(NAME jtty_decode_wav
    COMMAND jtty_decode ${CMAKE_CURRENT_BINARY_DIR}/sample.wav)
  set_tests_properties(jtty_encode_wav PROPERTIES FIXTURES_SETUP sample_wav)
  set_tests_properties(jtty_decode_wav PROPERTIES
    FIXTURES_REQUIRED sample_wav
    PASS_REGULAR_EXPRESSION "1700 K1ABC HELLO")
  add_test(NAME jtty_decode_rejects_garbage
    COMMAND jtty_decode ${CMAKE_CURRENT_SOURCE_DIR}/jtty_tests.cpp)
  set_tests_properties(jtty_decode_rejects_garbage PROPERTIES WILL_FAIL TRUE)
endif()
