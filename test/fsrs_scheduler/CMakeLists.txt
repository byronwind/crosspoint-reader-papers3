add_executable(FsrsSchedulerTest
  FsrsSchedulerTest.cpp
  ${REPO_ROOT}/lib/fsrs/src/FsrsScheduler.cpp
  ${REPO_ROOT}/lib/fsrs/src/FsrsQueue.cpp
)

target_include_directories(FsrsSchedulerTest PRIVATE
  ${REPO_ROOT}/lib/fsrs/include
)

target_compile_options(FsrsSchedulerTest PRIVATE
  -Wno-pedantic  # std::clamp narrowing in some STL versions
)

target_link_libraries(FsrsSchedulerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FsrsSchedulerTest)
