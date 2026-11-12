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
 * File         : memory/mlp_duel.c
 * Description  : See mlp_duel.h.
 ***************************************************************************************/

#include "memory/mlp_duel.h"

#include <math.h>
#include <stdlib.h>

#include "globals/assert.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "statistics.h"
#include "trigger.h"

/**************************************************************************************/
/* Types */

typedef enum Mlp_Duel_Role_enum {
  MLP_DUEL_FOLLOWER = 0,
  MLP_DUEL_INS_PLUS,
  MLP_DUEL_INS_MINUS,
  MLP_DUEL_DATA_PLUS,
  MLP_DUEL_DATA_MINUS,
} Mlp_Duel_Role;

typedef struct Mlp_Duel_State_struct {
  uns num_sets;
  uns8* role;        /* [num_sets]: Mlp_Duel_Role, MLP_DUEL_FOLLOWER unless this set is a leader */
  int* leader_slot;  /* [num_sets]: -1 for followers, else an index into the compact leader
                         arrays below. Slots [0,n) are INS_PLUS, [n,2n) INS_MINUS,
                         [2n,3n) DATA_PLUS, [3n,4n) DATA_MINUS. */
  uns num_groups;    /* n: mlp_duel_num_groups, snapshotted at init */

  Counter* leader_bound_sum;  /* [4n], zeroed each period */
  Counter follower_bound_sum; /* pooled, zeroed each period */

  Counter* leader_access_count;     /* [4n], zeroed each period -- demand accesses, hit or miss.
                                        Denominator for both metrics: leader_bound_sum/this by
                                        default, leader_marked_hit_count/this under
                                        mlp_duel_marked_hit_metric. */
  Counter* leader_marked_hit_count; /* [4n], zeroed each period -- subset of the above that hit a
                                        marked line */
  Counter follower_access_count;    /* pooled, zeroed each period */
  Counter follower_marked_hit_count;

  Flag have_prev_ref;
  double prev_follower_ref;   /* follower_metric as of the last committed lambda change */
  double follower_delta_sum;  /* accounting only: how far things drift since the last change */
  Counter follower_delta_periods;

  /* mlp_duel_confirm_consecutive: the winning candidate from the period before this one, held
     back rather than committed -- only applied once the same role+direction wins again on an
     immediately following period. Any period whose winner differs (or that has no winner at
     all) resets this to "no streak", so confirmation always means *consecutive*. */
  Flag pending_valid;
  Flag pending_is_ins;
  Flag pending_plus;

  float ins_lambda, data_lambda;    /* currently-applied, shared by every set this period */
  uns lambda_hist_size;
  Counter* ins_lambda_periods;      /* histogram: periods spent at each lambda bucket */
  Counter* data_lambda_periods;

  struct Mlp_Duel_State_struct* next; /* registry list, walked by mlp_duel_cycle/finish */
} Mlp_Duel_State;

/**************************************************************************************/
/* Globals */

static Mlp_Duel_State* g_duel_states = NULL;
static Trigger* g_duel_trigger = NULL;

/**************************************************************************************/
/* Static prototypes */

static uns lambda_bucket(Mlp_Duel_State* s, float lambda);
static Flag role_metric(Mlp_Duel_State* s, uns base, double* out_metric);
static void mlp_duel_end_period(Mlp_Duel_State* s);
static void mlp_duel_reset_period(Mlp_Duel_State* s);
static double duel_ratio(double a, double b);
static double metric_ratio(double follower_metric, double role_metric);

/**************************************************************************************/

