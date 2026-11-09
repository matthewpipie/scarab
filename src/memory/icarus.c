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
 * File         : memory/icarus.c
 * Description  : REPL_ICARUS (Kalbande, Jedhe Deshmukh, Ros & Panda, "ICARUS: Criticality
 *                 and Reuse based Instruction Caching for Datacenter Applications", ASPLOS
 *                 2026) criticality-detection engine.
 *
 * Mechanism (the per-line criticality/reuse bits, the four priority bins, eviction) lives in
 * the ICARUS section of libs/cache_lib.c. This file is the "intelligence" that decides
 * whether a filling instruction line should be marked critical, and stages that decision via
 * cache_set_icarus_next_insert immediately before that line's cache_insert, mirroring the
 * staging pattern REPL_MLP already uses (cache_set_next_fill_mlp_boost).
 *
 * The paper's Critical Instruction Table (CIT): a per-core table of 2-bit saturating
 * counters, indexed by XOR-folding the missing line's address with the branch history that
 * led to the fetch. That history is Op.fetch_ghist of the line's first op (the BP's history
 * just before predicting it, recorded in ft.cc), not g_bp_data's current history, which with
 * a decoupled front end is up to an FTQ ahead of fetch and FDIP. icarus_tag_req copies it
 * onto the line's request (Mem_Req.icarus_hist) when the demand fetch or FDIP issues it.
 * icarus_note_fetch_stall trains an entry up (saturating) once per request, the first cycle
 * that, with that request outstanding, decode was starved by it *and* the issue queue was
 * completely empty -- nothing else was around to hide the stall, so this miss was genuinely
 * on the critical path. icarus_fill looks the entry up at fill time, and icarus_mlc_hit on
 * an MLC hit (the paper's criticality signal for a resident line): at or above
 * --icarus_crit_thresh means "critical." The whole table is reset every
 * --icarus_reset_interval cycles, per the paper's finding that criticality is highly dynamic
 * (~28% of critical lines stay critical across intervals).
 *
 * --icarus_costly_fetch (§4.5, for workloads where many fetches are critical, e.g. Verilator
 * on a 1 MB L2) switches the CIT to the paper's redefinition of a critical fetch: one that
 * starves decode for more than --icarus_costly_cycles cycles (icarus_note_costly_stall).
 * While the mode is on, only those fetches train the CIT. Mode 2 is always on. Mode 1 decides
 * at each reset, from the interval that just ended: on if more than
 * --icarus_costly_trigger_pct percent of the instruction fetches that reached the MLC
 * (icarus_mlc_lookup) were critical by the §3.3 definition. That is counted per fetch (did
 * this fetch starve decode), not by looking up the CIT, so it doesn't depend on which
 * definition the CIT is trained with, and the trigger can turn the mode back off. The CIT is
 * flushed at every reset, so after a switch it only ever holds the new definition.
 *
 * Only instruction-type requests ever train or consult the CIT; data-type fills always stage
 * "not critical," per the paper's instruction-caching-only scope. Unlike REPL_LACS/REPL_MLP
 * (which can run at the MLC or the LLC), REPL_ICARUS is MLC-only -- Scarab's MLC is this
 * repo's analog of the paper's private per-core L2.
 ***************************************************************************************/

#include "memory/icarus.h"

#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "memory/memory.h"
#include "memory/mshr.h"

#include "statistics.h"

/**************************************************************************************/
/* Types */

#define ICARUS_CIT_MAX 3u

typedef struct Icarus_State_struct {
  Flag initialized;
  uns8* cit;
  uns cit_size;
  Counter last_reset_cycle;
  Flag costly_active;  // the CIT is trained by §4.5 costly fetches, not §3.3 critical fetches
  // --icarus_costly_fetch 1: this interval's instruction fetches that reached the MLC, and
  // how many of them were critical by the §3.3 definition
  Counter interval_fetches;
  Counter interval_critical_fetches;
} Icarus_State;

static Icarus_State g_icarus[MAX_NUM_PROCS];

/**************************************************************************************/
/* Helpers */

static Icarus_State* icarus_state(uns8 proc_id) {
  Icarus_State* s = &g_icarus[proc_id];
  if (!s->initialized) {
    ASSERTM(proc_id, ICARUS_BHR_BITS >= 1 && ICARUS_BHR_BITS <= 32,
            "--icarus_bhr_bits must be in [1, 32] (global_hist is 32 bits), got %u\n", ICARUS_BHR_BITS);
    ASSERTM(proc_id, ICARUS_COSTLY_FETCH <= 2, "--icarus_costly_fetch must be 0 (off), 1 (auto) or 2 (on), got %u\n",
            ICARUS_COSTLY_FETCH);
    // Default size of 512 entries
    s->cit_size = 1u << ICARUS_CIT_BITS;
    // Initialize counter array and set base of reset counter
    s->cit = (uns8*)calloc(s->cit_size, sizeof(uns8));
    s->costly_active = ICARUS_COSTLY_FETCH == 2;
    s->last_reset_cycle = cycle_count;
    s->initialized = TRUE;
  }
  return s;
}

// Calculate CIT table indexer
// The CIT only ever predicts MLC fills (ICARUS is MLC-only), so MLC_LINE_SIZE is the one
// correct granularity for its block number -- not whatever line size happens to belong to
// the cache at a given call site. Shifting here, once, means callers (icache_stage.c's
// training hook, icarus_fill's lookup) just pass a raw address and can't disagree with each
// other about granularity the way two independently-computed shifts could (e.g. if
// --icache_line_size and --mlc_line_size are ever set to different values).
static inline uns icarus_cit_index(Icarus_State* s, Addr addr, uns32 hist) {
  Addr block_num = addr >> LOG2(MLC_LINE_SIZE);
  // Scarab's global history shifts each new outcome in at bit 31 (bp.c), so the last
  // ICARUS_BHR_BITS branches are the TOP bits of hist; the low bits are the oldest ones.
  uns32 recent = hist >> (32 - ICARUS_BHR_BITS);
  Addr sig = block_num ^ (block_num >> 3) ^ recent;
  return (uns)(sig & (s->cit_size - 1));
}

// Returns TRUE if req is an instruction request
static inline Flag icarus_is_inst_req(Mem_Req* req) {
  return mem_req_is_type(req, MRT_IFETCH) || mem_req_is_type(req, MRT_IPRF) ||
         mem_req_is_type(req, MRT_UOCPRF) || mem_req_is_type(req, MRT_FDIPPRFON) ||
         mem_req_is_type(req, MRT_FDIPPRFOFF) || mem_req_is_type(req, MRT_FDIPPRFALT);
}

static inline void icarus_cit_train(Icarus_State* s, Addr addr, uns32 hist) {
  uns idx = icarus_cit_index(s, addr, hist);
  if (s->cit[idx] < ICARUS_CIT_MAX)
    s->cit[idx]++;
}

// Returns TRUE if the CIT entry for (addr, hist) is at or above the criticality threshold
static Flag icarus_lookup_critical(uns8 proc_id, Addr addr, uns32 hist) {
  Icarus_State* s = icarus_state(proc_id);
  uns idx = icarus_cit_index(s, addr, hist);
  return s->cit[idx] >= ICARUS_CRIT_THRESH;
}

// Flushes the CIT every --icarus_reset_interval cycles, checked lazily from the training (and,
// with --icarus_costly_fetch, lookup) hooks. With --icarus_costly_fetch 1 this is also where
// the §4.5 trigger decides, from the interval that just ended, whether the mode is on for the
// next one; flushing first means the CIT never mixes the two definitions.
static void icarus_maybe_reset(uns8 proc_id, Icarus_State* s) {
  if (cycle_count - s->last_reset_cycle < ICARUS_RESET_INTERVAL)
    return;
  memset(s->cit, 0, s->cit_size * sizeof(uns8));
  if (s->costly_active)
    STAT_EVENT(proc_id, MLC_ICARUS_COSTLY_INTERVAL);
  if (ICARUS_COSTLY_FETCH == 1) {
    Flag on = s->interval_critical_fetches * 100 > s->interval_fetches * ICARUS_COSTLY_TRIGGER_PCT;
    if (on != s->costly_active)
      STAT_EVENT(proc_id, MLC_ICARUS_COSTLY_SWITCH);
    s->costly_active = on;
  }
  s->interval_fetches = 0;
  s->interval_critical_fetches = 0;
  s->last_reset_cycle = cycle_count;
  STAT_EVENT(proc_id, MLC_ICARUS_CIT_RESET);
}

/**************************************************************************************/
/* Interface */

void icarus_tag_req(uns8 proc_id, Addr line_addr, uns32 hist) {
  Mshr_Line* line = mshr_line(mem_core_file(proc_id, MRT_IFETCH), line_addr);
  Mem_Req* req = line ? mshr_line_first(line) : NULL;
  // First issuer wins: a demand that merges into an FDIP prefetch keeps the prefetch's
  // (same-path) history; one that waits on an untagged request (e.g. an IPRF) supplies it.
  if (req && !req->icarus_hist_valid) {
    req->icarus_hist = hist;
    req->icarus_hist_valid = TRUE;
  }
}

void icarus_note_fetch_stall(uns8 proc_id, Addr addr, Mem_Req* req) {
  Icarus_State* s = icarus_state(proc_id);
  icarus_maybe_reset(proc_id, s);

  // Paper §3.3: one increment per critical fetch, not per starved cycle. Counting cycles
  // saturates the 2-bit entry within a single stall, so a line would be marked critical at
  // its own fill on its first starvation, defeating the "starved at least twice" filter that
  // keeps dead-on-arrival lines out of the [1,0] bin.
  if (req->icarus_trained || !req->icarus_hist_valid)
    return;
  req->icarus_trained = TRUE;

  // A §3.3 critical fetch: the §4.5 trigger's numerator, counted in every mode
  s->interval_critical_fetches++;
  STAT_EVENT(proc_id, MLC_ICARUS_CRITICAL_FETCH);
  if (s->costly_active)  // costly fetches train the CIT instead
    return;
  icarus_cit_train(s, addr, req->icarus_hist);
  STAT_EVENT(proc_id, MLC_ICARUS_CIT_TRAIN);
}

void icarus_note_costly_stall(uns8 proc_id, Addr addr, Mem_Req* req) {
  Icarus_State* s = icarus_state(proc_id);
  icarus_maybe_reset(proc_id, s);

  // §4.5: "any instruction fetch that causes decode starvation for more than 40 cycles" --
  // total starved cycles while the request is outstanding; trains once, on crossing the cost.
  req->icarus_starved_cycles++;
  if (!s->costly_active || req->icarus_costly_trained || !req->icarus_hist_valid ||
      req->icarus_starved_cycles <= ICARUS_COSTLY_CYCLES)
    return;
  req->icarus_costly_trained = TRUE;

  icarus_cit_train(s, addr, req->icarus_hist);
  STAT_EVENT(proc_id, MLC_ICARUS_COSTLY_TRAIN);
}

void icarus_mlc_lookup(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_ICARUS || !icarus_is_inst_req(req) || !req->icarus_hist_valid)
    return;
  // Don't start the CIT from here: its reset clock starts at the first training or fill, as it
  // did before this hook existed, so --icarus_costly_fetch 0 behaves exactly as without it.
  Icarus_State* s = &g_icarus[req->proc_id];
  if (!s->initialized)
    return;
  if (ICARUS_COSTLY_FETCH)
    icarus_maybe_reset(req->proc_id, s);
  s->interval_fetches++;
  STAT_EVENT(req->proc_id, MLC_ICARUS_FETCH);
}

// Computes the criticality from the CIT for req and stages globals for
// the actual insertion which happens via cache_insert in memory.c
void icarus_fill(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_ICARUS)
    return;

  STAT_EVENT(req->proc_id, MLC_ICARUS_FILL);

  // Lookup addr in CIT and determine criticality. A request no fetch has tagged (e.g. an
  // IPRF nobody has demanded yet) has no history to look up with, so it fills as non-critical.
  Flag critical = FALSE;
  if (icarus_is_inst_req(req) && req->icarus_hist_valid) {
    critical = icarus_lookup_critical(req->proc_id, req->addr, req->icarus_hist);
    if (critical)
      STAT_EVENT(req->proc_id, MLC_ICARUS_CRITICAL_FILL);
  }
  // Set globals
  cache_set_icarus_next_insert(TRUE, critical);
}

void icarus_mlc_hit(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_ICARUS || !icarus_is_inst_req(req) || !req->icarus_hist_valid)
    return;
  if (icarus_lookup_critical(req->proc_id, req->addr, req->icarus_hist) && cache_icarus_mark_critical(cache, req->addr))
    STAT_EVENT(req->proc_id, MLC_ICARUS_HIT_MARK_CRITICAL);
}
