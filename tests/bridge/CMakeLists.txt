cmake_minimum_required(VERSION 3.24)
project(KratosRageBridgeTests LANGUAGES CXX)

# ---------------------------------------------------------------------------
# GameBridge 离线 mock 集成测试（独立工程，不参与根 CMakeLists，也不碰 build/native）。
# 由 tools/test_bridge.py 调用：
#   * 先把 src/GameBridge.{h,cpp} + src/RageState.{h,cpp} 原样复制到
#     build/bridge-tests/source/，并写入同目录的 fake PCH.h；
#   * 再 cmake -S tests/bridge -B build/bridge-tests/build 构建本文件。
# ---------------------------------------------------------------------------

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>DLL")

if(NOT MSVC)
    message(FATAL_ERROR "KratosRageBridgeTests 需要 MSVC 2022 工具链（/utf-8 + C++23）")
endif()
add_compile_options(/utf-8 /EHsc /MP)

set(KR_BRIDGE_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/../../build/bridge-tests/source"
    CACHE PATH "原样复制的 src/ + fake PCH.h 所在目录（由 tools/test_bridge.py 生成）")

if(NOT EXISTS "${KR_BRIDGE_SOURCE_DIR}/GameBridge.cpp")
    message(FATAL_ERROR
        "缺少 ${KR_BRIDGE_SOURCE_DIR}/GameBridge.cpp，请先运行 python tools/test_bridge.py")
endif()

add_executable(KratosRageBridgeTests
    "${CMAKE_CURRENT_LIST_DIR}/BridgeTests.cpp"
    "${KR_BRIDGE_SOURCE_DIR}/GameBridge.cpp"
    "${KR_BRIDGE_SOURCE_DIR}/RageState.cpp")

# 复制目录在前：GameBridge.h / PCH.h / RageState.h 都从副本解析；
# 用例驱动的 Fake.h 从 fake/ 解析。
target_include_directories(KratosRageBridgeTests PRIVATE
    "${KR_BRIDGE_SOURCE_DIR}"
    "${CMAKE_CURRENT_LIST_DIR}/fake")
