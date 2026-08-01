cmake_minimum_required(VERSION 3.16)

project(MiniRuntime VERSION 0.1.0 LANGUAGES CXX)

# -----------------------------
# Sanitizer 配置
# -----------------------------
option(MINIRT_ENABLE_ASAN "Enable AddressSanitizer" OFF)
option(MINIRT_ENABLE_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(MINIRT_ENABLE_TSAN "Enable ThreadSanitizer" OFF)
option(MINIRT_BUILD_BENCHMARKS "Build MiniRuntime benchmarks" ON)

include(${PROJECT_SOURCE_DIR}/cmake/Sanitizers.cmake)

# -----------------------------
# 设置 C++ 标准
# -----------------------------
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# -----------------------------
# 系统线程库
# -----------------------------
find_package(Threads REQUIRED) # 会在 Linux 下正确添加 -pthread

# -----------------------------
# MiniRuntime 静态库
# -----------------------------
add_library(minirt STATIC
    src/thread_pool.cpp
)

target_include_directories(minirt PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(minirt PUBLIC
    Threads::Threads
)

target_compile_options(minirt PRIVATE 
    -Wall
    -Wextra
    -Wpedantic
)

minirt_enable_sanitizers(minirt)

# -----------------------------
# 单元测试
# -----------------------------
include(CTest)

if(BUILD_TESTING)
    # GoogleTest 官方提供 CMake FetchContent 集成方式
    include(FetchContent)
    FetchContent_Declare(googletest
        GIT_REPOSITORY https://github.com/google/googletest.git
        GIT_TAG v1.17.0
        GIT_SHALLOW TRUE
    )

    # 避免 Windows 下 GoogleTest 和主项目使用不同 CRT。
    set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(googletest)

    add_executable(minirt_tests
        tests/test_thread_pool.cpp
        tests/test_submit.cpp
        tests/test_backpressure.cpp
        tests/test_cancellation.cpp
        tests/test_metrics.cpp
        tests/test_work_stealing_queue.cpp
        tests/test_work_stealing.cpp
        tests/test_stress.cpp
    )

    target_link_libraries(minirt_tests PRIVATE
        minirt
        GTest::gtest_main
    )

    target_compile_options(minirt_tests PRIVATE
        -Wall
        -Wextra
        -Wpedantic
    )

    minirt_enable_sanitizers(minirt_tests)

    # CMake 官方则推荐使用 gtest_discover_tests()，它会从测试程序中自动发现每个 TEST，并分别注册到 CTest
    include(GoogleTest)
    gtest_discover_tests(minirt_tests
        PROPERTIES
            TIMEOUT 30
    )
endif()