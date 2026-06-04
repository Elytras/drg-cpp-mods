#pragma once

/**
 * Lib_Json — High-Performance JSON Parser Hook
 *
 * PURPOSE:
 *  Replaces the slow UJSON_C::FromString Blueprint implementation
 *  with an optimized C++ parser.
 *
 * TECHNICAL STRATEGY:
 *  1. ExecFunction Patching: Instead of a standard detour, we patch the
 *     UFunction's ExecFunction and set the FUNC_Native flag. This allows us to
 *     intercept calls directly from the Blueprint VM
 *  2. Raw Memory Injection: Unlike standard API calls that use "SetObject",
 *     "SetArray" or (which incur the overhead of blueprint call),
 *     this parser writes directly into the underlying Unreal Engine memory
 *     layouts (TMap, TArray, SparseArray).
 *  3. Call Path Detection: Correctly distinguishes between Direct VM calls
 *     (where we must manually evaluate bytecode via StepOne) and standard
 *     ProcessEvent calls (where parameters are pre-evaluated).
 * SAFETY:
 *  - Includes a GObjects sweep during Teardown to identify and patch "clone"
 *    UFunctions created by Blueprint class regeneration, preventing crashes
 *    after the DLL is unloaded.
 */

namespace JsonHook {
    void Setup();
    void Teardown();
}
