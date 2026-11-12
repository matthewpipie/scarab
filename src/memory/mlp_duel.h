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
 * File         : memory/mlp_duel.h
 * Description  : Online set dueling (DRRIP-style) that tunes REPL_MLP's two fill-boost
 *                 lambdas (mlp_lin_data_lambda, mlp_lin_instr_lambda) instead of taking
 *                 them fixed from the command line. Per cache running REPL_MLP: n groups
 *                 of 4 leader sets each (INS+, INS-, DATA+, DATA-, spread evenly across
 *                 the cache) probe +-mlp_duel_step on one dimension; every other set is a
 *                 follower running the currently-applied pair. Every period
 *                 (mlp_duel_period), the follower population's score is compared against
 *                 each probe role's mean per-set score; if the ratio between them clears
 *                 mlp_duel_margin, the single best-performing candidate across both
 *                 dimensions is rolled out to every set for the next period (immediately, or
 *                 only once the same role+direction wins twice in a row under
 *                 mlp_duel_confirm_consecutive). The score is mean stall-cycles-per-access by
 *                 default (lower is better), or with mlp_duel_window_denom, stall-cycles per
 *                 window-cycle (same denominator mlp_stage_fill_boost itself thresholds on,
 *                 immune to bound_cycles' merged-load inflation -- see mlp_duel_record), or with
 *                 mlp_duel_marked_hit_metric, marked-hit rate per access (higher is better) --
 *                 see mlp_duel.c's metric_ratio and role_metric. Whichever denominator is active,
 *                 both the follower and every probe role use the same one, so the metrics stay
 *                 directly comparable in scale. A no-op (dueling off, or this cache isn't running
 *                 REPL_MLP): every function below falls back to the raw CLI lambda or returns
 *                 immediately.
 ***************************************************************************************/

#ifndef __MLP_DUEL_H__
#define __MLP_DUEL_H__

#include "globals/global_types.h"

#include "libs/cache_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/

/* Called from init_cache_impl right after cache->predictor is cleared. A no-op unless
   --mlp_duel_on and `repl_policy == REPL_MLP`; otherwise partitions `cache`'s sets into
   mlp_duel_num_groups leader groups of 4 (INS+/INS-/DATA+/DATA-) plus followers, and
   allocates the duel state into cache->predictor. */
void mlp_duel_maybe_init(Cache* cache, Repl_Policy repl_policy);

/* The lambda REPL_MLP should currently use for a `is_data` (vs. instruction) mark in
   `set`: the plain currently-applied lambda, offset by +-mlp_duel_step (clamped) if `set`
   is a probe leader for this dimension. Falls back to the raw mlp_lin_data_lambda /
   mlp_lin_instr_lambda CLI params when dueling is off for this cache
   (cache->predictor == NULL), so this is the single lookup REPL_MLP's mechanics need
   regardless of whether dueling is on. */
float mlp_duel_effective_lambda(Cache* cache, uns set, Flag is_data);

/* Record one fill's bound-cycles and window-cycles contribution into whichever population `set`
   belongs to (follower pool, or its specific leader slot) -- dimension-blind: every fill's
   counts toward `set`'s accounting regardless of the fill's own type or the set's probe role.
   `window_cycles` (ld_window_cycles/fe_window_cycles) feeds mlp_duel_window_denom's alternate
   denominator; unused when that's off. A no-op when dueling is off for `cache`. */
void mlp_duel_record(Cache* cache, uns set, Counter bound_cycles, Counter window_cycles);

/* Record one demand cache access (hit or miss) into whichever population `set` belongs to --
   dimension-blind, like mlp_duel_record. `hit_marked` is whether this access was a hit on a
   line whose mlp_mark is non-NONE; it contributes to the population's marked-hit count only
   when true, but every call (hit or miss) counts toward its access count, so marked-hit rate
   is always hits-on-marked-lines over all accesses. Feeds mlp_duel_marked_hit_metric; a no-op
   when dueling is off for `cache`. */
void mlp_duel_record_access(Cache* cache, uns set, Flag hit_marked);

/* Polled once per cycle (cmp_cycle, guarded by MLP_DUEL_ON): advances the shared period
   trigger and, when it fires, ends the period for every registered duel state. */
void mlp_duel_cycle(void);

/* End-of-simulation teardown (cmp_done, guarded by MLP_DUEL_ON): dumps each registered
   duel state's lambda histograms and summary stats. */
void mlp_duel_finish(void);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __MLP_DUEL_H__ */
