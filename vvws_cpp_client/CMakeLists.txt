cmake_minimum_required(VERSION 3.16)
project(vvws_client LANGUAGES CXX)

# NGS is a standalone decision library. It contains no socket or protocol code.
add_library(vvws_ngs STATIC
    ../ngs/src/agent.cpp
    ../ngs/src/movement_rules.cpp
    ../ngs/src/transition_engine.cpp
    ../ngs/src/evaluation.cpp
    ../ngs/src/candidate_generator.cpp
    ../ngs/src/multi_move_candidate_generator.cpp
    ../ngs/src/candidate_ranker.cpp
    ../ngs/src/opponent_history.cpp
    ../ngs/src/opponent_predictor.cpp
    ../ngs/src/opponent_search.cpp
)
target_compile_features(vvws_ngs PUBLIC cxx_std_17)
target_include_directories(vvws_ngs PUBLIC
    ../ngs/include
    ../ngs/src
)

add_executable(vvws_client client.cpp)
target_compile_features(vvws_client PRIVATE cxx_std_17)
target_include_directories(vvws_client PRIVATE ../ngs/src)
target_link_libraries(vvws_client PRIVATE vvws_ngs)
set_target_properties(vvws_client PROPERTIES OUTPUT_NAME "vvws_client")

if(WIN32)
    # Winsock 2 is part of Windows; no professor-provided DLL is linked here.
    target_link_libraries(vvws_client PRIVATE ws2_32)
    target_compile_definitions(vvws_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

if(MSVC)
    target_compile_options(vvws_ngs PRIVATE /W4 /permissive-)
    target_compile_options(vvws_client PRIVATE /W4 /permissive-)
else()
    target_compile_options(vvws_ngs PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(vvws_client PRIVATE -Wall -Wextra -Wpedantic)
endif()

# Keep MinGW-built executables self-contained with respect to the GNU C++ runtime.
if(MINGW)
    target_link_options(vvws_client PRIVATE -static -static-libgcc -static-libstdc++)
endif()

include(CTest)
if(BUILD_TESTING)
    # Existing opponent-history unit tests now link the compiled NGS library.
    add_executable(
        vvws_opponent_history_test
        ../ngs/tests/opponent_history_test.cpp
    )
    target_compile_features(vvws_opponent_history_test PRIVATE cxx_std_17)
    target_include_directories(vvws_opponent_history_test PRIVATE
        ../ngs/include ../ngs/src
    )
    target_link_libraries(vvws_opponent_history_test PRIVATE vvws_ngs)

    add_executable(
        vvws_agent_test
        ../ngs/tests/agent_test.cpp
    )
    target_compile_features(vvws_agent_test PRIVATE cxx_std_17)
    target_include_directories(vvws_agent_test PRIVATE
        ../ngs/include ../ngs/src
    )
    target_link_libraries(vvws_agent_test PRIVATE vvws_ngs)

    if(MSVC)
        target_compile_options(vvws_opponent_history_test PRIVATE /W4 /permissive- /UNDEBUG)
        target_compile_options(vvws_agent_test PRIVATE /W4 /permissive- /UNDEBUG)
    else()
        # The tests use assert(); keep assertions enabled in Release builds.
        target_compile_options(vvws_opponent_history_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
        target_compile_options(vvws_agent_test PRIVATE -Wall -Wextra -Wpedantic -UNDEBUG)
    endif()

    add_test(NAME opponent_history_unit COMMAND vvws_opponent_history_test)
    add_test(NAME agent_orchestration_unit COMMAND vvws_agent_test)
    set_tests_properties(opponent_history_unit agent_orchestration_unit PROPERTIES TIMEOUT 20)

    # C++ integration test: starts a local mock TCP server and launches vvws_client.
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
    set_tests_properties(cpp_client_protocol PROPERTIES TIMEOUT 30)
endif()
