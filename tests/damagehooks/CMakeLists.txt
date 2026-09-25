cmake_minimum_required(VERSION 3.24)
project(KratosRageDamageHookTests LANGUAGES CXX)

# Standalone harness for the production damage hook. It is configured directly (it is not
# added to the root CMakeLists.txt), and it compiles the real src/DamageHooks.cpp, copied
# byte for byte into the build tree, against the fake RE / REL / SKSE / spdlog / GameBridge
# headers in fake/ plus the real standard library.
#
#   cmake -S tests/damagehooks -B build/diagnostic-0.1.1/damagehook-tests -G "Visual Studio 17 2022" -A x64
#   cmake --build build/diagnostic-0.1.1/damagehook-tests --config Release
#   build/diagnostic-0.1.1/damagehook-tests/Release/KratosRageDamageHookTests.exe
#   build/diagnostic-0.1.1/damagehook-tests/Release/KratosRageDamageHookTests.exe empty

set(KR_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../../src/DamageHooks.cpp")
set(KR_HEADER "${CMAKE_CURRENT_SOURCE_DIR}/../../src/DamageHooks.h")
set(KR_COPIED "${CMAKE_CURRENT_BINARY_DIR}/copied")

file(COPY "${KR_SOURCE}" "${KR_HEADER}" DESTINATION "${KR_COPIED}")
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files "${KR_SOURCE}" "${KR_COPIED}/DamageHooks.cpp"
    RESULT_VARIABLE kr_copy_result)
if(NOT kr_copy_result EQUAL 0)
    message(FATAL_ERROR "the copied DamageHooks.cpp differs from src/DamageHooks.cpp")
endif()

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(KratosRageDamageHookTests
    "${CMAKE_CURRENT_SOURCE_DIR}/DamageHookTests.cpp"
    "${KR_COPIED}/DamageHooks.cpp")

# fake/ must come first: it supplies PCH.h, GameBridge.h and the RE/REL/SKSE/spdlog headers
# that the copied production file includes. copied/ supplies the copied DamageHooks.h.
target_include_directories(KratosRageDamageHookTests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/fake"
    "${KR_COPIED}")

if(MSVC)
    target_compile_options(KratosRageDamageHookTests PRIVATE /utf-8 /EHsc /W4)
endif()
