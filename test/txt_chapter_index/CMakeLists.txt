add_executable(txt_chapter_index_test
  TxtChapterIndexTest.cpp
  ${REPO_ROOT}/lib/Txt/TxtChapterIndex.cpp
)
target_include_directories(txt_chapter_index_test PRIVATE ${REPO_ROOT}/lib/Txt)
target_link_libraries(txt_chapter_index_test PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(txt_chapter_index_test)
