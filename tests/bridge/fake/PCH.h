#pragma once
// ---------------------------------------------------------------------------
// fake PCH.h —— 由 tools/test_bridge.py 原样复制到 build/bridge-tests/source/，
// 与被复制的 src/GameBridge.cpp / GameBridge.h 同目录，从而顶替真实的
// <RE/Skyrim.h> + <SKSE/SKSE.h> + <spdlog/spdlog.h>（见 fake/Fake.h 顶部说明）。
//
// 注意：本文件必须在「同一份」内容下同时服务 GameBridge.cpp 与测试驱动，
// 不得包含任何依赖生产源码内部状态的开关。
// ---------------------------------------------------------------------------

#include "Fake.h"