void mlp_duel_maybe_init(Cache* cache, Repl_Policy repl_policy) {
  if (!MLP_DUEL_ON || repl_policy != REPL_MLP)
    return;

  uns n = MLP_DUEL_NUM_GROUPS;
  ASSERTM(0, n > 0, "mlp_duel_num_groups must be positive\n");
  ASSERTM(0, cache->num_sets >= 4 * n,
          "cache '%s' has %u sets, too few for %u mlp_duel leader groups of 4\n", cache->name, cache->num_sets, n);

  Mlp_Duel_State* s = (Mlp_Duel_State*)calloc(1, sizeof(Mlp_Duel_State));
  s->num_sets = cache->num_sets;
  s->num_groups = n;
  s->role = (uns8*)calloc(cache->num_sets, sizeof(uns8));
  s->leader_slot = (int*)malloc(cache->num_sets * sizeof(int));
  for (uns set = 0; set < cache->num_sets; set++)
    s->leader_slot[set] = -1;

  uns stride = cache->num_sets / n;
  for (uns g = 0; g < n; g++) {
    uns base = g * stride;
    s->role[base + 0] = MLP_DUEL_INS_PLUS;
    s->leader_slot[base + 0] = g;
    s->role[base + 1] = MLP_DUEL_INS_MINUS;
    s->leader_slot[base + 1] = n + g;
    s->role[base + 2] = MLP_DUEL_DATA_PLUS;
    s->leader_slot[base + 2] = 2 * n + g;
    s->role[base + 3] = MLP_DUEL_DATA_MINUS;
    s->leader_slot[base + 3] = 3 * n + g;
  }

  s->leader_bound_sum = (Counter*)calloc(4 * n, sizeof(Counter));
  s->leader_access_count = (Counter*)calloc(4 * n, sizeof(Counter));
  s->leader_marked_hit_count = (Counter*)calloc(4 * n, sizeof(Counter));

  s->have_prev_ref = FALSE;
  s->prev_follower_ref = 0.0;
  s->follower_delta_sum = 0.0;
  s->follower_delta_periods = 0;

  s->ins_lambda = MLP_DUEL_INIT_LAMBDA;
  s->data_lambda = MLP_DUEL_INIT_LAMBDA;
  s->lambda_hist_size = (uns)((MLP_DUEL_LAMBDA_MAX - MLP_DUEL_LAMBDA_MIN) / MLP_DUEL_STEP) + 1;
  s->ins_lambda_periods = (Counter*)calloc(s->lambda_hist_size, sizeof(Counter));
  s->data_lambda_periods = (Counter*)calloc(s->lambda_hist_size, sizeof(Counter));

  s->next = g_duel_states;
  g_duel_states = s;

  cache->predictor = s;

  if (!g_duel_trigger)
    g_duel_trigger = trigger_create("MLP_DUEL_PERIOD", MLP_DUEL_PERIOD, TRIGGER_REPEAT);
}

float mlp_duel_effective_lambda(Cache* cache, uns set, Flag is_data) {
  Mlp_Duel_State* s = (Mlp_Duel_State*)cache->predictor;
  if (!s)
    return is_data ? MLP_LIN_DATA_LAMBDA : MLP_LIN_INSTR_LAMBDA;

  float applied = is_data ? s->data_lambda : s->ins_lambda;
  Mlp_Duel_Role role = (Mlp_Duel_Role)s->role[set];
  float lo = MLP_DUEL_LAMBDA_MIN, hi = MLP_DUEL_LAMBDA_MAX;
  if (is_data && role == MLP_DUEL_DATA_PLUS)
    applied = MAX2(lo, MIN2(hi, applied + MLP_DUEL_STEP));
  else if (is_data && role == MLP_DUEL_DATA_MINUS)
    applied = MAX2(lo, MIN2(hi, applied - MLP_DUEL_STEP));
  else if (!is_data && role == MLP_DUEL_INS_PLUS)
    applied = MAX2(lo, MIN2(hi, applied + MLP_DUEL_STEP));
  else if (!is_data && role == MLP_DUEL_INS_MINUS)
    applied = MAX2(lo, MIN2(hi, applied - MLP_DUEL_STEP));
  return applied;
}

void mlp_duel_record(Cache* cache, uns set, Counter bound_cycles) {
  Mlp_Duel_State* s = (Mlp_Duel_State*)cache->predictor;
  if (!s)
    return;

  int slot = s->leader_slot[set];
  if (slot < 0)
    s->follower_bound_sum += bound_cycles;
  else
    s->leader_bound_sum[slot] += bound_cycles;
}

void mlp_duel_record_access(Cache* cache, uns set, Flag hit_marked) {
  Mlp_Duel_State* s = (Mlp_Duel_State*)cache->predictor;
  if (!s)
    return;

  int slot = s->leader_slot[set];
  if (slot < 0) {
    s->follower_access_count++;
    if (hit_marked)
      s->follower_marked_hit_count++;
  } else {
    s->leader_access_count[slot]++;
    if (hit_marked)
      s->leader_marked_hit_count[slot]++;
  }
}

void mlp_duel_cycle(void) {
  if (!g_duel_trigger || !trigger_fired(g_duel_trigger))
    return;
  for (Mlp_Duel_State* s = g_duel_states; s; s = s->next)
    mlp_duel_end_period(s);
}

