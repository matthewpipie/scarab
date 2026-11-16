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
 * File         : memory/shippp.c
 * Description  : REPL_SHIPPP (Young, Chou, Jaleel & Qureshi, "SHiP++: Enhancing
 *                 Signature-Based Hit Predictor for Improved Cache Performance", CRC2 2017)
 *                 on top of SHiP-PC-S (Wu et al., "SHiP: Signature-based Hit Predictor for
 *                 High Performance Caching", MICRO 2011), at the MLC. Select it with
 *                 --mlc_cache_repl_policy (the REPL_SHIPPP value of Repl_Policy).
 *
 * SHiP is 2-bit SRRIP whose insertion RRPV is predicted from the PC that brought the line in:
 *  - Victim search (unchanged SRRIP): a free way, else the lowest way at RRPV 3, aging every
 *    line of the set by one until there is one.
 *  - A Signature History Counter Table (SHCT, 2^--mlc_shippp_shct_bits saturating counters of
 *    --mlc_shippp_counter_bits bits, one table per core) learns whether a signature's lines are
 *    re-referenced. Only the lines of --mlc_shippp_sampled_sets sampled sets keep a signature
 *    and a reuse bit R and train it (SHiP-S, Sec. 7.1): a hit sets R and increments the line's
 *    counter; an eviction with R clear decrements it.
 *  - A fill whose signature's counter is 0 inserts at RRPV 3, any other at 2.
 *
 * SHiP++'s five enhancements (Sec. 3), each one bit of --mlc_shippp_features (all on by
 * default; 0 leaves SHiP-PC-S):
 *  1. A counter at its maximum inserts at RRPV 0.
 *  2. A line trains the SHCT on its first re-reference only (R gates the increment too).
 *  3. A writeback inserts at RRPV 3. (Without 3 it inserts at 2: it has no PC to predict by.)
 *  4. The signature is (PC << 1) + is_prefetch, so demand and prefetch fills by one PC train
 *     separate halves of the SHCT.
 *  5. Each line keeps an is_prefetched bit. A demand's first hit on a prefetched line clears
 *     the bit and sets RRPV 3; a prefetch hit on a prefetched line leaves its RRPV. Any other
 *     hit sets RRPV 0.
 *
 * Choices the papers leave open:
 *  - The PC hash: PC mod the largest prime below the table (16381 for 14 bits; 8191 for the
 *    13 PC bits beside the prefetch bit), as upstream ChampSim's SHiP indexes its SHCT. A
 *    plain low-bits mask would leave line-aligned instruction addresses 7 bits of spread.
 *  - The counters start at --mlc_shippp_shct_init (1: weakly reused). An untrained signature
 *    then inserts at RRPV 2, as SRRIP does; 0 would insert it distant and the maximum at
 *    RRPV 0.
 *  - The sampled sets are spread like DRRIP's leaders (one per constituency), not random.
 *  - Every first non-writeback hit counts as a re-reference, a prefetch's included.
 *  - A writeback-filled line has no signature, so in a sampled set it trains nothing.
 *  - A line that leaves by invalidation, not eviction, trains nothing (ChampSim's policies
 *    never see invalidations either).
 *
 * Where Scarab differs from ChampSim:
 *  - The hooks. memory.c calls shippp_hit when a lookup hit is served (once per request:
 *    cache_access runs again on every retry, so the strategy table's update_hit does nothing)
 *    and shippp_fill right before the fill's cache_insert, whose update_insert predicts.
 *  - The PC. A data demand uses the PC of the load or store that allocated the request (or,
 *    for a prefetch a demand merged into, the oldest op waiting on it); a data prefetch uses
 *    the load PC that trained it (a software prefetch, its own PC). An instruction fetch or
 *    prefetch uses its line number: its address is the line's, and ChampSim's ifetch PC is
 *    an instruction in that line.
 *  - What is a prefetch. A prefetch a demand merged into counts as a demand, as ChampSim's
 *    MSHR merge makes it one. Instruction prefetches (FDIP and the icache/uop cache
 *    prefetchers) count as demands unless --mlc_shippp_inst_pref_is_pref: the paper's rules
 *    target data prefetchers (CRC2 had no instruction prefetcher), and FDIP fills and re-hits
 *    most code lines at the MLC, which rule 5 would then never promote.
 *  - Prefetches that hit at the level they were sent to are dropped by mem_pref_probe without
 *    touching replacement state, so they never reach shippp_hit. --prefetch_update_lru_mlc 0
 *    also keeps prefetch hits away from it.
 *  - The MLC is shared by every core, so each line in a sampled set also keeps the core that
 *    filled it, whose SHCT it trains (the paper's multi-core configuration).
 *  - An MLC exclusive of the dcache or icache is refused (memory.c): every fill would be a
 *    demoted line, i.e. a writeback.
 *
 * --mlp_rrip_boost 3 (memory.param.def) puts REPL_MLP's countdown on top, as on the RRIP policies,
 * with SHiP's aging round (every RRPV of the set up by one) as its clock in place of a fill:
 *  - A line REPL_MLP's marking marks, with a nonzero gamma (mlp_lin_{instr,data}_lambda) for its
 *    side, starts a 2-bit countdown of MLP_COUNTDOWN_TICKS at its fill, whatever RRPV the SHCT
 *    gives it, and restarts it on a hit (not a writeback's) while it is still running.
 *  - Every round(gamma / MLP_COUNTDOWN_TICKS) aging rounds of its set is a tick for the line, which
 *    decrements the countdown. The line is protected while it is nonzero.
 *  - The victim is the lowest way at RRPV 3 among the unprotected lines, aging the set until there
 *    is one; if every line is protected, among the lines with the lowest countdown
 *    (mlp_rrip_countdown_victim in libs/cache_lib.c).
 *  - The RRPV ages underneath; mlp_rrip_boost_restore_rrpv instead gives a line whose countdown
 *    runs out the RRPV of its last fill or hit.
 * The sampled sets are boosted too, so the SHCT learns reuse under the policy that runs. Modes 1
 * and 2 of mlp_rrip_boost are DRRIP's and refused here.
 *
 * Reference implementations, and where this one does not follow them:
 *  - sachinpuranik99/SHiP_Cache_Replacement_Policy (a course project on the CRC1 framework):
 *    3-bit RRPVs, 6-bit counters, an 18-bit signature that also encodes the access type, an
 *    LRU tie-break, SRRIP frequency promotion (RRPV - 1 on a hit) and no set sampling. Not
 *    adopted: none of it is in the paper.
 *  - upstream ChampSim's replacement/ship: plain SHiP with a decoupled LRU sampler (rather than
 *    the cache's own sampled lines), training on every hit, writebacks at RRPV 2. Only its
 *    prime-modulus PC hash is adopted.
 ***************************************************************************************/

#include "memory/shippp.h"

#include <stdlib.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "statistics.h"

/**************************************************************************************/

#define SHIPPP_RRPV_MAX 3 /* 2-bit RRPVs */
#define SHIPPP_RRPV_FAR (SHIPPP_RRPV_MAX - 1)

/* A line of a sampled set: what the paper's sampled sets add to each line */
typedef struct Shippp_Sample_struct {
  uns16 sig;
  uns8 core;    /* the core whose SHCT the line trains (the one that filled it) */
  Flag tracked; /* filled with a signature: FALSE for a writeback or a free way */
  Flag reuse;   /* R: re-referenced since its fill */
} Shippp_Sample;

typedef struct Shippp_State_struct {
  uns assoc;
  uns features; /* --mlc_shippp_features */
  uns8* is_pf;  /* [num_sets * assoc]: the is_prefetched bit (feature 5) */
  int* slot;    /* [num_sets]: the set's index among the sampled sets, or -1 */
  uns num_sampled;
  Shippp_Sample* sample; /* [num_sampled * assoc] */

  uns shct_bits;
  uns shct_max;
  uns pc_mod_full; /* largest prime below 2^shct_bits */
  uns pc_mod_half; /* largest prime below 2^(shct_bits - 1), for feature 4 */
  uns sharers;     /* SHCTs: one per core sharing the cache */
  uns8* shct;      /* [sharers << shct_bits] */
  Flag stats_on;
} Shippp_State;

/* One request, as the policy sees it */
typedef struct Shippp_Access_struct {
  Addr addr;
  Addr pc;
  Flag prefetch; /* a prefetch for SHiP++'s prefetch rules (features 4 and 5) */
  Flag writeback;
  uns8 core;
} Shippp_Access;

/* Staged by shippp_fill for the cache_insert that follows */
static struct {
  Flag valid;
  Cache* cache;
  Shippp_Access acc;
} g_shippp_next_fill;

/**************************************************************************************/
/* Helpers */

static inline Shippp_State* shippp_state(Cache* cache) {
  return (Shippp_State*)cache->predictor;
}

static inline void shippp_stat(Shippp_State* s, uns8 proc_id, Stat_Enum stat) {
  if (s->stats_on)
    STAT_EVENT(proc_id, stat);
}

static uns shippp_prime_below(uns64 n) {
  for (uns64 p = n - 1; p >= 2; p--) {
    Flag prime = TRUE;
    for (uns64 d = 2; d * d <= p && prime; d++)
      prime = p % d != 0;
    if (prime)
      return (uns)p;
  }
  return 1;
}

static inline uns shippp_signature(Shippp_State* s, const Shippp_Access* acc) {
  if (s->features & SHIPPP_F_PF_SIG)
    return (uns)((acc->pc % s->pc_mod_half) << 1) | (acc->prefetch ? 1 : 0);
  return (uns)(acc->pc % s->pc_mod_full);
}

static inline uns8* shippp_ctr(Shippp_State* s, uns8 core, uns sig) {
  return &s->shct[((size_t)core << s->shct_bits) | sig];
}

static inline Shippp_Sample* shippp_sample(Shippp_State* s, uns set, uns way) {
  return s->slot[set] < 0 ? NULL : &s->sample[(size_t)s->slot[set] * s->assoc + way];
}

static inline Flag shippp_req_is_inst(Mem_Req_Type type) {
  return type == MRT_IFETCH || type == MRT_IPRF || type == MRT_UOCPRF || type == MRT_FDIPPRFON ||
         type == MRT_FDIPPRFOFF || type == MRT_FDIPPRFALT;
}

static void shippp_req_access(Cache* cache, Mem_Req* req, Shippp_Access* acc) {
  Flag inst = shippp_req_is_inst(req->type);
  /* A prefetch a demand merged into is the demand's now, as in ChampSim's MSHR merge. */
  Flag pf_type = mem_req_type_is_prefetch(req->type) && !req->demand_match_prefetch;
  acc->addr = req->addr;
  acc->core = req->proc_id;
  acc->writeback = req->type == MRT_WB || req->type == MRT_WB_NODIRTY;
  acc->prefetch = pf_type && (MLC_SHIPPP_INST_PREF_IS_PREF || !inst);
  if (acc->writeback)
    acc->pc = 0; /* a writeback's PC fields are not set (and not read) */
  else if (inst)
    acc->pc = req->addr >> cache->shift_bits; /* the line number */
  else if (pf_type)
    acc->pc = req->pref_loadPC ? req->pref_loadPC : req->loadPC; /* a software prefetch has an op */
  else
    acc->pc = req->loadPC ? req->loadPC : req->oldest_op_addr ? req->oldest_op_addr : req->pref_loadPC;
}

/* The way holding addr, or -1 */
static int shippp_find_way(Cache* cache, Addr addr, uns* set) {
  Addr tag, line_addr;
  *set = ext_cache_index(cache, addr, &tag, &line_addr);
  for (uns w = 0; w < cache->assoc; w++) {
    Cache_Entry* entry = &cache->entries[*set][w];
    if (entry->valid && entry->tag == tag)
      return w;
  }
  return -1;
}

/**************************************************************************************/
/* Strategy table */

void shippp_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                        Repl_Policy repl_policy) {
  ASSERTM(0, MLC_SHIPPP_SHCT_BITS >= 2 && MLC_SHIPPP_SHCT_BITS <= 16,
          "--mlc_shippp_shct_bits must be in [2, 16] (got %u)\n", MLC_SHIPPP_SHCT_BITS);
  ASSERTM(0, MLC_SHIPPP_COUNTER_BITS >= 1 && MLC_SHIPPP_COUNTER_BITS <= 8,
          "--mlc_shippp_counter_bits must be in [1, 8] (got %u)\n", MLC_SHIPPP_COUNTER_BITS);
  ASSERTM(0, MLC_SHIPPP_SHCT_INIT <= N_BIT_MASK(MLC_SHIPPP_COUNTER_BITS),
          "--mlc_shippp_shct_init %u does not fit in %u-bit counters\n", MLC_SHIPPP_SHCT_INIT, MLC_SHIPPP_COUNTER_BITS);
  ASSERTM(0, (MLC_SHIPPP_FEATURES & ~(uns)SHIPPP_F_ALL) == 0,
          "--mlc_shippp_features 0x%x has bits beyond the five enhancements (0x%x)\n", MLC_SHIPPP_FEATURES,
          SHIPPP_F_ALL);
  ASSERTM(0, MLP_RRIP_BOOST == MLP_RRIP_BOOST_OFF || MLP_RRIP_BOOST == MLP_RRIP_BOOST_COUNTDOWN,
          "REPL_SHIPPP takes only --mlp_rrip_boost %u (the countdown), not %u\n", MLP_RRIP_BOOST_COUNTDOWN,
          MLP_RRIP_BOOST);

  Shippp_State* s = (Shippp_State*)calloc(1, sizeof(Shippp_State));
  s->assoc = assoc;
  s->features = MLC_SHIPPP_FEATURES;
  s->is_pf = (uns8*)calloc((size_t)cache->num_sets * assoc, sizeof(uns8));

  /* Sampled sets, spread like DRRIP's leaders: the S sets an index reaches are cut into
     L = min(--mlc_shippp_sampled_sets, S) constituencies of C = S / L sets, and constituency k
     samples the set at offset k mod C. 0 samples every set. */
  uns sets = MIN2(cache->num_sets, (uns)(cache->set_mask + 1));
  uns samples = MLC_SHIPPP_SAMPLED_SETS ? MIN2(MLC_SHIPPP_SAMPLED_SETS, sets) : sets;
  uns constituency = sets / samples;
  s->slot = (int*)malloc(sizeof(int) * cache->num_sets);
  for (uns set = 0; set < cache->num_sets; set++)
    s->slot[set] = -1;
  for (uns k = 0; k < samples; k++)
    s->slot[k * constituency + k % constituency] = (int)k;
  s->num_sampled = samples;
  s->sample = (Shippp_Sample*)calloc((size_t)samples * assoc, sizeof(Shippp_Sample));

  s->shct_bits = MLC_SHIPPP_SHCT_BITS;
  s->shct_max = N_BIT_MASK(MLC_SHIPPP_COUNTER_BITS);
  s->pc_mod_full = shippp_prime_below(1ULL << s->shct_bits);
  s->pc_mod_half = shippp_prime_below(1ULL << (s->shct_bits - 1));

  /* Until shippp_configure says otherwise: one core, no stats */
  s->sharers = 1;
  s->shct = (uns8*)malloc((size_t)1 << s->shct_bits);
  for (size_t i = 0; i < ((size_t)1 << s->shct_bits); i++)
    s->shct[i] = (uns8)MLC_SHIPPP_SHCT_INIT;
  s->stats_on = FALSE;

  cache->predictor = s;
}

void shippp_update_hit(Cache* cache, uns set, uns way, void* arg) {
  /* shippp_hit does a hit's work, once per request */
}

void shippp_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  Shippp_State* s = shippp_state(cache);
  ASSERTM(proc_id, g_shippp_next_fill.valid && g_shippp_next_fill.cache == cache,
          "%s: REPL_SHIPPP insert without shippp_fill (only the MLC's fills stage one)\n", cache->name);
  const Shippp_Access* acc = &g_shippp_next_fill.acc;
  ASSERT(proc_id, cache->entries[set][way].base == get_cache_line_addr(cache, acc->addr));
  g_shippp_next_fill.valid = FALSE;

  uns8* rrpv = &cache->entries[set][way].reference_val;
  uns idx = set * s->assoc + way;
  uns sig = shippp_signature(s, acc);
  uns8 core = acc->core < s->sharers ? acc->core : 0;

  s->is_pf[idx] = (s->features & SHIPPP_F_PF_PROMO) && acc->prefetch;

  Shippp_Sample* sample = shippp_sample(s, set, way);
  if (sample) {
    sample->tracked = !acc->writeback;
    sample->reuse = FALSE;
    sample->sig = (uns16)sig;
    sample->core = core;
  }

  if (acc->writeback) {
    *rrpv = (s->features & SHIPPP_F_WB_DISTANT) ? SHIPPP_RRPV_MAX : SHIPPP_RRPV_FAR;
    shippp_stat(s, proc_id, MLC_SHIPPP_INSERT_WB);
    return;
  }

  uns ctr = *shippp_ctr(s, core, sig);
  if (ctr == 0) {
    *rrpv = SHIPPP_RRPV_MAX;
    shippp_stat(s, proc_id, MLC_SHIPPP_INSERT_DISTANT);
  } else if ((s->features & SHIPPP_F_INSERT_MRU) && ctr == s->shct_max) {
    *rrpv = 0;
    shippp_stat(s, proc_id, MLC_SHIPPP_INSERT_IMMEDIATE);
  } else {
    *rrpv = SHIPPP_RRPV_FAR;
    shippp_stat(s, proc_id, MLC_SHIPPP_INSERT_FAR);
  }
  if (acc->prefetch)
    shippp_stat(s, proc_id, MLC_SHIPPP_INSERT_PREF);
  /* mlp_rrip_boost 3: a boosted fill the SHCT predicts dead, which the countdown keeps anyway
     (cache_insert_strategy starts it after this) */
  if (*rrpv == SHIPPP_RRPV_MAX && cache->entries[set][way].mlp_boost > 0.0 && mlp_rrip_countdown_on(cache))
    shippp_stat(s, proc_id, MLC_SHIPPP_BOOST_DISTANT);
}

/* SRRIP's victim search. A free way is taken before any valid line and ages nothing. Called
   with if_external for get_next_repl_line's peek, which may age the set; the insert that
   follows then finds the same victim without aging it further. Only that insert trains. With
   mlp_rrip_boost 3 the search skips protected lines (mlp_rrip_countdown_victim). */
Cache_Entry* shippp_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  for (uns w = 0; w < cache->assoc; w++) {
    if (!cache->entries[set][w].valid) {
      *way = w;
      return &cache->entries[set][w];
    }
  }

  if (mlp_rrip_countdown_on(cache)) {
    *way = mlp_rrip_countdown_victim(cache, proc_id, set, SHIPPP_RRPV_MAX, if_external);
    goto found;
  }
  for (;;) {
    for (uns w = 0; w < cache->assoc; w++) {
      if (cache->entries[set][w].reference_val >= SHIPPP_RRPV_MAX) {
        *way = w;
        goto found;
      }
    }
    for (uns w = 0; w < cache->assoc; w++)
      cache->entries[set][w].reference_val++;
  }

found:
  if (!if_external) {
    Shippp_State* s = shippp_state(cache);
    Shippp_Sample* sample = shippp_sample(s, set, *way);
    if (sample && sample->tracked && !sample->reuse) {
      uns8* ctr = shippp_ctr(s, sample->core, sample->sig);
      if (*ctr > 0)
        (*ctr)--;
      shippp_stat(s, proc_id, MLC_SHIPPP_TRAIN_DEC);
    }
    if (sample)
      sample->tracked = FALSE;
    shippp_stat(s, proc_id, s->is_pf[set * s->assoc + *way] ? MLC_SHIPPP_EVICT_PF : MLC_SHIPPP_EVICT);
  }
  return &cache->entries[set][*way];
}

/**************************************************************************************/
/* Memory system hooks */

void shippp_configure(Cache* cache, uns sharers) {
  if (cache->repl_policy != REPL_SHIPPP)
    return;
  Shippp_State* s = shippp_state(cache);
  ASSERT(0, sharers >= 1 && sharers <= 256);
  if (sharers != s->sharers) {
    size_t entries = (size_t)sharers << s->shct_bits;
    free(s->shct);
    s->shct = (uns8*)malloc(entries);
    for (size_t i = 0; i < entries; i++)
      s->shct[i] = (uns8)MLC_SHIPPP_SHCT_INIT;
    s->sharers = sharers;
  }
  s->stats_on = TRUE;
}

void shippp_hit(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_SHIPPP)
    return;
  Shippp_State* s = shippp_state(cache);
  Shippp_Access acc;
  shippp_req_access(cache, req, &acc);
  if (acc.writeback) {
    shippp_stat(s, req->proc_id, MLC_SHIPPP_HIT_WB);
    return; /* no indication of reuse (Sec. 3, enhancement 3) */
  }
  uns set;
  int way = shippp_find_way(cache, req->addr, &set);
  if (way < 0)
    return;

  uns8* rrpv = &cache->entries[set][way].reference_val;
  uns idx = set * s->assoc + (uns)way;
  if (s->is_pf[idx]) { /* only set with feature 5 */
    if (!acc.prefetch) {
      *rrpv = SHIPPP_RRPV_MAX;
      s->is_pf[idx] = FALSE;
      shippp_stat(s, req->proc_id, MLC_SHIPPP_HIT_PF_DEMOTE);
    } else {
      shippp_stat(s, req->proc_id, MLC_SHIPPP_HIT_PF_KEEP);
    }
  } else {
    *rrpv = 0;
    shippp_stat(s, req->proc_id, MLC_SHIPPP_HIT_PROMOTE);
  }
  /* mlp_rrip_boost 3: restart a running countdown */
  mlp_rrip_countdown_touch(cache, &cache->entries[set][way], FALSE);

  Shippp_Sample* sample = shippp_sample(s, set, (uns)way);
  if (sample && sample->tracked) {
    if (!(s->features & SHIPPP_F_FIRST_HIT) || !sample->reuse) {
      uns8* ctr = shippp_ctr(s, sample->core, sample->sig);
      if (*ctr < s->shct_max)
        (*ctr)++;
      shippp_stat(s, req->proc_id, MLC_SHIPPP_TRAIN_INC);
    }
    sample->reuse = TRUE;
  }
}

void shippp_fill(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_SHIPPP)
    return;
  g_shippp_next_fill.valid = TRUE;
  g_shippp_next_fill.cache = cache;
  shippp_req_access(cache, req, &g_shippp_next_fill.acc);
}
