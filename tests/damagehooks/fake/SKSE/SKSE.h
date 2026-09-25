#pragma once

// Test stand-in for SKSE/SKSE.h. The damage hook needs no SKSE symbol: CommonLibSSE-NG's
// Relocation::write_vfunc patches the vtable in place, so no trampoline is required. The
// header exists only so the fake PCH mirrors the real src/PCH.h include list.
