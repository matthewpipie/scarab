/* Copyright 2020 HPS/SAFARI Research Groups
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
 * File         : memory/lacs.h
 * Description  : REPL_LACS (Kharbutli & Sheikh, IEEE TC 2014) cost-estimation engine.
 *                 Level-agnostic: callers pass which of the two slots (MLC, L1) they're
 *                 driving. A no-op for any cache that isn't actually running REPL_LACS
 *                 (checked internally via cache->repl_policy), unless --lacs_profile asks
 *                 for the training stats anyway.
 ***************************************************************************************/

#ifndef __LACS_H__
#define __LACS_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"
#include "memory/mem_req.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/

/* Every execution attempt of every op -- increments the issuing core's instruction-issue
   counter (the paper's IIC) that lacs_note_miss/lacs_note_demand_merge snapshot from. Off-path
   ops count too unless --lacs_onpath_only, an oracle knob. */
void lacs_note_issue(Op* op);

/* First miss of `req` at this slot's level: snapshot the issuing core's issue counter.
   A no-op if this slot already holds a snapshot for `req` (repeat calls for the same
   still-missing request). */
void lacs_note_miss(Lacs_Slot slot, Cache* cache, Mem_Req* req);

/* A demand of `new_type` just merged into `req`, which may have only been snapshotted as
   a prefetch so far -- re-snapshot as a demand if so. */
void lacs_note_demand_merge(Lacs_Slot slot, Cache* cache, Mem_Req* req, Mem_Req_Type new_type);

/* `req`'s miss at this slot's level has returned (paper Steps D/E): train this slot's
   history table from the snapshot, if `req` has one that trains, and close the period
   (Op1/Op2) when it fills up. Call it when the data reaches the level, before deciding
   whether to insert there: an exclusive level that skips the insert still learns the
   miss's cost. Idempotent, so a fill that is refused and retried trains once. */
void lacs_miss_return(Lacs_Slot slot, Cache* cache, Mem_Req* req);

/* Paper Step F: stage the cost the immediately-following cache_insert stamps on `req`'s
   line, via cache_set_lacs_next_insert. Must be the last thing called before that insert,
   and only after lacs_miss_return for the same request. A no-op unless the cache runs
   REPL_LACS. */
void lacs_fill(Lacs_Slot slot, Cache* cache, Mem_Req* req);

/* Once per core, at the end of that core's run: final-threshold and eviction-counter
   stats for every distinct cache LACS drove on this core's behalf. */
void lacs_done(uns8 proc_id);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __LACS_H__ */
