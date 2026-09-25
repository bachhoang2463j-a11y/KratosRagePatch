cmake_minimum_required(VERSION 3.24)
project(KratosRagePatch VERSION 0.2.0 LANGUAGES CXX)

option(KR_BUILD_PLUGIN "Build Skyrim native plugin" ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(MSVC)
    add_compile_options(/utf-8 /EHsc /MP)
endif()

add_library(KratosRageRules STATIC src/RageState.cpp)
target_include_directories(KratosRageRules PUBLIC src)

include(CTest)
if(BUILD_TESTING)
    add_executable(KratosRageTests
        tests/TestMain.cpp
        tests/RageStateTests.cpp
        tests/PlayerDamageTests.cpp
        tests/CombatTimerTests.cpp
        tests/ActivationTests.cpp
        tests/SerializationTests.cpp)
    target_link_libraries(KratosRageTests PRIVATE KratosRageRules)
    add_test(NAME RageRules COMMAND KratosRageTests "${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures")
endif()

if(KR_BUILD_PLUGIN)
    set(ENABLE_SKYRIM_SE OFF CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_AE ON CACHE BOOL "" FORCE)
    set(ENABLE_SKYRIM_VR OFF CACHE BOOL "" FORCE)
    set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
    set(SKSE_SUPPORT_XBYAK OFF CACHE BOOL "" FORCE)
    set(RAPIDCSV_INCLUDE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}/vendor/rapidcsv/src" CACHE PATH "" FORCE)
    list(PREPEND CMAKE_PREFIX_PATH "${CMAKE_CURRENT_SOURCE_DIR}/vendor/prefix")
    add_subdirectory(vendor/CommonLibSSE-NG EXCLUDE_FROM_ALL)
    add_library(KratosRage SHARED
        src/Plugin.cpp
        src/GameBridge.cpp
        src/PapyrusBindings.cpp
        src/Serialization.cpp)
    target_link_libraries(KratosRage PRIVATE KratosRageRules CommonLibSSE::CommonLibSSE)
    target_include_directories(KratosRage PRIVATE "${RAPIDCSV_INCLUDE_DIRS}")
    target_precompile_headers(KratosRage PRIVATE src/PCH.h)
    set_target_properties(KratosRage PROPERTIES PREFIX "")
    add_custom_command(TARGET KratosRage POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_CURRENT_SOURCE_DIR}/build/stage/SKSE/Plugins"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "$<TARGET_FILE:KratosRage>" "${CMAKE_CURRENT_SOURCE_DIR}/build/stage/SKSE/Plugins/KratosRage.dll")
endif()
