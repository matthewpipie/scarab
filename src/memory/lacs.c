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
 * File         : memory/lacs.c
 * Description  : REPL_LACS (Kharbutli & Sheikh, "LACS: A Locality-Aware Cost-Sensitive
 *                 Cache Replacement Algorithm", IEEE TC 2014) cost-estimation engine.
 *
 * Mechanism (eviction, aging, the per-line cost_current field) lives in the REPL_LACS
 * section of libs/cache_lib.c. This file estimates costs and stages each fill's
 * cost_current via cache_set_lacs_next_insert immediately before that line's cache_insert.
 * Step names follow the paper's Table 1:
 *
 *  A/B  A miss to block B at this level (lacs_note_miss): snapshot the issuing core's
 *       Issued Instructions Counter (IIC, kept by lacs_note_issue) into the request's IIR.
 *       Only the first miss snapshots. The paper takes the IIR "when the load/store
 *       instruction is added to the MSHR", so a demand that merges into a prefetch's
 *       in-flight miss retakes it from the merge (lacs_note_demand_merge).
 *  D/E  The miss returns to this level (lacs_miss_return): numIssued = IIC - IIR. If
 *       numIssued < thresh the miss is high-cost: the block's 2-bit history entry
 *       (cost_stored) is incremented and totalHigh counted (E1); otherwise the entry is
 *       decremented (E2). This happens whether or not the line is then inserted here, so an
 *       exclusive (victim-filled) level, which skips the insert, still learns the cost.
 *       Prefetches do not train (paper section 4.5) unless --lacs_train_prefetch.
 *  F    Insert (lacs_fill): cost_current = the block's cost_stored. The paper initializes
 *       every fill from the history table ("all cost_current values would be initialized
 *       to 1 on cache fills instead of the corresponding cost_stored values", Op2), so by
 *       default prefetch and writeback fills read the table too; they just never train it.
 *       A writeback is how a victim-filled LLC receives a block, so this is where that
 *       level's trained cost takes effect. --lacs_{pref,wb}_insert_lookup off swaps in a
 *       fixed cost.
 *  C, G Victim search, aging and hit promotion: cache_lib.c.
 *
 * Optimizations, every 1 << --lacs_period_log2 trained misses (paper: 16K), per core per
 * level:
 *  Op1  If fewer than period >> 5 (1/32) of the period's trained misses were high-cost,
 *       thresh rises by --lacs_thresh_step (paper: 8); if more than period >> 4 (1/16), it
 *       drops by the same step; in between it stays put. Reserving only a small fraction of
 *       blocks is what makes LACS thrash-resistant (paper section 4.2).
 *  Op2  --lacs_cs_mode 0 (paper): cost-sensitivity stays on while this period's average
 *       numIssued is within --lacs_cs_tolerance (paper: 10) of the previous period's. While
 *       it is off, every fill inserts at cost 1, and learning continues. 1 forces it on, 2
 *       forces it off.
 *
 * Choices the paper leaves open:
 *  - The period counts misses that train, not every miss (the paper's Step A missCount).
 *    In the paper every miss trains, and the period average is sumIssued >> 14, which is
 *    only an average if the two counts agree; with prefetchers they would not.
 *  - The starting thresh (--lacs_thresh_init) is not given in the paper.
 *  - IIC counts Scarab ops (uops) at execution, replays and off-path ops included -- what a
 *    hardware issue counter sees.
 *
 * Per-core, per-level (Lacs_Slot) state, as in the paper's section 4.4, since training is
 * driven by that core's own issue stream even when the cache a level names is shared.
 ***************************************************************************************/

#include "memory/lacs.h"

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "memory/memory.h"

#include "op.h"
#include "statistics.h"

/**************************************************************************************/
/* Types */

#define LACS_COST_HIGH 3u
#define LACS_COST_LOW 0u

/* Paper section 4.2: minHigh = period / 32, maxHigh = period / 16. */
#define LACS_MIN_HIGH_SHIFT 5
#define LACS_MAX_HIGH_SHIFT 4

typedef struct Lacs_Train_State_struct {
  Flag initialized;

  /* Adaptive threshold on numIssued: below it, a fill is classified "high cost". */
  Counter threshold;
  Flag cost_sensitive;

  /* Current period accumulators (trained fills only). */
  Counter period_fills;
  Counter period_high;
  Counter period_issued_sum;

  /* Previous period's average numIssued, for the cost-sensitivity stability check. */
  Counter prev_period_avg;
  Flag have_prev_avg;

  /* Per-line-address history (the paper's cost_stored): a 2-bit saturating counter per
     (XOR-folded) index, trained up on a high-cost miss and down on a low-cost one. */
  uns8* hist;
  uns hist_size;

  Counter final_threshold;
} Lacs_Train_State;

static Lacs_Train_State g_lacs_train[MAX_NUM_PROCS][LACS_NUM_SLOTS];
static Counter g_lacs_iic[MAX_NUM_PROCS]; /* instructions issued, running per-core count */

/**************************************************************************************/
/* Helpers */

static inline Flag lacs_cache_active(Cache* cache) {
  return (cache->repl_policy == REPL_LACS) || LACS_PROFILE;
}

/* A type with no meaningful "exposure" signal of its own: a writeback didn't stall any
   consumer, and (when --lacs_data_only) neither did an instruction-side miss. */
static inline Flag lacs_type_eligible(Mem_Req_Type type) {
  if (type == MRT_WB || type == MRT_WB_NODIRTY)
    return FALSE;
  if (LACS_DATA_ONLY && (type == MRT_IFETCH || type == MRT_IPRF || type == MRT_UOCPRF || type == MRT_FDIPPRFON ||
                         type == MRT_FDIPPRFOFF || type == MRT_FDIPPRFALT))
    return FALSE;
  return TRUE;
}

static Lacs_Train_State* lacs_train_state(uns8 proc_id, Lacs_Slot slot) {
  Lacs_Train_State* s = &g_lacs_train[proc_id][slot];
  if (!s->initialized) {
    s->threshold = LACS_THRESH_INIT;
    s->cost_sensitive = (LACS_CS_MODE != 2);
    s->hist_size = 1u << LACS_HIST_BITS;
    s->hist = (uns8*)malloc(s->hist_size * sizeof(uns8));
    /* Paper section 4.3: every entry starts at 1 (low cost, low confidence). */
    for (uns ii = 0; ii < s->hist_size; ii++)
      s->hist[ii] = 1;
    s->initialized = TRUE;
  }
  return s;
}

// Calculate the xor folded index into the history table
static inline uns lacs_hist_index(Lacs_Train_State* s, Addr line_addr) {
    Addr folded = 0;
    // Split into LACS_HIST_BIT parts and xor them together.
    // After the final split, the next shift will bring in all 0's hence
    // we stop when v == 0.
    for (Addr v = line_addr; v != 0; v >>= LACS_HIST_BITS)
      folded ^= v;
    return (uns)(folded & (s->hist_size - 1));
  }

/* Re-evaluates the threshold and the cost-sensitivity switch from the period just closed,
   then resets the accumulators for the next one. */
static void lacs_end_period(Lacs_Train_State* s, Lacs_Slot slot, uns8 proc_id) {
  Counter avg_issued = s->period_issued_sum / s->period_fills;
  /* Op1 (paper section 4.2, Table 1): keep the number of high-cost fills between minHigh and
     maxHigh (1/32 and 1/16 of the period in the paper), with a fixed step and no change in
     between. */
  Counter min_high = s->period_fills >> LACS_MIN_HIGH_SHIFT;
  Counter max_high = s->period_fills >> LACS_MAX_HIGH_SHIFT;

  if (s->period_high < min_high) {
    s->threshold += LACS_THRESH_STEP;
    STAT_EVENT(proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_THRESH_UP : L1_LACS_THRESH_UP);
  } else if (s->period_high > max_high) {
    /* thresh_old - step, saturating at 0 like the 32-bit register it models. */
    s->threshold = (s->threshold > LACS_THRESH_STEP) ? s->threshold - LACS_THRESH_STEP : 0;
    STAT_EVENT(proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_THRESH_DOWN : L1_LACS_THRESH_DOWN);
  }

  if (LACS_CS_MODE == 1) {
    s->cost_sensitive = TRUE;
  } else if (LACS_CS_MODE == 2) {
    s->cost_sensitive = FALSE;
  } else if (s->have_prev_avg) {
    Counter diff =
        (avg_issued > s->prev_period_avg) ? avg_issued - s->prev_period_avg : s->prev_period_avg - avg_issued;
    s->cost_sensitive = diff <= LACS_CS_TOLERANCE; /* Op2: absolute tolerance, as in the paper */
  } else {
    s->cost_sensitive = TRUE; /* optimistic until there are two periods to compare */
  }
  STAT_EVENT(proc_id, slot == LACS_SLOT_MLC ? (s->cost_sensitive ? MLC_LACS_CS_ON : MLC_LACS_CS_OFF)
                                            : (s->cost_sensitive ? L1_LACS_CS_ON : L1_LACS_CS_OFF));

  s->prev_period_avg = avg_issued;
  s->have_prev_avg = TRUE;
  s->period_fills = 0;
  s->period_high = 0;
  s->period_issued_sum = 0;
}

/**************************************************************************************/
/* Interface */

void lacs_note_issue(Op* op) {
  if (LACS_ONPATH_ONLY && op->off_path)
    return;
  g_lacs_iic[op->proc_id]++;
}

void lacs_note_miss(Lacs_Slot slot, Cache* cache, Mem_Req* req) {
  if (!lacs_cache_active(cache))
    return;
  if (req->lacs_snap[slot] != LACS_SNAP_NONE)
    return; /* already snapshotted for this slot */
  if (!lacs_type_eligible(req->type))
    return;

  Flag is_prefetch = mem_req_type_is_prefetch(req->type) && !req->demand_match_prefetch;
  req->lacs_snap[slot] = is_prefetch ? LACS_SNAP_PREFETCH : LACS_SNAP_DEMAND;
  req->lacs_iir[slot] = g_lacs_iic[req->proc_id];
}

void lacs_note_demand_merge(Lacs_Slot slot, Cache* cache, Mem_Req* req, Mem_Req_Type new_type) {
  if (!lacs_cache_active(cache))
    return;
  if (req->lacs_snap[slot] != LACS_SNAP_PREFETCH)
    return; /* no prefetch-only snapshot to upgrade */
  if (!lacs_type_eligible(new_type))
    return;

  req->lacs_snap[slot] = LACS_SNAP_DEMAND;
  req->lacs_iir[slot] = g_lacs_iic[req->proc_id];
}

void lacs_miss_return(Lacs_Slot slot, Cache* cache, Mem_Req* req) {
  Lacs_Snap snap = (Lacs_Snap)req->lacs_snap[slot];
  if (snap != LACS_SNAP_DEMAND && snap != LACS_SNAP_PREFETCH)
    return; /* no miss outstanding at this level, or its return was already counted */
  if (!lacs_cache_active(cache))
    return;

  if (snap == LACS_SNAP_PREFETCH && !LACS_TRAIN_PREFETCH) {
    /* Section 4.5: a prefetch did not stall the core, so it has no numIssued to train on. */
    req->lacs_snap[slot] = LACS_SNAP_UNTRAINED;
    return;
  }

  Lacs_Train_State* s = lacs_train_state(req->proc_id, slot);
  uns idx = lacs_hist_index(s, req->addr >> cache->shift_bits);

  /* Step D */
  Counter num_issued = g_lacs_iic[req->proc_id] - req->lacs_iir[slot];
  Flag high_cost = num_issued < s->threshold;

  /* Steps E1/E2: the history entry moves one step per miss (3/2 = high cost, 1/0 = low;
     3 and 0 are the confident ends). */
  uns8* stored = &s->hist[idx];
  if (high_cost) {
    if (*stored < LACS_COST_HIGH)
      (*stored)++;
  } else if (*stored > LACS_COST_LOW) {
    (*stored)--;
  }

  s->period_fills++;
  s->period_issued_sum += num_issued;
  if (high_cost) {
    s->period_high++;
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_HIGH_COST : L1_LACS_HIGH_COST);
  } else {
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_LOW_COST : L1_LACS_LOW_COST);
  }
  STAT_EVENT(req->proc_id, snap == LACS_SNAP_DEMAND
                               ? (slot == LACS_SLOT_MLC ? MLC_LACS_TRAIN_DEMAND : L1_LACS_TRAIN_DEMAND)
                               : (slot == LACS_SLOT_MLC ? MLC_LACS_TRAIN_PREFETCH : L1_LACS_TRAIN_PREFETCH));

  if (s->period_fills >= ((Counter)1 << LACS_PERIOD_LOG2))
    lacs_end_period(s, slot, req->proc_id);

  req->lacs_snap[slot] = LACS_SNAP_TRAINED;
}

void lacs_fill(Lacs_Slot slot, Cache* cache, Mem_Req* req) {
  /* --lacs_profile trains a cache that runs another policy; nothing there would consume a
     staged cost, and a leftover one would land on the next REPL_LACS insert. */
  if (cache->repl_policy != REPL_LACS)
    return;

  Lacs_Snap snap = (Lacs_Snap)req->lacs_snap[slot];
  ASSERTM(req->proc_id, snap != LACS_SNAP_DEMAND && snap != LACS_SNAP_PREFETCH,
          "lacs_fill on %s before lacs_miss_return (addr 0x%llx)\n", cache->name, (unsigned long long)req->addr);

  Lacs_Train_State* s = lacs_train_state(req->proc_id, slot);
  uns idx = lacs_hist_index(s, req->addr >> cache->shift_bits);
  Flag valid;
  uns cost;

  STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_FILL : L1_LACS_FILL);

  /* Step F: cost_current = cost_stored. Prefetches and writebacks read the table without
     having trained it, unless their *_insert_lookup knob swaps in a fixed cost. */
  if (req->type == MRT_WB || req->type == MRT_WB_NODIRTY) {
    valid = TRUE;
    cost = LACS_WB_INSERT_LOOKUP ? s->hist[idx] : LACS_WB_INSERT_COST;
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_WB_FILL : L1_LACS_WB_FILL);
  } else if (snap == LACS_SNAP_TRAINED) {
    valid = TRUE;
    cost = s->hist[idx];
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_TRAINED_FILL : L1_LACS_TRAINED_FILL);
  } else if (snap == LACS_SNAP_UNTRAINED || mem_req_type_is_prefetch(req->type)) {
    valid = TRUE;
    cost = LACS_PREF_INSERT_LOOKUP ? s->hist[idx] : LACS_PREF_INSERT_COST;
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_PREF_FILL : L1_LACS_PREF_FILL);
  } else {
    /* A demand with no snapshot (e.g. --lacs_data_only excluded it): cache_lib.c's default
       cost, the paper's 1. */
    valid = FALSE;
    cost = 0;
  }

  /* Op2: while cost-sensitivity is off, every fill inserts at the default cost of 1. */
  if (!s->cost_sensitive)
    valid = FALSE;

  cache_set_lacs_next_insert(valid, cost);
}

