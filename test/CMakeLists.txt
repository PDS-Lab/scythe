function(add_test TEST_FILE)
  get_filename_component(TEST_NAME ${TEST_FILE} NAME_WE)
  add_executable(${TEST_NAME} ${TEST_FILE})
  target_link_libraries(${TEST_NAME} 
    base
    gtest_main
  )
  message(STATUS "[ADD TEST] : ${TEST_NAME}")
  # gtest_discover_tests(${TEST_NAME})
endfunction()

FILE(GLOB_RECURSE  test_files *_test.cc)
# message(STATUS "${test_files}")

foreach(test_file ${test_files})
  add_test(${test_file})
endforeach(test_file)