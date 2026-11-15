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
 * File         : memory/emissary.c
 * Description  : EMISSARY (Nagendra, Godala, Chaturvedi, Patel, Kanev, Moseley, Stark, Pokam,
 *                 Campanoni & August, "EMISSARY: Enhanced Miss Awareness Replacement Policy
 *                 for L2 Instruction Caching", ISCA 2023).
 *
 * EMISSARY keeps the instruction lines whose misses starve decode in the L2 longer. A line gets
 * a priority bit P at its icache fill, P reaches the L2 when the icache evicts the line, and the
 * L2 protects up to N lines with P = 1 per set. This file does the first two steps; the L2's
 * eviction (the paper's P(N) treatment, Algorithm 1) is REPL_EMISSARY in libs/cache_lib.c.
 * Scarab's MLC is the paper's L2.
 *
 * Mode selection (§4.1, Table 1), once per icache insertion, from what happened while the line's
 * miss was outstanding:
 *   S  decode starved on the miss: some cycle, fetch delivered no ops because the uop cache and
 *      the icache both missed and it was waiting on this line (TD_FE_ICACHE_MISS, the cycle PDIP
 *      counts as a starvation cycle; Scarab's own DECODE_STAGE_STARVED is "decode's input was
 *      empty").
 *   E  in one of those cycles the issue queue was also empty (same cycle, as PDIP and ICARUS
 *      check it).
 *   R(1/d) then a selected miss gets P = 1 with probability 1/d (a per-core xorshift64 with a
 *      fixed seed, so runs are deterministic).
 * --emissary_select 0/1/2 picks none/S/S&E before R. The defaults, S&E with d = 32 and N = 8, are
 * the paper's preferred P(8):S&E&R(1/32).
 *
 * The starvation is charged to the request the icache stage finds for ic->line_addr in the
 * icache's MSHR file (mshr_line_first: the demand fetch, or the FDIP prefetch it merged into),
 * and at the fill the flags of every request waiting on the line are OR'd. Every icache fill
 * is a candidate, demand or prefetch: a prefetch that fetch never waited on has neither flag.
 * An L1I miss that hits in the MLC can starve decode and be selected too. No off-path gate:
 * hardware can't tell.
 *
 * The path to the L2 (§3): "A line's priority is only communicated to L2 cache once it is
 * evicted from the L1I cache." When the icache evicts a line with P = 1, P is OR'd into the
 * MLC's copy: metadata only, no request and no timing. It is an OR because P "does not change as
 * long as the line remains in either cache". The paper's L2 is inclusive; Scarab's MLC is not
 * (no back-invalidation), so the MLC may have dropped the line already, and then the bit is
 * lost (EMISSARY_MLC_MARK_ABSENT). An MLC exclusive of the icache has no copy to mark at all
 * and is refused at init (P would have to ride the demoted line).
 *
 * --emissary_reset_interval: the §6 reset, clearing every P bit in the icache and the MLC every
 * that many retired instructions. --emissary_passive selects and carries P bits under any MLC
 * policy; only REPL_EMISSARY reads them, so it must leave timing unchanged.
 ***************************************************************************************/

#include "memory/emissary.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "memory/memory.h"
#include "memory/mshr.h"

#include "cmp_model.h"
#include "statistics.h"

/**************************************************************************************/
/* Types */

#define EMISSARY_RNG_SEED 0x5DEECE66D2B7E151ULL
#define EMISSARY_CENSUS_BUCKETS (EMISSARY_END_SETS_P16 - EMISSARY_END_SETS_P00 + 1)

typedef struct Emissary_Core_struct {
  Flag initialized;
  uns64 rng;          /* R(1/d)'s generator */
  Counter next_reset; /* retired instructions at the next --emissary_reset_interval reset */
} Emissary_Core;

static Emissary_Core g_emissary[MAX_NUM_PROCS];

/**************************************************************************************/
/* Helpers */

static Emissary_Core* emissary_core(uns8 proc_id) {
  Emissary_Core* c = &g_emissary[proc_id];
  if (!c->initialized) {
    c->rng = EMISSARY_RNG_SEED ^ ((uns64)(proc_id + 1) * 0x9E3779B97F4A7C15ULL);
    ASSERT(proc_id, c->rng != 0);
    c->next_reset = EMISSARY_RESET_INTERVAL;
    c->initialized = TRUE;
  }
  return c;
}

/* xorshift64: never 0 once seeded away from it */
static inline uns64 emissary_rng_next(Emissary_Core* c) {
  uns64 x = c->rng;
  x ^= x << 13;
  x ^= x >> 7;
  x ^= x << 17;
  c->rng = x;
  return x;
}

/* Whether the miss `req` filled is selected for P = 1 (Table 1). */
static Flag emissary_select_fill(uns8 proc_id, Mem_Req* req) {
  Flag s = req->emissary_s;
  Flag se = req->emissary_se;
  Mshr_Line* line = mshr_line(mem_core_file(proc_id, MRT_IFETCH), req->addr);
  uns n = line ? mshr_line_count(line) : 0;
  if (n) {
    Mem_Req* reqs[n];
    mshr_line_reqs(line, reqs);
    for (uns i = 0; i < n; i++) {
      s |= reqs[i]->emissary_s;
      se |= reqs[i]->emissary_se;
    }
  }
  STAT_EVENT(proc_id, EMISSARY_L1I_FILL);
  if (s)
    STAT_EVENT(proc_id, EMISSARY_L1I_FILL_S);
  if (se)
    STAT_EVENT(proc_id, EMISSARY_L1I_FILL_SE);

  Flag selected = EMISSARY_SELECT == 0 ? TRUE : EMISSARY_SELECT == 1 ? s : se;
  if (selected && EMISSARY_RAND_DENOM > 1)
    selected = (emissary_rng_next(emissary_core(proc_id)) >> 11) % EMISSARY_RAND_DENOM == 0;
  return selected;
}

/**************************************************************************************/
/* Interface */

Flag emissary_on(uns8 proc_id) {
  Cache* mlc = mem_mlc_cache(proc_id);
  return mlc && (mlc->repl_policy == REPL_EMISSARY || EMISSARY_PASSIVE);
}

void emissary_note_fetch_starve(uns8 proc_id, Mem_Req* req, Flag iq_empty, Flag off_path) {
  req->emissary_s = TRUE;
  STAT_EVENT(proc_id, EMISSARY_STARVE_CYCLES);
  if (!off_path)
    STAT_EVENT(proc_id, EMISSARY_STARVE_CYCLES_ONPATH);
  if (!iq_empty)
    return;
  req->emissary_se = TRUE;
  STAT_EVENT(proc_id, EMISSARY_STARVE_IQE_CYCLES);
  if (!off_path)
    STAT_EVENT(proc_id, EMISSARY_STARVE_IQE_CYCLES_ONPATH);
}

Flag emissary_l1i_victim_p(uns8 proc_id, Cache* icache, Addr fill_addr) {
  return emissary_on(proc_id) && cache_emissary_victim_p(icache, proc_id, fill_addr);
}

void emissary_l1i_insert(uns8 proc_id, Cache* icache, Mem_Req* req, Addr fill_addr, Addr victim_addr, Flag victim_p) {
  if (!emissary_on(proc_id))
    return;

  if (victim_p && victim_addr) {
    STAT_EVENT(proc_id, EMISSARY_L1I_EVICT_HIGH);
    switch (cache_emissary_set_p(mem_mlc_cache(proc_id), victim_addr)) {
      case CACHE_EMISSARY_NEWLY_SET:
        STAT_EVENT(proc_id, EMISSARY_MLC_MARK);
        break;
      case CACHE_EMISSARY_ALREADY_SET:
        STAT_EVENT(proc_id, EMISSARY_MLC_MARK_ALREADY);
        break;
      case CACHE_EMISSARY_ABSENT:
        STAT_EVENT(proc_id, EMISSARY_MLC_MARK_ABSENT);
        break;
    }
  }

  if (req && emissary_select_fill(proc_id, req)) {
    Cache_Emissary_Mark mark = cache_emissary_set_p(icache, fill_addr);
    ASSERT(proc_id, mark != CACHE_EMISSARY_ABSENT);
    STAT_EVENT(proc_id, EMISSARY_L1I_FILL_HIGH);
  }
}

void emissary_update(uns8 proc_id) {
  if (!EMISSARY_RESET_INTERVAL || !emissary_on(proc_id))
    return;
  Emissary_Core* c = emissary_core(proc_id);
  if (inst_count[proc_id] < c->next_reset)
    return;
  c->next_reset += EMISSARY_RESET_INTERVAL;
  Counter cleared = cache_emissary_clear_all(mem_mlc_cache(proc_id)) +
                    cache_emissary_clear_all(&cmp_model.icache_stage[proc_id].icache);
  STAT_EVENT(proc_id, EMISSARY_RESET);
  INC_STAT_EVENT(proc_id, EMISSARY_RESET_LINES, cleared);
}

void emissary_done(uns8 proc_id) {
  if (!emissary_on(proc_id))
    return;
  Counter hist[EMISSARY_CENSUS_BUCKETS] = {0};
  cache_emissary_set_census(mem_mlc_cache(proc_id), hist, EMISSARY_CENSUS_BUCKETS);
  for (uns k = 0; k < EMISSARY_CENSUS_BUCKETS; k++)
    INC_STAT_EVENT(proc_id, EMISSARY_END_SETS_P00 + k, hist[k]);
}
