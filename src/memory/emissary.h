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
 * File         : memory/emissary.h
 * Description  : EMISSARY (Nagendra, Godala et al., "EMISSARY: Enhanced Miss Awareness
 *                 Replacement Policy for L2 Instruction Caching", ISCA 2023): selecting the
 *                 icache misses that get a priority bit, and carrying the bit to the MLC.
 *                 The MLC's eviction policy is REPL_EMISSARY in libs/cache_lib.c.
 ***************************************************************************************/

#ifndef __EMISSARY_H__
#define __EMISSARY_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"
#include "memory/mem_req.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Whether EMISSARY selects and carries P bits for this core: the MLC runs REPL_EMISSARY, or
   --emissary_passive. */
Flag emissary_on(uns8 proc_id);

/* Every cycle fetch delivered nothing because it is waiting on the outstanding instruction
   request `req` (decode starvation, S); iq_empty: the issue queue was empty that cycle too (E).
   Records both on the request; off_path only steers the _ONPATH stats. */
void emissary_note_fetch_starve(uns8 proc_id, Mem_Req* req, Flag iq_empty, Flag off_path);

/* The icache is about to insert the line `req` filled at `fill_addr`: returns whether its
   victim (the line the insert will evict) has P = 1, which emissary_l1i_insert then hands to
   the MLC. Call before the insert. */
Flag emissary_l1i_victim_p(uns8 proc_id, Cache* icache, Addr fill_addr);

/* Right after the icache inserted `fill_addr` for `req`, evicting `victim_addr` (0 if it took
   an invalid way), whose P bit emissary_l1i_victim_p returned: OR the victim's P into its MLC
   copy, and give the new line P = 1 if its miss is selected (--emissary_select,
   --emissary_rand_denom). req NULL: an insert no request made, which is never selected. */
void emissary_l1i_insert(uns8 proc_id, Cache* icache, Mem_Req* req, Addr fill_addr, Addr victim_addr, Flag victim_p);

/* Once per core cycle: --emissary_reset_interval. */
void emissary_update(uns8 proc_id);

/* The core is done: the end-of-run census of P lines per MLC set. */
void emissary_done(uns8 proc_id);

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __EMISSARY_H__ */