void lacs_done(uns8 proc_id) {
  for (Lacs_Slot slot = LACS_SLOT_MLC; slot < LACS_NUM_SLOTS; slot++) {
    Lacs_Train_State* s = &g_lacs_train[proc_id][slot];
    if (!s->initialized)
      continue;
    s->final_threshold = s->threshold;

    Cache* cache = mem_lacs_cache(proc_id, slot);
    Counter evictions = 0, evict_aging = 0, evict_ties = 0;
    /* Under --lacs_profile the cache may run another policy, whose predictor is not a
       Lacs_State. */
    if (cache->repl_policy == REPL_LACS)
      cache_lacs_counters(cache, &evictions, &evict_aging, &evict_ties);

    if (slot == LACS_SLOT_MLC) {
      INC_STAT_EVENT(proc_id, MLC_LACS_FINAL_THRESHOLD, s->final_threshold);
      INC_STAT_EVENT(proc_id, MLC_LACS_EVICTIONS, evictions);
      INC_STAT_EVENT(proc_id, MLC_LACS_EVICT_AGING, evict_aging);
      INC_STAT_EVENT(proc_id, MLC_LACS_EVICT_TIES, evict_ties);
    } else {
      INC_STAT_EVENT(proc_id, L1_LACS_FINAL_THRESHOLD, s->final_threshold);
      INC_STAT_EVENT(proc_id, L1_LACS_EVICTIONS, evictions);
      INC_STAT_EVENT(proc_id, L1_LACS_EVICT_AGING, evict_aging);
      INC_STAT_EVENT(proc_id, L1_LACS_EVICT_TIES, evict_ties);
    }
  }
}
