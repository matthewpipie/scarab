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
 *                 (mlp_duel_period), the follower population's pooled stall-cycles-per-
 *                 request is compared against each probe role's mean per-set ratio; if a
 *                 probe clears mlp_duel_margin, the single best-performing candidate
 *                 across both dimensions is rolled out to every set for the next period.
 *                 A no-op (dueling off, or this cache isn't running REPL_MLP): every
 *                 function below falls back to the raw CLI lambda or returns immediately.
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

/* Record one fill's bound-cycles contribution into whichever population `set` belongs to
   (follower pool, or its specific leader slot) -- dimension-blind: every fill's bound
   cycles count toward `set`'s accounting regardless of the fill's own type or the set's
   probe role. A no-op when dueling is off for `cache`. */
void mlp_duel_record(Cache* cache, uns set, Counter bound_cycles);

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
