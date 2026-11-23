cmake_minimum_required(VERSION 3.15)
project(twilight CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)   # optimised build: important with a 2 s limit
endif()

# The tournament runs on the school's CTI machines. Link the C++ runtime
# statically so the program runs there without installing anything extra.
if(MSVC)
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
elseif(MINGW)
  set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -static")
endif()

include_directories(include)
add_executable(twilight
  src/main.cpp     # game loop
  src/client.cpp   # networking and protocol
  src/rules.cpp    # board updates and game rules
  src/eval.cpp     # evaluation function
  src/moves.cpp    # candidate moves
  src/search.cpp   # alpha-beta search
)

if(WIN32)
  target_link_libraries(twilight ws2_32)   # Windows sockets library
endif()

# ---- Tests (run them all with: ctest --test-dir build) ----
set(BOT_LOGIC src/rules.cpp src/eval.cpp src/moves.cpp src/search.cpp)

# Bot-vs-bot games: every move legal and on time, boards up to 255 x 255.
add_executable(twilight_tests tests/test_search.cpp ${BOT_LOGIC})

# The game rules and the evaluation, case by case.
add_executable(twilight_rules_tests tests/test_rules.cpp ${BOT_LOGIC})

# Self-play weight tuning (not a test; run it by hand): twilight_tune [rounds] [maps] [depth] [threads]
add_executable(twilight_tune tools/tune.cpp ${BOT_LOGIC})
find_package(Threads REQUIRED)
target_link_libraries(twilight_tune Threads::Threads)

enable_testing()
add_test(NAME rules  COMMAND twilight_rules_tests)
add_test(NAME search COMMAND twilight_tests)

# A fake server playing two full games against the real program (macOS / Linux).
if(NOT WIN32)
  add_executable(twilight_protocol_tests tests/test_protocol.cpp ${BOT_LOGIC})
  target_compile_definitions(twilight_protocol_tests PRIVATE TWILIGHT_BIN="$<TARGET_FILE:twilight>")
  add_dependencies(twilight_protocol_tests twilight)
  target_link_libraries(twilight_protocol_tests Threads::Threads)
  add_test(NAME protocol COMMAND twilight_protocol_tests)
endif()
