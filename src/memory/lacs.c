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
 * Mechanism (eviction, aging, the per-line cost field) lives in the REPL_LACS section of
 * libs/cache_lib.c. This file is the "intelligence" that decides what cost a filling line
 * should be trained with, and stages it via cache_set_lacs_next_insert immediately before
 * that line's cache_insert:
 *
 *  1. A miss to block B occurs (lacs_note_miss): snapshot the issuing core's
 *     instructions-issued counter (lacs_note_issue keeps this running). Only the first
 *     miss to a given request at a given level snapshots -- a request can be re-looked-up
 *     at the same level (e.g. a merge) without resetting the clock, unless...
 *  2. ...a demand later merges into what had only been a prefetch's miss
 *     (lacs_note_demand_merge): the window that matters for cost training is "how long did
 *     an actual consumer wait", not "how long has this prefetch been in flight", so the
 *     snapshot is retaken from the merge point.
 *  3. The miss to block B returns (lacs_fill): numIssued = (issue counter now) - (issue
 *     counter at snapshot). Few instructions issued while the miss was outstanding means
 *     the core was starved waiting on it -- an "exposed" miss, worth protecting next time
 *     (trained cost: high). Many instructions issued means other in-flight work hid the
 *     latency -- trained cost: low, a fine eviction candidate. The classification threshold
 *     on numIssued adapts periodically (see below). A fill with no snapshot (a writeback,
 *     or a prefetch that was never demanded and --lacs_train_prefetch is off) gets a fixed
 *     insert cost instead of training, optionally still consulting whatever this address's
 *     history table entry already holds (--lacs_{pref,wb}_insert_lookup).
 *  4. On a hit to block B: cache_lib.c's lacs_update_hit promotes reference_val toward
 *     LACS_MAX_COST -- the mirror image of this file's "this address is usually exposed"
 *     signal, handled entirely on the mechanism side.
 *
 * Threshold and cost-sensitivity adaptation (every 1 << --lacs_period_log2 trained fills,
 * per core per level):
 *   - The fraction of this period's fills classified "high cost" is compared against a
 *     50% target. The threshold moves by --lacs_thresh_step, left-shifted by an amount in
 *     [--lacs_min_high_shift, --lacs_max_high_shift] proportional to how far off target the
 *     period was -- a small miss gets a small nudge, a large one a bigger correction.
 *   - --lacs_cs_mode selects whether the trained cost is used at all: 1 forces it on, 2
 *     forces it off (every fill gets the plain default cost, which degenerates eviction
 *     toward the aging/RNG tie-break alone), 0 (default) decides adaptively each period by
 *     comparing this period's average numIssued against the previous period's -- numIssued
 *     swinging by more than --lacs_cs_tolerance percent means the working set just changed
 *     underneath the trained history, so cost-sensitivity is switched off until a period
 *     stable enough to retrain it is observed.
 *
 * Per-core, per-level (Lacs_Slot) state, since training is driven by that core's own issue
 * stream even when the cache a level names is physically shared across cores.
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

  /* Per-line-address history: last cost trained at that (XOR-folded) index. */
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
    s->hist = (uns8*)calloc(s->hist_size, sizeof(uns8));
    s->initialized = TRUE;
  }
  return s;
}

static inline uns lacs_hist_index(Lacs_Train_State* s, Addr line_addr) {
  Addr folded = line_addr ^ (line_addr >> LACS_HIST_BITS) ^ (line_addr >> (2 * LACS_HIST_BITS));
  return (uns)(folded & (s->hist_size - 1));
}

/* Re-evaluates the threshold and the cost-sensitivity switch from the period just closed,
   then resets the accumulators for the next one. */
