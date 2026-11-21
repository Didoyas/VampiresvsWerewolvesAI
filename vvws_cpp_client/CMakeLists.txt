cmake_minimum_required(VERSION 3.16)
project(vvws_cpp_client LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# The repository keeps common/, ngs/, and vvws_cpp_client/ as siblings.
set(VVW_COMMON_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../common/include")
set(VVW_NGS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ngs/src")
set(VVW_NGS_INCLUDE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../ngs/include")

# -----------------------------------------------------------------------------
# NGS decision engine library
# -----------------------------------------------------------------------------
# The adapter is part of this library and supplies the implementation of
# vvw::game::makeDecisionEngine() for this NGS-backed executable.
add_library(vvws_ngs STATIC
    ../ngs/src/agent.cpp
    ../ngs/src/ngs_decision_engine.cpp
    ../ngs/src/decision_engine_factory.cpp
    ../ngs/src/movement_rules.cpp
    ../ngs/src/transition_engine.cpp
    ../ngs/src/evaluation.cpp
    ../ngs/src/candidate_generator.cpp
    ../ngs/src/multi_move_candidate_generator.cpp
    ../ngs/src/candidate_ranker.cpp
    ../ngs/src/opponent_history.cpp
    ../ngs/src/opponent_predictor.cpp
    ../ngs/src/opponent_search.cpp
    ../ngs/src/state_adapter.cpp
)

target_compile_features(vvws_ngs PUBLIC cxx_std_17)
target_include_directories(vvws_ngs PUBLIC
    "${VVW_COMMON_INCLUDE_DIR}"
    "${VVW_NGS_SOURCE_DIR}"
    "${VVW_NGS_INCLUDE_DIR}"
)

# -----------------------------------------------------------------------------
# Single executable for now: shared TCP client + NGS decision engine
# -----------------------------------------------------------------------------
add_executable(vvws_client
    client.cpp
)
target_compile_features(vvws_client PRIVATE cxx_std_17)
target_link_libraries(vvws_client PRIVATE vvws_ngs)
set_target_properties(vvws_client PROPERTIES OUTPUT_NAME "vvws_client")

if(WIN32)
    target_link_libraries(vvws_client PRIVATE ws2_32)
    target_compile_definitions(vvws_client PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
endif()

# Apply consistent warnings to production targets.
function(vvw_enable_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /permissive-)
    else()
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
    endif()
endfunction()

vvw_enable_warnings(vvws_ngs)
vvw_enable_warnings(vvws_client)

# Keep MinGW-built Windows executables self-contained with respect to the GNU
# C++ runtime. This affects executable targets, not the static NGS archive.
if(MINGW)
    target_link_options(vvws_client PRIVATE -static -static-libgcc -static-libstdc++)
endif()

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
include(CTest)

if(BUILD_TESTING)
    # The common interface is tested without linking any algorithm.
    add_executable(vvws_decision_engine_contract_test
        ../common/include/vvw/tests/decision_engine_test.cpp
    )
    target_include_directories(vvws_decision_engine_contract_test PRIVATE
        "${VVW_COMMON_INCLUDE_DIR}"
    )
    target_compile_features(vvws_decision_engine_contract_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_decision_engine_contract_test)

    # Movement validation, including destination population wire limits.
    add_executable(vvws_movement_rules_test
        ../ngs/tests/movement_rules_test.cpp
    )
    target_link_libraries(vvws_movement_rules_test PRIVATE vvws_ngs)
    target_compile_features(vvws_movement_rules_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_movement_rules_test)

    # Cooperative deadline propagation across generation and search.
    add_executable(vvws_search_deadline_test
        ../ngs/tests/search_deadline_test.cpp
    )
    target_link_libraries(vvws_search_deadline_test PRIVATE vvws_ngs)
    target_compile_features(vvws_search_deadline_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_search_deadline_test)

    # Existing NGS history test.
    add_executable(vvws_opponent_history_test
        ../ngs/tests/opponent_history_test.cpp
    )
    target_link_libraries(vvws_opponent_history_test PRIVATE vvws_ngs)
    target_compile_features(vvws_opponent_history_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_opponent_history_test)

    # Existing NGS orchestration test.
    add_executable(vvws_agent_test
        ../ngs/tests/agent_test.cpp
    )
    target_link_libraries(vvws_agent_test PRIVATE vvws_ngs)
    target_compile_features(vvws_agent_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_agent_test)

        # New test: NGS is usable through the shared DecisionEngine interface.
    add_executable(vvws_ngs_decision_engine_test
        ../ngs/tests/ngs_decision_engine_test.cpp
    )
    target_link_libraries(vvws_ngs_decision_engine_test PRIVATE vvws_ngs)
    target_compile_features(vvws_ngs_decision_engine_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_ngs_decision_engine_test)

    # Multi-move candidate generation, including four-source plans.
    add_executable(vvws_multi_move_candidate_generator_test
        ../ngs/tests/multi_move_candidate_generator_test.cpp
    )
    target_link_libraries(vvws_multi_move_candidate_generator_test PRIVATE vvws_ngs)
    target_compile_features(vvws_multi_move_candidate_generator_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_multi_move_candidate_generator_test)

    # Opponent movement prediction tests.
    add_executable(vvws_opponent_predictor_test
        ../ngs/tests/opponent_predictor_test.cpp
    )
    target_link_libraries(vvws_opponent_predictor_test PRIVATE vvws_ngs)
    target_compile_features(vvws_opponent_predictor_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_opponent_predictor_test)

    # History-aware opponent response search tests.
    add_executable(vvws_opponent_search_test
        ../ngs/tests/opponent_search_test.cpp
    )
    target_link_libraries(vvws_opponent_search_test PRIVATE vvws_ngs)
    target_compile_features(vvws_opponent_search_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_opponent_search_test)

    # Horizon-aware position evaluation and simulated turn accounting.
    add_executable(vvws_evaluation_test
        ../ngs/tests/evaluation_test.cpp
    )
    target_link_libraries(vvws_evaluation_test PRIVATE vvws_ngs)
    target_compile_features(vvws_evaluation_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_evaluation_test)

    add_executable(vvws_transition_engine_test
        ../ngs/tests/transition_engine_test.cpp
    )
    target_link_libraries(vvws_transition_engine_test PRIVATE vvws_ngs)
    target_compile_features(vvws_transition_engine_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_transition_engine_test)

    add_executable(vvws_state_adapter_test
        ../ngs/tests/state_adapter_test.cpp
    )
    target_link_libraries(vvws_state_adapter_test PRIVATE vvws_ngs)
    target_compile_features(vvws_state_adapter_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_state_adapter_test)

    add_executable(vvws_game_horizon_tracker_test
        ../ngs/tests/game_horizon_tracker_test.cpp
    )
    target_include_directories(vvws_game_horizon_tracker_test PRIVATE
        "${VVW_COMMON_INCLUDE_DIR}"
    )
    target_compile_features(vvws_game_horizon_tracker_test PRIVATE cxx_std_17)
    vvw_enable_warnings(vvws_game_horizon_tracker_test)

    # Assertions must remain active in Release builds for these unit tests.
    foreach(test_target IN ITEMS
        vvws_decision_engine_contract_test
        vvws_movement_rules_test
        vvws_search_deadline_test
        vvws_opponent_history_test
        vvws_agent_test
        vvws_ngs_decision_engine_test
        vvws_multi_move_candidate_generator_test
        vvws_opponent_predictor_test
        vvws_opponent_search_test
        vvws_evaluation_test
        vvws_transition_engine_test
        vvws_state_adapter_test
        vvws_game_horizon_tracker_test
    )
        if(MSVC)
            target_compile_options(${test_target} PRIVATE /UNDEBUG)
        else()
            target_compile_options(${test_target} PRIVATE -UNDEBUG)
        endif()
    endforeach()

    add_test(
        NAME decision_engine_contract_unit
        COMMAND vvws_decision_engine_contract_test
    )
    add_test(
        NAME movement_rules_unit
        COMMAND vvws_movement_rules_test
    )
    add_test(
        NAME search_deadline_unit
        COMMAND vvws_search_deadline_test
    )
    add_test(
        NAME opponent_history_unit
        COMMAND vvws_opponent_history_test
    )
    add_test(
        NAME agent_orchestration_unit
        COMMAND vvws_agent_test
    )
    add_test(
        NAME ngs_decision_engine_unit
        COMMAND vvws_ngs_decision_engine_test
    )
    add_test(
        NAME multi_move_candidate_generator_unit
        COMMAND vvws_multi_move_candidate_generator_test
    )

    add_test(
        NAME opponent_predictor_unit
        COMMAND vvws_opponent_predictor_test
    )

    add_test(
        NAME opponent_search_unit
        COMMAND vvws_opponent_search_test
    )
    add_test(
        NAME evaluation_unit
        COMMAND vvws_evaluation_test
    )
    add_test(
        NAME transition_engine_unit
        COMMAND vvws_transition_engine_test
    )
    add_test(
        NAME state_adapter_unit
        COMMAND vvws_state_adapter_test
    )
    add_test(
        NAME game_horizon_tracker_unit
        COMMAND vvws_game_horizon_tracker_test
    )

    set_tests_properties(
        decision_engine_contract_unit
        movement_rules_unit
        search_deadline_unit
        opponent_history_unit
        agent_orchestration_unit
        ngs_decision_engine_unit
        multi_move_candidate_generator_unit
        opponent_predictor_unit
        opponent_search_unit
        evaluation_unit
        transition_engine_unit
        state_adapter_unit
        game_horizon_tracker_unit
        PROPERTIES TIMEOUT 30
    )

    # Local mock TCP server integration test. It launches the production client.
    find_package(Threads REQUIRED)
    add_executable(vvws_cpp_mock_server_test
        tests/mock_server_test.cpp
    )
    target_compile_features(vvws_cpp_mock_server_test PRIVATE cxx_std_17)
    target_link_libraries(vvws_cpp_mock_server_test PRIVATE Threads::Threads)
    vvw_enable_warnings(vvws_cpp_mock_server_test)

    if(WIN32)
        target_link_libraries(vvws_cpp_mock_server_test PRIVATE ws2_32)
        target_compile_definitions(vvws_cpp_mock_server_test PRIVATE
            WIN32_LEAN_AND_MEAN NOMINMAX
        )
    endif()

    if(MINGW)
        target_link_options(vvws_cpp_mock_server_test PRIVATE
            -static -static-libgcc -static-libstdc++
        )
    endif()

    add_test(
        NAME cpp_client_protocol
        COMMAND vvws_cpp_mock_server_test $<TARGET_FILE:vvws_client>
    )
    set_tests_properties(cpp_client_protocol PROPERTIES TIMEOUT 30)
endif()
