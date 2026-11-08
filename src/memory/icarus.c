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
 * led to the fetch (icache_stage.c snapshots the latter onto the request's `ghist` field,
 * extending the existing REPL_MLP-adjacent FDIP-only capture in memory.c's new_mem_req).
 * icarus_note_fetch_stall trains an entry up (saturating) whenever, on a cycle an
 * instruction-fetch miss is outstanding, decode was starved by it *and* the issue queue was
 * completely empty -- nothing else was around to hide the stall, so this miss was genuinely
 * on the critical path. icarus_fill looks the entry up at fill time: at or above
 * --icarus_crit_thresh means "critical." The whole table is reset every
 * --icarus_reset_interval cycles, per the paper's finding that criticality is highly dynamic
 * (~28% of critical lines stay critical across intervals).
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

#include "statistics.h"

/**************************************************************************************/
/* Types */

#define ICARUS_CIT_MAX 3u

typedef struct Icarus_State_struct {
  Flag initialized;
  uns8* cit;
  uns cit_size;
  Counter last_reset_cycle;
} Icarus_State;

static Icarus_State g_icarus[MAX_NUM_PROCS];

/**************************************************************************************/
/* Helpers */

static Icarus_State* icarus_state(uns8 proc_id) {
  Icarus_State* s = &g_icarus[proc_id];
  if (!s->initialized) {
    ASSERTM(proc_id, ICARUS_BHR_BITS >= 1 && ICARUS_BHR_BITS <= 32,
            "--icarus_bhr_bits must be in [1, 32] (global_hist is 32 bits), got %u\n", ICARUS_BHR_BITS);
    // Default size of 512 entries
    s->cit_size = 1u << ICARUS_CIT_BITS;
    // Initialize counter array and set base of reset counter
    s->cit = (uns8*)calloc(s->cit_size, sizeof(uns8));
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

// Returns true if CIT table entry is > threshold for criticality
static Flag icarus_lookup_critical(uns8 proc_id, Addr addr, uns32 hist) {
  Icarus_State* s = icarus_state(proc_id);
  uns idx = icarus_cit_index(s, addr, hist);
  return s->cit[idx] >= ICARUS_CRIT_THRESH;
}

/**************************************************************************************/
/* Interface */

void icarus_note_fetch_stall(uns8 proc_id, Addr addr, uns64 bhr) {
  Icarus_State* s = icarus_state(proc_id);

  // Check if we should reset the CIT table
  if (cycle_count - s->last_reset_cycle >= ICARUS_RESET_INTERVAL) {
    memset(s->cit, 0, s->cit_size * sizeof(uns8));
    s->last_reset_cycle = cycle_count;
    STAT_EVENT(proc_id, MLC_ICARUS_CIT_RESET);
  }

  // Increment counter because we are critical
  uns idx = icarus_cit_index(s, addr, bhr);
  if (s->cit[idx] < ICARUS_CIT_MAX)
    s->cit[idx]++;

  STAT_EVENT(proc_id, MLC_ICARUS_CIT_TRAIN);
}

// Computes the criticality from the CIT for req and stages globals for
// the actual insertion which happens via cache_insert in memory.c
void icarus_fill(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_ICARUS)
    return;

  STAT_EVENT(req->proc_id, MLC_ICARUS_FILL);

  // Lookup addr in CIT and determine criticality
  Flag critical = FALSE;
  if (icarus_is_inst_req(req)) {
    critical = icarus_lookup_critical(req->proc_id, req->addr, req->ghist);
    if (critical)
      STAT_EVENT(req->proc_id, MLC_ICARUS_CRITICAL_FILL);
  }
  // Set globals
  cache_set_icarus_next_insert(TRUE, critical);
}