void mlp_duel_finish(void) {
  for (Mlp_Duel_State* s = g_duel_states; s; s = s->next) {
    double avg_follower_delta =
        s->follower_delta_periods ? s->follower_delta_sum / (double)s->follower_delta_periods : 0.0;
    fprintf(mystderr,
            "mlp_duel: final ins_lambda=%.1f data_lambda=%.1f avg_follower_delta=%.4f over %llu periods\n",
            s->ins_lambda, s->data_lambda, avg_follower_delta, (unsigned long long)s->follower_delta_periods);
    for (uns b = 0; b < s->lambda_hist_size; b++) {
      float lambda = MLP_DUEL_LAMBDA_MIN + b * MLP_DUEL_STEP;
      if (s->ins_lambda_periods[b])
        fprintf(mystderr, "mlp_duel: ins_lambda=%.1f periods=%llu\n", lambda,
                (unsigned long long)s->ins_lambda_periods[b]);
      if (s->data_lambda_periods[b])
        fprintf(mystderr, "mlp_duel: data_lambda=%.1f periods=%llu\n", lambda,
                (unsigned long long)s->data_lambda_periods[b]);
    }
  }
}

/**************************************************************************************/
/* Static helpers */

static uns lambda_bucket(Mlp_Duel_State* s, float lambda) {
  int b = (int)((lambda - MLP_DUEL_LAMBDA_MIN) / MLP_DUEL_STEP + 0.5);
  if (b < 0)
    b = 0;
  if ((uns)b >= s->lambda_hist_size)
    b = s->lambda_hist_size - 1;
  return (uns)b;
}

/* Mean of the per-set score over the n leader sets occupying compact slots [base, base+n),
   skipping any slot with zero accesses this period (not a fair sample). The score is always
   per-access: mean stall-cycles-per-access (leader_bound_sum/leader_access_count) by default,
   or marked-hit rate (leader_marked_hit_count/leader_access_count) under
   mlp_duel_marked_hit_metric. FALSE (role ineligible this period) if every slot had zero
   accesses. */
static Flag role_metric(Mlp_Duel_State* s, uns base, double* out_metric) {
  Counter* sum_arr = MLP_DUEL_MARKED_HIT_METRIC ? s->leader_marked_hit_count : s->leader_bound_sum;
  Counter* count_arr = s->leader_access_count;
  double sum = 0.0;
  uns count = 0;
  for (uns g = 0; g < s->num_groups; g++) {
    uns slot = base + g;
    if (count_arr[slot] == 0)
      continue;
    sum += (double)sum_arr[slot] / (double)count_arr[slot];
    count++;
  }
  if (!count)
    return FALSE;
  *out_metric = sum / (double)count;
  return TRUE;
}

/* a / b, with b <= 0 (nothing to divide into) handled specially: an actual positive a is
   reported as an unbounded win, and "both zero" (no signal either way) as a no-op ratio of
   1.0, which no mlp_duel_margin > 1 will ever clear. */
static double duel_ratio(double a, double b) {
  if (b <= 0.0)
    return a > 0.0 ? INFINITY : 1.0;
  return a / b;
}

/* How many times better this probe role's score is than the follower population's, in
   whichever direction "better" means for the active metric: lower stall-cycles-per-access by
   default (follower_metric / role_metric), or higher marked-hit rate under
   mlp_duel_marked_hit_metric (role_metric / follower_metric). Either way, clearing
   mlp_duel_margin means the same thing: this role is a clear enough win to adopt. */
static double metric_ratio(double follower_metric, double role_metric) {
  if (MLP_DUEL_MARKED_HIT_METRIC)
    return duel_ratio(role_metric, follower_metric);
  return duel_ratio(follower_metric, role_metric);
}

static void mlp_duel_reset_period(Mlp_Duel_State* s) {
  s->follower_bound_sum = 0;
  s->follower_access_count = 0;
  s->follower_marked_hit_count = 0;
  for (uns slot = 0; slot < 4 * s->num_groups; slot++) {
    s->leader_bound_sum[slot] = 0;
    s->leader_access_count[slot] = 0;
    s->leader_marked_hit_count[slot] = 0;
  }
}

/* One line per period, timestamped by cycle_count, giving the then-applied lambdas -- lets the
   lambda trajectory over the run be reconstructed and plotted against time, unlike
   mlp_duel_finish's end-of-run histogram which only has the lifetime bucket totals. */
static void mlp_duel_trace(Mlp_Duel_State* s) {
  fprintf(mystderr, "mlp_duel: trace cycle=%llu ins_lambda=%.1f data_lambda=%.1f\n",
          (unsigned long long)cycle_count, s->ins_lambda, s->data_lambda);
}

