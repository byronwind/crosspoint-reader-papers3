add_executable(txt_encoding_test
  TxtEncodingTest.cpp
  ${REPO_ROOT}/lib/Txt/TxtEncoding.cpp
)
# GBK support is always compiled in for this fork (no ENABLE_CHINESE_VERSION gate)
target_include_directories(txt_encoding_test PRIVATE ${REPO_ROOT}/lib/Txt)
target_link_libraries(txt_encoding_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(txt_encoding_test)
