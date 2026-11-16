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
 * File         : memory/shippp.h
 * Description  : REPL_SHIPPP (Young, Chou, Jaleel & Qureshi, "SHiP++: Enhancing
 *                 Signature-Based Hit Predictor for Improved Cache Performance", CRC2 2017)
 *                 at the MLC. See the top of shippp.c.
 ***************************************************************************************/

#ifndef __SHIPPP_H__
#define __SHIPPP_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"
#include "memory/mem_req.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/
/* --mlc_shippp_features: one bit per enhancement of the paper's Section 3. With none set the
   policy is SHiP-PC-S, the original SHiP with the same set sampling.
     1. INSERT_MRU: a saturated (max) SHCT counter inserts at RRPV 0.
     2. FIRST_HIT: a line trains the SHCT on its first re-reference only.
     3. WB_DISTANT: writebacks insert at RRPV 3.
     4. PF_SIG: prefetches and demands get separate signatures.
     5. PF_PROMO: a demand's first hit on a prefetched line demotes it to RRPV 3, and a
        prefetch hit on one leaves its RRPV. */

#define SHIPPP_F_INSERT_MRU 0x01
#define SHIPPP_F_FIRST_HIT 0x02
#define SHIPPP_F_WB_DISTANT 0x04
#define SHIPPP_F_PF_SIG 0x08
#define SHIPPP_F_PF_PROMO 0x10
#define SHIPPP_F_ALL 0x1f

/**************************************************************************************/
/* The strategy-table entry (libs/cache_lib.c). update_hit does nothing: shippp_hit does a hit's
   work once per request, since cache_access runs again on every retry of a lookup and does
   not know the request. update_insert consumes what shippp_fill staged. */

void shippp_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                        Repl_Policy repl_policy);
void shippp_update_hit(Cache* cache, uns set, uns way, void* arg);
void shippp_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* shippp_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

/**************************************************************************************/
/* The memory system's side. Each is a no-op on a cache that does not run REPL_SHIPPP. */

/* After init_cache: how many cores share the cache (one private SHCT each) and turn on the
   MLC_SHIPPP_* stats. */
void shippp_configure(Cache* cache, uns sharers);

/* A request's lookup hit the cache and is being served: promote, demote or keep the line and
   train its signature. Call once per request. A writeback changes nothing. */
void shippp_hit(Cache* cache, Mem_Req* req);

/* Stage req for the cache_insert that immediately follows, which predicts the new line's
   RRPV and, in a sampled set, records its signature. */
void shippp_fill(Cache* cache, Mem_Req* req);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __SHIPPP_H__ */
