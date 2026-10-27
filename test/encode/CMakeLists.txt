if(BUILD_TESTING)
    add_executable(hpu_encode_self_test
            self_test.cpp
    )
    target_link_libraries(hpu_encode_self_test PRIVATE hpu_encode)
    target_include_directories(hpu_encode_self_test PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/../../include
    )
    target_compile_features(hpu_encode_self_test PRIVATE cxx_std_17)
    add_test(NAME hpu_encode_self_test COMMAND hpu_encode_self_test)
endif()
