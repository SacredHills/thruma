add_library(thruma_core STATIC
    Instruments.cpp
    TransientDetector.cpp
    FFT.cpp
    Resampler.cpp
    Kit.cpp
    SampleEngine.cpp
    Debleed.cpp
    Triggers.cpp
    ReplaceEngine.cpp
    LinkHub.cpp)

target_include_directories(thruma_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

find_package(Threads REQUIRED)
target_link_libraries(thruma_core PUBLIC Threads::Threads)

if(MSVC)
    target_compile_options(thruma_core PRIVATE /W4 /fp:fast)
else()
    target_compile_options(thruma_core PRIVATE -Wall -Wextra -Wno-unused-parameter
        $<$<CONFIG:Release>:-O3> -ffp-contract=fast)
endif()
