// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "vita/VitaGpuVuCgGenerator.h"
#include "vita/VitaGpuVuShaderCompiler.h"

namespace VitaGpuVu {

// Bump whenever generated source conventions or the GXM binding contract
// change. The key is exclusively generated-program content; it never contains
// a title, ELF, address, game, or known-program identity.
constexpr u32 GeneratedProgramAbiVersion = 3;

enum class GeneratedProgramState : u8 {
  Missing,
  Queued,
  Compiled,
  Ready,
  Failed,
  Unavailable,
};

struct ProgramRegistryStatistics {
  u64 requests = 0;
  u64 unavailable_requests = 0;
  u64 cache_hits = 0;
  u64 cache_misses = 0;
  u64 compiler_queue_retries = 0;
  u64 compile_successes = 0;
  u64 compile_failures = 0;
  u64 ready_programs = 0;
  u64 failed_programs = 0;
  ShaderCompilerStatistics compiler;
};

// Stable 128-bit content key used only to coalesce/invalidate generated GXP.
ShaderKey MakeGeneratedProgramKey(const GeneratedCgProgram &program);

// GSDeviceGXM owns the compiler lifetime. These calls merely expose its
// bounded asynchronous queue to the EE producer; compilation remains
// serialized on the low-priority compiler thread.
bool AttachGeneratedProgramCompiler(ShaderCompiler *compiler);
void DetachGeneratedProgramCompiler(ShaderCompiler *compiler);

// Moves source into the compiler queue and retains only resource metadata.
// Duplicate content is coalesced. This call never waits for compilation.
bool RequestGeneratedProgram(GeneratedCgProgram program, ShaderKey *key);
GeneratedProgramState QueryGeneratedProgram(const ShaderKey &key);

// GS-thread-only completion handoff. Registration and patching remain owned by
// GSDeviceGXM; the registry never calls libGXM.
bool PollGeneratedProgramCompile(CompileResult *result,
                                 GeneratedCgProgram *metadata);
void CompleteGeneratedProgramRegistration(const ShaderKey &key, bool succeeded);
ProgramRegistryStatistics GetGeneratedProgramRegistryStatistics();

// Called after the GS context has drained and generated programs are released.
void ClearGeneratedProgramRegistry();

} // namespace VitaGpuVu
