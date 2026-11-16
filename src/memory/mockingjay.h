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
 * File         : memory/mockingjay.h
 * Description  : REPL_MOCKINGJAY (Shah, Jain & Lin, "Effective Mimicry of Belady's MIN
 *                 Policy", HPCA 2022), ported from the authors' ChampSim CRC2 artifact
 *                 (mockingjay.llc_repl). Runs at the MLC or the L1 (Scarab's LLC); see the
 *                 top of mockingjay.c.
 ***************************************************************************************/

#ifndef __MOCKINGJAY_H__
#define __MOCKINGJAY_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"
#include "memory/mem_req.h"

#include "statistics.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/
/* The strategy-table entry (libs/cache_lib.c). update_hit does nothing: a hit is trained by
   mockingjay_hit, once per request, because cache_access runs again on every retry of a
   lookup. update_insert consumes what mockingjay_fill staged. */

void mockingjay_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                            Repl_Policy repl_policy);
void mockingjay_update_hit(Cache* cache, uns set, uns way, void* arg);
void mockingjay_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* mockingjay_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

/**************************************************************************************/
/* The memory system's side. Each is a no-op on a cache that does not run REPL_MOCKINGJAY. */

/* After init_cache: how many cores share the cache (1 picks the paper's single-core variant,
   more its multi-core one) and the first stat of the level's MJ block in memory.stat.def. */
void mockingjay_configure(Cache* cache, uns sharers, Stat_Enum stat_base);

/* A request's lookup hit the cache and is being served: train and re-predict the line.
   Call once per request. Writebacks change nothing, as in the artifact. */
void mockingjay_hit(Cache* cache, Mem_Req* req);

/* A fill is about to insert req's line, every other check done. If the predictor says the
   line will be reused after everything already in its (full) set, train as the artifact does
   for a bypass and return TRUE: the caller must then not insert the line. Never bypasses a
   writeback, a fill into a set with a free way, or (dirty) a line this level must keep.
   With mlp_mj_boost, `boosted` says the fill will be boosted (marked, with a nonzero gamma): it is
   then never bypassed, and an unboosted fill competes only with the set's unprotected lines. */
Flag mockingjay_bypass(Cache* cache, Mem_Req* req, Flag dirty, Flag boosted);

/* mockingjay_bypass in two steps, for a caller that has more to do before it can commit to the
   bypass and may have to retry: _check decides and changes nothing but what the stats of _commit
   or mockingjay_fill read; _commit trains (call it once, after _check returned TRUE and only when
   the line is really not inserted). */
Flag mockingjay_bypass_check(Cache* cache, Mem_Req* req, Flag dirty, Flag boosted);
void mockingjay_bypass_commit(Cache* cache, Mem_Req* req);

/* Stage req for the cache_insert that immediately follows, which trains and predicts the new
   line. Call after mockingjay_bypass returned FALSE. */
void mockingjay_fill(Cache* cache, Mem_Req* req);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __MOCKINGJAY_H__ */
