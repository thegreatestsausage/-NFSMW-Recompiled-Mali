/**
 * @file        ppc/func.h
 * @brief       Recompiled PPC function signature, mapping table, and definition macros
 *
 * @copyright   Copyright (c) 2026 Tom Clay <tomc@tctechstuff.com>
 *              All rights reserved.
 *
 * @license     BSD 3-Clause License
 *              See LICENSE file in the project root for full license text.
 *
 * @remarks     Split out of rex/ppc/context.h so callers that only need the
 *              PPCFunc type do not pull in PPCContext and its SIMD headers.
 */

#pragma once

#include <cstddef>
#include <cstdint>

//=============================================================================
// PPCFunc Type Definition
//=============================================================================
// Function signature for recompiled PPC functions.
// All recompiled functions take a context reference and memory base pointer.

// Forward declaration of the PPC execution context
struct PPCContext;

// Function signature for recompiled PPC functions
using PPCFunc = void(PPCContext& ctx, uint8_t* base);

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);

/**
 * Runtime fallback for branch edges the static codegen could not resolve
 * (emitted in place of a hard fatal). If `to` is registered in the function
 * dispatcher, it is tail-called; otherwise the helper returns and execution
 * continues at the instruction after the branch site. Each distinct
 * (from, to) pair is logged once per process.
 */
void rexglue_branch_unresolved(PPCContext& ctx, uint32_t from, uint32_t to);
}  // namespace rex::runtime

//=============================================================================
// PPC Function Macros
//=============================================================================

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
// REX_FUNC is the bare signature; the declaring site supplies linkage, either
// by writing extern "C" itself or by using REX_EXTERN.
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)

//=============================================================================
// Function Mapping
//=============================================================================

struct PPCFuncMapping {
  size_t guest;
  PPCFunc* host;
};

extern PPCFuncMapping PPCFuncMappings[];