static void mlp_duel_end_period(Mlp_Duel_State* s) {
  Counter follower_sum = MLP_DUEL_MARKED_HIT_METRIC ? s->follower_marked_hit_count : s->follower_bound_sum;
  Counter follower_count = s->follower_access_count;
  if (!follower_count) {
    /* Nothing to compare the leader populations against this period. */
    mlp_duel_trace(s);
    mlp_duel_reset_period(s);
    return;
  }
  double follower_metric = (double)follower_sum / (double)follower_count;

  uns n = s->num_groups;
  double m_ins_plus, m_ins_minus, m_data_plus, m_data_minus;
  Flag have_ins_plus = role_metric(s, 0, &m_ins_plus);
  Flag have_ins_minus = role_metric(s, n, &m_ins_minus);
  Flag have_data_plus = role_metric(s, 2 * n, &m_data_plus);
  Flag have_data_minus = role_metric(s, 3 * n, &m_data_minus);

  Flag ins_candidate = FALSE, ins_plus_wins = FALSE;
  double ins_ratio = 0.0;
  if (have_ins_plus) {
    double r = metric_ratio(follower_metric, m_ins_plus);
    if (r > MLP_DUEL_MARGIN) {
      ins_candidate = TRUE;
      ins_ratio = r;
      ins_plus_wins = TRUE;
    }
  }
  if (have_ins_minus) {
    double r = metric_ratio(follower_metric, m_ins_minus);
    if (r > MLP_DUEL_MARGIN && (!ins_candidate || r > ins_ratio)) {
      ins_candidate = TRUE;
      ins_ratio = r;
      ins_plus_wins = FALSE;
    }
  }

  Flag data_candidate = FALSE, data_plus_wins = FALSE;
  double data_ratio = 0.0;
  if (have_data_plus) {
    double r = metric_ratio(follower_metric, m_data_plus);
    if (r > MLP_DUEL_MARGIN) {
      data_candidate = TRUE;
      data_ratio = r;
      data_plus_wins = TRUE;
    }
  }
  if (have_data_minus) {
    double r = metric_ratio(follower_metric, m_data_minus);
    if (r > MLP_DUEL_MARGIN && (!data_candidate || r > data_ratio)) {
      data_candidate = TRUE;
      data_ratio = r;
      data_plus_wins = FALSE;
    }
  }

  Flag winner_exists = FALSE, winner_is_ins = FALSE, winner_plus = FALSE;
  if (ins_candidate && (!data_candidate || ins_ratio >= data_ratio)) {
    winner_exists = TRUE;
    winner_is_ins = TRUE;
    winner_plus = ins_plus_wins;
  } else if (data_candidate) {
    winner_exists = TRUE;
    winner_is_ins = FALSE;
    winner_plus = data_plus_wins;
  }

  Flag commit = winner_exists;
  if (MLP_DUEL_CONFIRM_CONSECUTIVE) {
    if (!winner_exists) {
      /* No candidate cleared margin this period -- any streak in progress is broken. */
      s->pending_valid = FALSE;
    } else if (s->pending_valid && s->pending_is_ins == winner_is_ins && s->pending_plus == winner_plus) {
      /* Same role+direction won on two consecutive periods -- commit now. */
      commit = TRUE;
      s->pending_valid = FALSE;
    } else {
      /* First period this role+direction has won (or it beat a different previous winner) --
         hold it back and see if it repeats next period before committing. */
      commit = FALSE;
      s->pending_valid = TRUE;
      s->pending_is_ins = winner_is_ins;
      s->pending_plus = winner_plus;
    }
  }

  Flag ins_updated = FALSE, data_updated = FALSE;
  float lo = MLP_DUEL_LAMBDA_MIN, hi = MLP_DUEL_LAMBDA_MAX;
  if (commit && winner_is_ins) {
    s->ins_lambda = MAX2(lo, MIN2(hi, s->ins_lambda + (winner_plus ? MLP_DUEL_STEP : -MLP_DUEL_STEP)));
    ins_updated = TRUE;
  } else if (commit) {
    s->data_lambda = MAX2(lo, MIN2(hi, s->data_lambda + (winner_plus ? MLP_DUEL_STEP : -MLP_DUEL_STEP)));
    data_updated = TRUE;
  }

  STAT_EVENT(0, MLP_DUEL_PERIODS);
  if (ins_updated)
    STAT_EVENT(0, MLP_DUEL_INS_UPDATES);
  if (data_updated)
    STAT_EVENT(0, MLP_DUEL_DATA_UPDATES);
  if (!ins_updated && !data_updated)
    STAT_EVENT(0, MLP_DUEL_NOOP_PERIODS);

  if (!s->have_prev_ref) {
    s->prev_follower_ref = follower_metric;
    s->have_prev_ref = TRUE;
  } else {
    s->follower_delta_sum += s->prev_follower_ref - follower_metric;
    s->follower_delta_periods++;
  }
  if (ins_updated || data_updated)
    s->prev_follower_ref = follower_metric;

  s->ins_lambda_periods[lambda_bucket(s, s->ins_lambda)]++;
  s->data_lambda_periods[lambda_bucket(s, s->data_lambda)]++;
  mlp_duel_trace(s);

  mlp_duel_reset_period(s);
}
