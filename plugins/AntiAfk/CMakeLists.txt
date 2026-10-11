# AntiAfk — Plugify port of the Metamod plugin Anti-Afk (ABKAM): AFK warnings, suicides and the
# final kick/spectator punishment on the CS2 dedicated server.
#
#   plugify-AntiAfk    the plugin shared library (entry "bin/AntiAfk")
#   antiafk_core       the engine-independent state machine (+ its tests)

set(ANTIAFK_VERSION "1.0.0")

# --- engine-independent core --------------------------------------------------
add_library(antiafk_core STATIC src/antiafk_core.cpp)
target_include_directories(antiafk_core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(antiafk_core SYSTEM PUBLIC "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(antiafk_core PRIVATE -Wall -Wextra -Werror)
target_compile_definitions(antiafk_core PUBLIC _GLIBCXX_USE_CXX11_ABI=1)

# --- plugin -------------------------------------------------------------------
add_library(plugify-AntiAfk SHARED
        src/plugin.cpp
        $<TARGET_OBJECTS:s2sdk_bindings>
)
target_include_directories(plugify-AntiAfk PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
# SYSTEM: warnings from the generated pisexcompat/plg/s2sdk headers must not fail -Werror on us.
target_include_directories(plugify-AntiAfk SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_link_libraries(plugify-AntiAfk PRIVATE
        antiafk_core
        pisexcompat
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
add_executable(antiafk_core_tests
        tests/antiafk_core_tests.cpp
        src/antiafk_core.cpp
)
target_include_directories(antiafk_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src")
target_include_directories(antiafk_core_tests SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/sdk/include")
target_compile_options(antiafk_core_tests PRIVATE -Wall -Wextra -Werror)
set_target_properties(antiafk_core_tests PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tests")
