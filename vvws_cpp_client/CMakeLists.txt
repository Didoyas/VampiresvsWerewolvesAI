cmake_minimum_required(VERSION 3.16)
project(vvws_client LANGUAGES CXX)

add_executable(vvws_client client.cpp)
target_compile_features(vvws_client PRIVATE cxx_std_17)
set_target_properties(vvws_client PROPERTIES OUTPUT_NAME "vvws_client")

if(WIN32)
    # Winsock 2 is part of Windows; no professor-provided DLL is linked here.
    target_link_libraries(vvws_client PRIVATE ws2_32)
    target_compile_definitions(vvws_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
    target_compile_options(vvws_client PRIVATE /W4 /permissive-)
else()
    target_compile_options(vvws_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Keep MinGW-built executables self-contained with respect to the GNU C++ runtime.
if(MINGW)
    target_link_options(vvws_client PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# C++ integration test: starts a local mock TCP server and launches vvws_client.
include(CTest)
if(BUILD_TESTING)
    find_package(Threads REQUIRED)

    add_executable(vvws_cpp_mock_server_test tests/mock_server_test.cpp)
    target_compile_features(vvws_cpp_mock_server_test PRIVATE cxx_std_17)
    target_link_libraries(vvws_cpp_mock_server_test PRIVATE Threads::Threads)

    if(WIN32)
        target_link_libraries(vvws_cpp_mock_server_test PRIVATE ws2_32)
        target_compile_definitions(vvws_cpp_mock_server_test PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
    endif()

    if(MSVC)
        target_compile_options(vvws_cpp_mock_server_test PRIVATE /W4 /permissive-)
    else()
        target_compile_options(vvws_cpp_mock_server_test PRIVATE -Wall -Wextra -Wpedantic)
    endif()

    if(MINGW)
        target_link_options(vvws_cpp_mock_server_test PRIVATE -static -static-libgcc -static-libstdc++)
    endif()

    add_test(
        NAME cpp_client_protocol
        COMMAND vvws_cpp_mock_server_test $<TARGET_FILE:vvws_client>
    )
    set_tests_properties(cpp_client_protocol PROPERTIES TIMEOUT 20)
endif()