cmake_minimum_required(VERSION 3.16)

project(MiniRuntime VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Threads REQUIRED) # 会在 Linux 下正确添加 -pthread

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
    $<CXX_COMPILER_ID:GNU,Clang,AppleClang>:
            -Wall
            -Wextra
            -Wpedantic
        >
)

add_executable(basic_submit
    examples/basic_submit.cpp
)

target_link_libraries(basic_submit PRIVATE
    minirt
)

include(CTest)

if(BUILD_TESTING)
    add_executable(test_thread_pool
        tests/test_thread_pool.cpp
    )

    target_link_libraries(test_thread_pool
        PRIVATE
            minirt
    )

    add_test(
        NAME thread_pool_tests
        COMMAND test_thread_pool
    )
endif()