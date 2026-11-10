/*
 * Copyright 2026 University of California Santa Cruz
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

/***************************************************************************************
 * File         : memory/mlp_oracle.h
 * Author       : Litz Lab
 * Date         : 11/2026
 * Description  : mlp_oracle: upper bounds and a random control for REPL_MLP's marking at
 *                the MLC, and reuse-distance stats for marked lines. Simulator-only: the
 *                marking reads only what the core sees, but forcing hits is not something
 *                hardware can do. See mlp_oracle in memory.param.def.
 ***************************************************************************************/

#ifndef __MLP_ORACLE_H__
#define __MLP_ORACLE_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"
#include "memory/mem_req.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sides of the hierarchy, as bits: what mlp_oracle_mark and mlp_oracle_pool take. */
#define MLP_SIDE_INSTR 1
#define MLP_SIDE_DATA 2

/* The side req's line is on: instruction for fetches and instruction prefetches, data for
   loads, stores, data prefetches and writebacks. */
uns mlp_req_side(Mem_Req* req);

/* The MLC looked req's line up; hit is whether the line was there. Once per request, and never
   for a writeback. in_flight is whether an earlier miss on the line is still on its way back, so
   this one will wait on it. Records the reuse stats, and returns whether mlp_oracle forces the
   access to hit. */
Flag mlp_oracle_lookup(Cache* mlc, Mem_Req* req, Flag hit, Flag in_flight);

/* req's MLC miss has come back; marked_sides is what REPL_MLP's marking says about it
   (MLP_SIDE_* bits, 0 for unmarked). The line has now been seen, so later accesses are reuses.
   Safe to repeat for the same request. */
void mlp_oracle_miss_return(Cache* mlc, Mem_Req* req, uns marked_sides);

/* The MLC inserted req's line: any fill, writebacks and forced installs included. */
void mlp_oracle_insert(Cache* mlc, Mem_Req* req);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __MLP_ORACLE_H__ */