static void lacs_end_period(Lacs_Train_State* s, Lacs_Slot slot, uns8 proc_id) {
  Counter avg_issued = s->period_issued_sum / s->period_fills;
  uns high_permille = (uns)((s->period_high * 1000) / s->period_fills);
  const uns target_permille = 500;
  uns error = high_permille > target_permille ? high_permille - target_permille : target_permille - high_permille;
  uns shift = LACS_MIN_HIGH_SHIFT + (error * (LACS_MAX_HIGH_SHIFT - LACS_MIN_HIGH_SHIFT)) / target_permille;
  Counter step = (Counter)LACS_THRESH_STEP << shift;

  if (high_permille > target_permille) {
    s->threshold = (s->threshold > step) ? s->threshold - step : 1;
    STAT_EVENT(proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_THRESH_DOWN : L1_LACS_THRESH_DOWN);
  } else if (high_permille < target_permille) {
    s->threshold += step;
    STAT_EVENT(proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_THRESH_UP : L1_LACS_THRESH_UP);
  }

  if (LACS_CS_MODE == 1) {
    s->cost_sensitive = TRUE;
  } else if (LACS_CS_MODE == 2) {
    s->cost_sensitive = FALSE;
  } else if (s->have_prev_avg) {
    Counter diff =
        (avg_issued > s->prev_period_avg) ? avg_issued - s->prev_period_avg : s->prev_period_avg - avg_issued;
    Counter tolerance_abs = (s->prev_period_avg * LACS_CS_TOLERANCE) / 100;
    s->cost_sensitive = diff <= tolerance_abs;
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

void lacs_fill(Lacs_Slot slot, Cache* cache, Mem_Req* req) {
  if (!lacs_cache_active(cache))
    return;

  Lacs_Train_State* s = lacs_train_state(req->proc_id, slot);
  Addr line_addr = get_cache_line_addr(cache, req->addr);
  uns idx = lacs_hist_index(s, line_addr);

  STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_FILL : L1_LACS_FILL);

  Flag trained =
      (req->lacs_snap[slot] != LACS_SNAP_NONE) && (req->lacs_snap[slot] == LACS_SNAP_DEMAND || LACS_TRAIN_PREFETCH);
  Flag valid;
  uns cost;

  if (trained) {
    Counter num_issued = g_lacs_iic[req->proc_id] - req->lacs_iir[slot];
    Flag high_cost = num_issued < s->threshold;
    cost = high_cost ? LACS_COST_HIGH : LACS_COST_LOW;
    valid = s->cost_sensitive;

    s->hist[idx] = (uns8)cost;
    s->period_fills++;
    s->period_issued_sum += num_issued;
    if (high_cost) {
      s->period_high++;
      STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_HIGH_COST : L1_LACS_HIGH_COST);
    } else {
      STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_LOW_COST : L1_LACS_LOW_COST);
    }
    STAT_EVENT(req->proc_id, req->lacs_snap[slot] == LACS_SNAP_DEMAND
                                 ? (slot == LACS_SLOT_MLC ? MLC_LACS_TRAIN_DEMAND : L1_LACS_TRAIN_DEMAND)
                                 : (slot == LACS_SLOT_MLC ? MLC_LACS_TRAIN_PREFETCH : L1_LACS_TRAIN_PREFETCH));

    if (s->period_fills >= ((Counter)1 << LACS_PERIOD_LOG2))
      lacs_end_period(s, slot, req->proc_id);
  } else if (req->type == MRT_WB || req->type == MRT_WB_NODIRTY) {
    valid = TRUE;
    cost = LACS_WB_INSERT_LOOKUP ? s->hist[idx] : LACS_WB_INSERT_COST;
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_WB_FILL : L1_LACS_WB_FILL);
  } else if (mem_req_type_is_prefetch(req->type) || req->lacs_snap[slot] == LACS_SNAP_PREFETCH) {
    valid = TRUE;
    cost = LACS_PREF_INSERT_LOOKUP ? s->hist[idx] : LACS_PREF_INSERT_COST;
    STAT_EVENT(req->proc_id, slot == LACS_SLOT_MLC ? MLC_LACS_PREF_FILL : L1_LACS_PREF_FILL);
  } else {
    /* Not snapshotted (e.g. --lacs_data_only excluded it) and not a prefetch/writeback:
       let cache_lib.c's lacs_update_insert fall back to its own default cost. */
    valid = FALSE;
    cost = 0;
  }

  cache_set_lacs_next_insert(valid, cost);
}

void lacs_done(uns8 proc_id) {
  for (Lacs_Slot slot = LACS_SLOT_MLC; slot < LACS_NUM_SLOTS; slot++) {
    Lacs_Train_State* s = &g_lacs_train[proc_id][slot];
    if (!s->initialized)
      continue;
    s->final_threshold = s->threshold;

    Cache* cache = mem_lacs_cache(proc_id, slot);
    Counter evictions, evict_aging, evict_ties;
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
