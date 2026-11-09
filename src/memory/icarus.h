/* Copyright 2026 HPS/SAFARI Research Groups
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
 * File         : memory/icarus.h
 * Description  : REPL_ICARUS (Kalbande, Jedhe Deshmukh, Ros & Panda, "ICARUS: Criticality
 *                 and Reuse based Instruction Caching for Datacenter Applications", ASPLOS
 *                 2026) criticality-detection engine. MLC-only (Scarab's MLC is this repo's
 *                 analog of the paper's private per-core L2). A no-op for any cache that
 *                 isn't actually running REPL_ICARUS.
 ***************************************************************************************/

#ifndef __ICARUS_H__
#define __ICARUS_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"
#include "memory/mem_req.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/

/* Right after a successful instruction new_mem_req for `line_addr` (demand fetch or FDIP):
   records `hist` -- Op.fetch_ghist of the line's first op, i.e. the branch history leading
   to the fetch -- on the request that will go to memory for the line, unless a request
   already carries one. The CIT is indexed with this history at training, fill and hit. */
void icarus_tag_req(uns8 proc_id, Addr line_addr, uns32 hist);

/* Called every cycle decode was starved by the outstanding instruction-fetch request `req`
   for `addr` and the issue queue was completely empty (nothing else was hiding the stall),
   but trains the per-core Critical Instruction Table (CIT) entry for (addr, req's history)
   only once per request: the paper counts critical fetches, not starved cycles. (While
   --icarus_costly_fetch mode is on, it only counts the fetch for the mode's trigger.) Lazily
   resets the whole CIT every --icarus_reset_interval cycles. `addr` is a raw address (e.g.
   ic->line_addr) -- the CIT hashes at MLC_LINE_SIZE granularity internally, so callers
   don't need to shift and can't disagree with icarus_fill's lookup about what granularity
   to use. */
void icarus_note_fetch_stall(uns8 proc_id, Addr addr, Mem_Req* req);

/* --icarus_costly_fetch only (§4.5): called every cycle decode was starved by the outstanding
   instruction-fetch request `req` for `addr`, whether or not the issue queue was empty. Counts
   the request's starved cycles and, once they exceed --icarus_costly_cycles while the mode is
   on, trains the CIT entry for (addr, req's history), once per request. */
void icarus_note_costly_stall(uns8 proc_id, Addr addr, Mem_Req* req);

/* An instruction request's first lookup of the MLC: counts it as a fetch, the denominator of
   the --icarus_costly_fetch 1 trigger (MLC_ICARUS_FETCH). icarus_note_fetch_stall counts the
   numerator, the §3.3 critical fetches (MLC_ICARUS_CRITICAL_FETCH). */
void icarus_mlc_lookup(Cache* cache, Mem_Req* req);

/* Fill time for the MLC: looks up the CIT for the filling line (instruction-type requests
   only; data lines are always "not critical") and stages the resulting criticality bit for
   the immediately-following cache_insert via cache_set_icarus_next_insert. Must be the last
   thing called before that insert. */
void icarus_fill(Cache* cache, Mem_Req* req);

/* MLC hit by an instruction request: the CIT's criticality signal for a line that is already
   resident (paper Fig. 10: [0,0]->[1,0], [0,1]->[1,1]). Sets the line's critical bit if the
   CIT predicts critical; never clears it. Call after the hit's cache_access, which has
   already set the reuse bit. */
void icarus_mlc_hit(Cache* cache, Mem_Req* req);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __ICARUS_H__ */
