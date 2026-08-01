function(minirt_enable_sanitizers target_name)
    # MSVC 环境下不启动 sanitizer
    if (MSVC)
        message(STATUS "Sanitizer configuration is skipped for MSVC")
        return()
    endif()

    # TSAN 不能和 ASAN、UBSAN 一起构建，需要单独构建
    if (MINIRT_ENABLE_TSAN AND (MINIRT_ENABLE_ASAN OR MINIRT_ENABLE_UBSAN))
        message(FATAL_ERROR
            "ThreadSanitizer must use a separate build "
            "from AddressSanitizer and UndefinedBehaviorSanitizer"
        )
    endif()

    set(sanitizers)

    if (MINIRT_ENABLE_ASAN)
        list(APPEND sanitizers address)
    endif()

    if (MINIRT_ENABLE_UBSAN)
        list(APPEND sanitizers undefined)
    endif()

    if (MINIRT_ENABLE_TSAN)
        list(APPEND sanitizers thread)
    endif()

    if (NOT sanitizers)
        return()
    endif()

    list(JOIN sanitizers "," sanitizer_list)
    set(sanitizer_flag "-fsanitize=${sanitizer_list}")
    
    # 所有被检测目标都需要在编译阶段加入 Sanitizer。
    target_compile_options(
        ${target_name}
        PRIVATE
            ${sanitizer_flag}
            -fno-omit-frame-pointer
    )

    # 只有最终可链接目标才需要链接 Sanitizer 运行库
    # 静态库不产生最终可执行文件，因此不在这里添加链接参数
    get_target_property(target_type ${target_name} TYPE)
    if(target_type STREQUAL "EXECUTABLE" OR target_type STREQUAL "SHARED_LIBRARY" OR target_type STREQUAL "MODULE_LIBRARY")
        target_link_options(
            ${target_name}
            PRIVATE
                ${sanitizer_flag}
        )
    endif()

endfunction(minirt_enable_sanitizers target_name)
