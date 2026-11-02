# AntiAfk — Plugify C++ port of the Metamod plugin Anti-Afk (ABKAM): AFK warnings, suicides and the
# final kick/spectator punishment on the CS2 dedicated server.
#
# Pure s2sdk (docs/PLUGIN_STANDARD.md §7): no pisexcompat, no guard library, no hudmenu consumer.
# The map gate (PITFALLS B1/B2), the slot/controller guards (C2/C1), the map-gated schema offsets
# (C8), the update-driven cadence (C9/B4, deliberately no s2sdk timer), the event hooks and their
# teardown (C12/A7) and the KV1 reader all live in this plugin's own sources.
#
#   antiafk_core        the engine-independent state machine (+ antiafk_core_tests)
#   plugify-AntiAfk     the plugin shared library (entry "bin/AntiAfk")
#   antiafk_plugin_tests the plugin TU with recording __s2sdk_* fakes (map gate, update cadence,
#                       warnings, reuse/disconnect, teardown); no pisexcompat, no Plugify host,
#                       no engine and no s2sdk timer

set(ANTIAFK_VERSION "1.1.0")

# --- engine-independent core --------------------------------------------------
add_library(antiafk_core STATIC src/antiafk_core.cpp)
target_include_directories(antiafk_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(antiafk_core SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(antiafk_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(antiafk_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-AntiAfk SHARED
        src/plugin.cpp
        # The __s2sdk_* binding globals the plugify module fills from the manifest's dependencies.
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-AntiAfk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated s2sdk/plg headers must not fail -Werror on our sources.
target_include_directories(plugify-AntiAfk SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-AntiAfk PRIVATE
        antiafk_core
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(plugify-AntiAfk PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(plugify-AntiAfk PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="AntiAfk"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
)
set_target_properties(plugify-AntiAfk PROPERTIES OUTPUT_NAME AntiAfk)

if(UNIX AND NOT APPLE)
    target_link_options(plugify-AntiAfk PRIVATE
            "-Wl,--version-script,${CMAKE_CURRENT_SOURCE_DIR}/sym/version_script.lds")
endif()

configure_file(
        "${CMAKE_CURRENT_SOURCE_DIR}/AntiAfk.pplugin.in"
        "${CMAKE_CURRENT_BINARY_DIR}/AntiAfk.pplugin"
)

# --- tests --------------------------------------------------------------------
# The top-level CMakeLists declares the PLUGIFY_PORT_TESTS option *after* the plugin directories, so
# on a first configure it is not defined here yet: default it to the same ON. An explicit
# -DPLUGIFY_PORT_TESTS=OFF (the exported standalone build) is already in the cache and still wins.
if(NOT DEFINED PLUGIFY_PORT_TESTS)
    set(PLUGIFY_PORT_TESTS ON)
endif()
if(PLUGIFY_PORT_TESTS)
# antiafk_core_tests: the engine-independent state machine (timing, countdown, warning escalation,
# kick/spec final punishment). No engine, no s2sdk.
add_executable(antiafk_core_tests
        tests/antiafk_core_tests.cpp
        src/antiafk_core.cpp
)
target_include_directories(antiafk_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(antiafk_core_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(antiafk_core_tests PRIVATE -Wall -Wextra -Werror)
set_target_properties(antiafk_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")

# antiafk_plugin_tests: the plugin TU is compiled into the test (tests/antiafk_plugin_tests.cpp
# includes ../src/plugin.cpp) and every __s2sdk_* binding slot is a recording fake. Covered: the
# plugin-local KV1 reader, the shipped production config/phrases and the exact pisexcompat raw-cwd
# first candidate order (first existing authoritative, malformed does not fall through), the
# ServerLang -> s2sdk -> en language chain, PITFALLS B1/B2 (gate), the C9/B4 update cadence
# (never an s2sdk timer), C2/C1 (slot/controller/handle guards), the full warning/countdown/
# punishment sequence, slot reuse/disconnect/map change and A7 teardown.
# Written into build/tests so ../test.sh runs it with the rest (*_tests convention).
add_executable(antiafk_plugin_tests
        tests/antiafk_plugin_tests.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(antiafk_plugin_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(antiafk_plugin_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(antiafk_plugin_tests PRIVATE
        antiafk_core
        plugify_sdk
        -static-libstdc++ -static-libgcc
)
target_compile_options(antiafk_plugin_tests PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(antiafk_plugin_tests PRIVATE
        _GLIBCXX_USE_CXX11_ABI=1
        PLUGIFY_MODULE="AntiAfk"
        PLUGIFY_IS_DEBUG=0
        PLUGIFY_IS_RELEASE=1
        ANTIAFK_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/data"
)
set_target_properties(antiafk_plugin_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
endif()
