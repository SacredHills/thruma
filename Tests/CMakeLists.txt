add_executable(thruma_tests
    TestMain.cpp
    TestCore.cpp
    TestDetection.cpp
    TestSampleEngine.cpp
    TestReplace.cpp
    TestDebleed.cpp
    TestRealtime.cpp)

target_link_libraries(thruma_tests PRIVATE thruma_core)
target_compile_definitions(thruma_tests PRIVATE THRUMA_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")

add_test(NAME thruma_dsp_tests COMMAND thruma_tests)
set_tests_properties(thruma_dsp_tests PROPERTIES TIMEOUT 900)
