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
 * File         : memory/mockingjay.c
 * Description  : REPL_MOCKINGJAY (Shah, Jain & Lin, "Effective Mimicry of Belady's MIN
 *                 Policy", HPCA 2022), ported from the authors' ChampSim CRC2 artifact
 *                 (mockingjay.llc_repl). Select it with --mlc_cache_repl_policy or
 *                 --l1_cache_repl_policy (the REPL_MOCKINGJAY value of Repl_Policy).
 *
 * Mockingjay evicts the line predicted to be reused furthest in the future:
 *  - A sampled cache keeps, for a few sampled sets of the real cache, the last access
 *    timestamp and PC signature of recently seen lines, over a history 8x the set's
 *    associativity. When a line comes back, the time since its last access (in accesses to
 *    its set) trains that signature's entry in the reuse distance predictor (RDP), by
 *    temporal-difference learning. A line that ages out unseen trains its signature as a
 *    scan (INF_RD).
 *  - Each line of the real cache has an estimated time remaining (ETR) to its next use: the
 *    RDP's prediction at its fill or last hit, in units of GRANULARITY set accesses, counted
 *    down every GRANULARITY accesses to its set and on past 0. The victim is the line with
 *    the largest |ETR| -- reused furthest in the future, or overdue the longest -- and ties
 *    go to an overdue line.
 *  - A fill predicted to be reused after every resident line is bypassed.
 *  - With a prefetcher (Flex-MIN), a reuse interval that ends in a prefetch trains as
 *    FLEXMIN_PENALTY times longer, so lines a prefetch will bring back are kept less.
 *
 * The predictor learns a signature's first reuse distance only when one of its sampled lines
 * is reused or ages out, and aging out takes INF_RD (8x associativity) accesses to that set:
 * about 8 cache-capacities of traffic. Until then the signature's fills insert at ETR 0
 * (MJ_FILL_COLD), so a short warmup leaves the policy partly untrained -- the paper warms for
 * 200M instructions.
 *
 * Every constant, and every quirk of the artifact's arithmetic, is kept. The ones that read
 * the LLC's geometry (sampled sets, signature and tag widths, INF_RD, INF_ETR) take the
 * geometry of whichever cache runs the policy. Where the artifact and the paper differ, this
 * follows the artifact:
 *  - The RDP moves by 1 only when the sample is at least 16 away (the artifact's integer
 *    min(1, diff * 1/16)); the paper describes a step of min(1, diff/16).
 *  - A signature's first sample is stored unclamped, so a prefetch-ended one can start
 *    above INF_RD (up to INF_RD * FLEXMIN_PENALTY); the paper saturates it at INF_RD.
 *  - MAX_RD is INF_RD - 22 (105 for 16 ways); the paper quotes 104.
 *  - With more than one core sharing the cache, the signature hashes the core id instead of
 *    the hit bit, and a signature with no prediction yet inserts at INF_ETR instead of 0.
 *
 * Where Scarab differs from ChampSim:
 *  - The hooks. ChampSim calls one update on every hit and every fill. Here memory.c calls
 *    mockingjay_hit when a lookup hit is served (once per request: cache_access runs again
 *    on every retry, so the strategy table's update_hit does nothing), mockingjay_bypass
 *    right before a fill would pick its victim, and mockingjay_fill right before the fill's
 *    cache_insert, whose update_insert trains and predicts.
 *  - The PC. A data demand uses the PC of the load or store that allocated the request (or,
 *    for a prefetch a demand merged into, the oldest op waiting on it); a data prefetch uses
 *    the load PC that triggered it, as the paper says (a software prefetch, its own PC); an
 *    instruction fetch or prefetch uses its line number (its address is line-aligned, and the
 *    signature hashes the PC's low bits). A prefetch a demand merged into counts as a demand,
 *    as ChampSim's MSHR merge makes it one.
 *  - Prefetches that hit at the level they were sent to are dropped by mem_pref_probe
 *    without touching replacement state, so they train nothing; ChampSim counts them as
 *    prefetch hits. --prefetch_update_lru_mlc/_l1 0 also stops prefetch hits from training.
 *  - A fill whose line this level must keep (a dirty line the MLC is the only holder of) is
 *    never bypassed.
 *  - At the MLC over an exclusive LLC (--exclusive_l1), which only MLC victims fill, a
 *    bypassed line goes down to the LLC as a clean MLC victim would (memory.c, via
 *    mockingjay_bypass_check/_commit); otherwise a clean bypassed line would leave the chip
 *    once the dcache or icache evicted it. A non-exclusive LLC already got the line on its
 *    miss, so there nothing moves.
 *  - The sampled cache's partial tag also hashes the core id when the cache is shared:
 *    Scarab tells cores' lines apart only by the top address bits, which the artifact's
 *    tag drops.
 ***************************************************************************************/

#include "memory/mockingjay.h"

#include <math.h>
#include <stdlib.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/utils.h"

#include "memory/memory.param.h"

#include "memory/memory.h"

/**************************************************************************************/
/* The artifact's constants */

#define MJ_HISTORY 8                 /* the sampled cache sees 8x the associativity */
#define MJ_GRANULARITY 8             /* set accesses per ETR unit */
#define MJ_MAX_RD_MARGIN 22          /* MAX_RD = INF_RD - 22 */
#define MJ_SAMPLED_CACHE_WAYS 5      /* SAMPLED_CACHE_WAYS */
#define MJ_LOG2_SAMPLED_CACHE_SETS 4 /* LOG2_SAMPLED_CACHE_SETS: sub-sets per sampled set */
#define MJ_TEMP_DIFFERENCE_DIV 16    /* TEMP_DIFFERENCE = 1/16 */
#define MJ_MIN_TIMESTAMP_BITS 8      /* TIMESTAMP_BITS */
#define MJ_CRC_POLYNOMIAL 3988292384ULL

/* Stat offsets from a level's first MJ stat (MLC_MJ_HIT or L1_MJ_HIT), in memory.stat.def's
   order. */
typedef enum Mj_Stat_enum {
  MJ_STAT_HIT,
  MJ_STAT_FILL,
  MJ_STAT_FILL_COLD,
  MJ_STAT_FILL_SCAN,
  MJ_STAT_WB_FILL,
  MJ_STAT_BYPASS,
  MJ_STAT_EVICT_FUTURE,
  MJ_STAT_EVICT_ZERO,
  MJ_STAT_EVICT_PAST,
  MJ_STAT_TRAIN,
  MJ_STAT_DETRAIN,
  MJ_NUM_STATS
} Mj_Stat;

_Static_assert(MLC_MJ_DETRAIN - MLC_MJ_HIT == MJ_STAT_DETRAIN, "MLC_MJ_* stats out of Mj_Stat order");
_Static_assert(L1_MJ_DETRAIN - L1_MJ_HIT == MJ_STAT_DETRAIN, "L1_MJ_* stats out of Mj_Stat order");

/* What an update predicted for the line it touched. */
typedef enum Mj_Pred_enum {
  MJ_PRED_NONE, /* nothing: a writeback, or a bypass (no line) */
  MJ_PRED_COLD, /* the signature had no prediction yet */
  MJ_PRED_SCAN, /* predicted past MAX_RD: ETR = INF_ETR, never aged */
  MJ_PRED_RD,   /* ETR = predicted reuse distance / GRANULARITY */
} Mj_Pred;

typedef struct Mj_Sample_Line_struct {
  Flag valid;
  uns64 tag;
  uns64 signature;
  uns timestamp;
} Mj_Sample_Line;

typedef struct Mj_State_struct {
  uns num_sets;
  uns assoc;
  uns set_bits;
  uns line_bits;

  /* Derived from the geometry as the artifact derives them from the LLC's */
  uns log2_size;    /* LOG2_LLC_SIZE: floor(log2(bytes)) */
  uns sampled_log2; /* LOG2_SAMPLED_SETS */
  uns sig_bits;     /* PC_SIGNATURE_BITS */
  uns sc_tag_bits;  /* SAMPLED_CACHE_TAG_BITS */
  uns ts_bits;      /* TIMESTAMP_BITS */
  int inf_rd;       /* INF_RD */
  int inf_etr;      /* INF_ETR */
  int max_rd;       /* MAX_RD */

  int* etr;           /* [num_sets * assoc] */
  uns* etr_clock;     /* [num_sets] */
  uns* timestamp;     /* [num_sets]; read only in sampled sets */
  int* sample_slot;   /* [num_sets]: the set's index among the sampled sets, or -1 */
  uns num_sampled;    /* number of sampled sets */
  Mj_Sample_Line* sc; /* [num_sampled << MJ_LOG2_SAMPLED_CACHE_SETS][MJ_SAMPLED_CACHE_WAYS] */
  int* rdp;           /* [1 << sig_bits] */
  Flag* rdp_valid;    /* [1 << sig_bits]: the artifact's rdp.count(signature) */

  /* mockingjay_configure */
  uns sharers;       /* NUM_CPUS for this cache */
  uns core_bits;     /* bits of the core id in a shared cache's signature */
  double pf_penalty; /* FLEXMIN_PENALTY */
  Flag stats_on;
  Stat_Enum stat_base;
} Mj_State;

/* One access, as the artifact's update sees it */
typedef struct Mj_Access_struct {
  Addr addr;
  Addr pc;
  Flag prefetch;
  Flag writeback;
  uns8 core;
} Mj_Access;

/* Staged by mockingjay_fill for the cache_insert that follows */
static struct {
  Flag valid;
  Cache* cache;
  Mj_Access acc;
} g_mj_next_fill;

/**************************************************************************************/
/* Helpers */

static inline Mj_State* mj_state(Cache* cache) {
  return (Mj_State*)cache->predictor;
}

static inline void mj_stat(Mj_State* s, uns8 proc_id, Mj_Stat stat) {
  if (s->stats_on)
    STAT_EVENT(proc_id, s->stat_base + stat);
}

static inline uns mj_floor_log2_64(uns64 x) {
  return 63 - __builtin_clzll(x);
}

/* CRC_HASH */
static inline uns64 mj_crc_hash(uns64 block_address) {
  uns64 ret = block_address;
  for (uns ii = 0; ii < 3; ii++)
    ret = (ret & 1) == 1 ? (ret >> 1) ^ MJ_CRC_POLYNOMIAL : ret >> 1;
  return ret;
}

/* get_pc_signature */
static uns64 mj_signature(Mj_State* s, Addr pc, Flag hit, Flag prefetch, uns8 core) {
  uns64 sig = pc;
  if (s->sharers == 1) {
    sig = (sig << 1) | (hit ? 1 : 0);
    sig = (sig << 1) | (prefetch ? 1 : 0);
  } else {
    sig = (sig << 1) | (prefetch ? 1 : 0);
    sig = (sig << s->core_bits) | core;
  }
  return mj_crc_hash(sig) & N_BIT_MASK(s->sig_bits);
}

/* time_elapsed */
static inline int mj_elapsed(Mj_State* s, uns global, uns local) {
  if (global >= local)
    return global - local;
  return global + (1u << s->ts_bits) - local;
}

/* temporal_difference: moves by 1, and only when the sample is TEMP_DIFFERENCE_DIV away */
static inline int mj_temporal_difference(Mj_State* s, int init, int sample) {
  if (sample > init) {
    int diff = MIN2(1, (sample - init) / MJ_TEMP_DIFFERENCE_DIV);
    return MIN2(init + diff, s->inf_rd);
  } else if (sample < init) {
    int diff = MIN2(1, (init - sample) / MJ_TEMP_DIFFERENCE_DIV);
    return MAX2(init - diff, 0);
  }
  return init;
}

static inline Mj_Sample_Line* mj_sc_set(Mj_State* s, uns sc_index) {
  return &s->sc[sc_index * MJ_SAMPLED_CACHE_WAYS];
}

/* detrain: a sampled line that leaves unseen trains its signature as a scan */
static void mj_detrain(Mj_State* s, Mj_Sample_Line* line, uns8 proc_id) {
  if (!line->valid)
    return;
  if (s->rdp_valid[line->signature])
    s->rdp[line->signature] = MIN2(s->rdp[line->signature] + 1, s->inf_rd);
  else {
    s->rdp[line->signature] = s->inf_rd;
    s->rdp_valid[line->signature] = TRUE;
  }
  line->valid = FALSE;
  mj_stat(s, proc_id, MJ_STAT_DETRAIN);
}

/* llc_update_replacement_state: one access to `set`, a hit or fill of `way`, or a bypass
   (way < 0). */
static Mj_Pred mj_update(Mj_State* s, uns set, int way, const Mj_Access* acc, Flag hit, uns8 proc_id) {
  if (acc->writeback) {
    if (!hit && way >= 0)
      s->etr[set * s->assoc + way] = -s->inf_etr;
    return MJ_PRED_NONE;
  }

  uns64 sig = mj_signature(s, acc->pc, hit, acc->prefetch, acc->core);

  if (s->sample_slot[set] >= 0) {
    uns64 block = acc->addr >> s->line_bits;
    uns sc_index = ((uns)s->sample_slot[set] << MJ_LOG2_SAMPLED_CACHE_SETS) |
                   (uns)((block >> s->set_bits) & N_BIT_MASK(MJ_LOG2_SAMPLED_CACHE_SETS));
    uns64 sc_tag = (block >> (s->set_bits + MJ_LOG2_SAMPLED_CACHE_SETS)) & N_BIT_MASK(s->sc_tag_bits);
    if (s->sharers > 1)
      sc_tag ^= acc->core;
    Mj_Sample_Line* sc_set = mj_sc_set(s, sc_index);

    for (uns w = 0; w < MJ_SAMPLED_CACHE_WAYS; w++) {
      Mj_Sample_Line* line = &sc_set[w];
      if (!line->valid || line->tag != sc_tag)
        continue;
      int sample = mj_elapsed(s, s->timestamp[set], line->timestamp);
      if (sample <= s->inf_rd) {
        if (acc->prefetch)
          sample = (int)(sample * s->pf_penalty);
        if (s->rdp_valid[line->signature])
          s->rdp[line->signature] = mj_temporal_difference(s, s->rdp[line->signature], sample);
        else {
          s->rdp[line->signature] = sample;
          s->rdp_valid[line->signature] = TRUE;
        }
        line->valid = FALSE;
        mj_stat(s, proc_id, MJ_STAT_TRAIN);
      }
      break;
    }

    /* The sampled cache's LRU: a free way, else a line past INF_RD (detrained on sight),
       else the oldest. */
    int lru_way = -1;
    int lru_rd = -1;
    for (uns w = 0; w < MJ_SAMPLED_CACHE_WAYS; w++) {
      Mj_Sample_Line* line = &sc_set[w];
      if (!line->valid) {
        lru_way = w;
        lru_rd = s->inf_rd + 1;
        continue;
      }
      int sample = mj_elapsed(s, s->timestamp[set], line->timestamp);
      if (sample > s->inf_rd) {
        lru_way = w;
        lru_rd = s->inf_rd + 1;
        mj_detrain(s, line, proc_id);
      } else if (sample > lru_rd) {
        lru_way = w;
        lru_rd = sample;
      }
    }
    mj_detrain(s, &sc_set[lru_way], proc_id);

    for (uns w = 0; w < MJ_SAMPLED_CACHE_WAYS; w++) {
      Mj_Sample_Line* line = &sc_set[w];
      if (!line->valid) {
        line->valid = TRUE;
        line->signature = sig;
        line->tag = sc_tag;
        line->timestamp = s->timestamp[set];
        break;
      }
    }

    s->timestamp[set] = (s->timestamp[set] + 1) & N_BIT_MASK(s->ts_bits);
  }

  /* Age the set every GRANULARITY accesses; the line just touched and scans are not aged. */
  if (s->etr_clock[set] == MJ_GRANULARITY) {
    for (uns w = 0; w < s->assoc; w++) {
      int* etr = &s->etr[set * s->assoc + w];
      if ((int)w != way && abs(*etr) < s->inf_etr)
        (*etr)--;
    }
    s->etr_clock[set] = 0;
  }
  s->etr_clock[set]++;

  if (way < 0)
    return MJ_PRED_NONE;

  int* etr = &s->etr[set * s->assoc + way];
  if (!s->rdp_valid[sig]) {
    *etr = s->sharers == 1 ? 0 : s->inf_etr;
    return MJ_PRED_COLD;
  }
  if (s->rdp[sig] > s->max_rd) {
    *etr = s->inf_etr;
    return MJ_PRED_SCAN;
  }
  *etr = s->rdp[sig] / MJ_GRANULARITY;
  return MJ_PRED_RD;
}

/* llc_find_victim's choice among a full set: the largest |ETR|, ties to an overdue line. That
   |ETR| goes to max_etr. */
static uns mj_victim(Mj_State* s, uns set, int* max_etr) {
  uns victim = 0;
  *max_etr = 0;
  for (uns w = 0; w < s->assoc; w++) {
    int etr = s->etr[set * s->assoc + w];
    if (abs(etr) > *max_etr || (abs(etr) == *max_etr && etr < 0)) {
      *max_etr = abs(etr);
      victim = w;
    }
  }
  return victim;
}

static inline Flag mj_req_is_inst(Mem_Req_Type type) {
  return type == MRT_IFETCH || type == MRT_IPRF || type == MRT_UOCPRF || type == MRT_FDIPPRFON ||
         type == MRT_FDIPPRFOFF || type == MRT_FDIPPRFALT;
}

/* The access a request makes, as the artifact's (type, ip) would describe it */
static void mj_req_access(Cache* cache, Mem_Req* req, Mj_Access* acc) {
  acc->addr = req->addr;
  acc->writeback = req->type == MRT_WB || req->type == MRT_WB_NODIRTY;
  /* A prefetch a demand merged into is the demand's now, as in ChampSim's MSHR merge. */
  acc->prefetch = mem_req_type_is_prefetch(req->type) && !req->demand_match_prefetch;
  acc->core = req->proc_id;
  if (acc->writeback)
    acc->pc = 0; /* a writeback's PC fields are not set (and not read) */
  else if (mj_req_is_inst(req->type))
    acc->pc = req->addr >> cache->shift_bits; /* line-aligned: the line number keeps the hashed bits */
  else if (acc->prefetch)
    acc->pc = req->pref_loadPC ? req->pref_loadPC : req->loadPC; /* a software prefetch has an op */
  else
    acc->pc = req->loadPC ? req->loadPC : req->oldest_op_addr ? req->oldest_op_addr : req->pref_loadPC;
}

/* The way holding addr, or -1 */
static int mj_find_way(Cache* cache, Addr addr, uns* set) {
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

void mockingjay_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                            Repl_Policy repl_policy) {
  Mj_State* s = (Mj_State*)calloc(1, sizeof(Mj_State));
  s->num_sets = cache->num_sets;
  s->assoc = assoc;
  s->set_bits = cache->set_bits;
  s->line_bits = cache->shift_bits;
  s->log2_size = mj_floor_log2_64((uns64)cache->num_sets * assoc * line_size);

  s->inf_rd = (int)(assoc * MJ_HISTORY) - 1;
  s->inf_etr = (int)(assoc * MJ_HISTORY / MJ_GRANULARITY) - 1;
  s->max_rd = MAX2(s->inf_rd - MJ_MAX_RD_MARGIN, 0);
  ASSERTM(0, s->inf_etr >= 1, "%s: REPL_MOCKINGJAY needs at least 2 ways (has %u)\n", name, assoc);
  /* The artifact's 8 bits cover a 16-way INF_RD of 127 with room for the wraparound check;
     wider sets need more. */
  s->ts_bits = MAX2((uns)MJ_MIN_TIMESTAMP_BITS, mj_floor_log2_64((uns64)s->inf_rd) + 2);

  s->sig_bits = s->log2_size > 10 ? MIN2(s->log2_size - 10, 24u) : 1;
  s->sc_tag_bits = s->log2_size < 31 ? MIN2(31 - s->log2_size, 32u) : 1;

  /* LOG2_SAMPLED_SETS = LOG2_LLC_SIZE - 16 (32 sets for 2 MB), at least 2 sets and at most
     every set; --mockingjay_sampled_sets overrides it. */
  if (MOCKINGJAY_SAMPLED_SETS) {
    ASSERTM(0, !(MOCKINGJAY_SAMPLED_SETS & (MOCKINGJAY_SAMPLED_SETS - 1)) && MOCKINGJAY_SAMPLED_SETS <= s->num_sets,
            "--mockingjay_sampled_sets %u must be a power of two no larger than %s's %u sets\n",
            MOCKINGJAY_SAMPLED_SETS, name, s->num_sets);
    s->sampled_log2 = LOG2(MOCKINGJAY_SAMPLED_SETS);
  } else {
    int log2_sampled = (int)s->log2_size - 16;
    s->sampled_log2 = (uns)MIN2(MAX2(log2_sampled, 1), (int)s->set_bits);
  }
  /* is_sampled_set: the low bits of the set index repeat its high bits, which keeps
     2^sampled_log2 sets spread over the cache. (With sampled_log2 0 the test passes for every
     set, so that needs a single-set cache.) */
  uns mask_length = s->set_bits - s->sampled_log2;
  Addr mask = N_BIT_MASK(mask_length);
  s->sample_slot = (int*)malloc(sizeof(int) * s->num_sets);
  s->num_sampled = 0;
  for (uns set = 0; set < s->num_sets; set++) {
    Flag sampled = (set & mask) == ((set >> s->sampled_log2) & mask);
    s->sample_slot[set] = sampled ? (int)s->num_sampled++ : -1;
  }
  ASSERT(0, s->num_sampled == (1u << s->sampled_log2));

  s->etr = (int*)calloc((size_t)s->num_sets * assoc, sizeof(int));
  s->etr_clock = (uns*)malloc(sizeof(uns) * s->num_sets);
  for (uns set = 0; set < s->num_sets; set++)
    s->etr_clock[set] = MJ_GRANULARITY;
  s->timestamp = (uns*)calloc(s->num_sets, sizeof(uns));
  s->sc = (Mj_Sample_Line*)calloc((size_t)s->num_sampled << MJ_LOG2_SAMPLED_CACHE_SETS,
                                  sizeof(Mj_Sample_Line) * MJ_SAMPLED_CACHE_WAYS);
  s->rdp = (int*)calloc((size_t)1 << s->sig_bits, sizeof(int));
  s->rdp_valid = (Flag*)calloc((size_t)1 << s->sig_bits, sizeof(Flag));

  /* Until mockingjay_configure says otherwise: one core, no stats */
  s->sharers = 1;
  s->core_bits = 0;
  s->pf_penalty = 2.0;
  s->stats_on = FALSE;

  cache->predictor = s;
}

void mockingjay_update_hit(Cache* cache, uns set, uns way, void* arg) {
  /* mockingjay_hit does a hit's work, once per request */
}

void mockingjay_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  Mj_State* s = mj_state(cache);
  ASSERTM(proc_id, g_mj_next_fill.valid && g_mj_next_fill.cache == cache,
          "%s: REPL_MOCKINGJAY insert without mockingjay_fill (only the MLC and L1 fills stage one)\n", cache->name);
  const Mj_Access* acc = &g_mj_next_fill.acc;
  ASSERT(proc_id, cache->entries[set][way].base == get_cache_line_addr(cache, acc->addr));
  g_mj_next_fill.valid = FALSE;

  Mj_Pred pred = mj_update(s, set, way, acc, FALSE, proc_id);
  if (acc->writeback) {
    mj_stat(s, proc_id, MJ_STAT_WB_FILL);
    return;
  }
  mj_stat(s, proc_id, MJ_STAT_FILL);
  if (pred == MJ_PRED_COLD)
    mj_stat(s, proc_id, MJ_STAT_FILL_COLD);
  else if (pred == MJ_PRED_SCAN)
    mj_stat(s, proc_id, MJ_STAT_FILL_SCAN);
}

Cache_Entry* mockingjay_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  for (uns w = 0; w < cache->assoc; w++) {
    if (!cache->entries[set][w].valid) {
      *way = w;
      return &cache->entries[set][w];
    }
  }

  Mj_State* s = mj_state(cache);
  int max_etr;
  *way = mj_victim(s, set, &max_etr);
  if (!if_external) {
    int etr = s->etr[set * s->assoc + *way];
    mj_stat(s, proc_id, etr > 0 ? MJ_STAT_EVICT_FUTURE : etr < 0 ? MJ_STAT_EVICT_PAST : MJ_STAT_EVICT_ZERO);
  }
  return &cache->entries[set][*way];
}

/**************************************************************************************/
/* Memory system hooks */

void mockingjay_configure(Cache* cache, uns sharers, Stat_Enum stat_base) {
  if (cache->repl_policy != REPL_MOCKINGJAY)
    return;
  Mj_State* s = mj_state(cache);
  ASSERT(0, sharers >= 1);
  s->sharers = sharers;
  /* The artifact's 4-core signature shifts in 2 bits of core id. */
  s->core_bits = sharers > 1 ? MAX2(2u, (uns)LOG2(sharers - 1) + 1) : 0;
  /* FLEXMIN_PENALTY = 2 - log2(NUM_CPUS) / 4 */
  s->pf_penalty = MOCKINGJAY_PF_PENALTY > 0.0 ? MOCKINGJAY_PF_PENALTY : 2.0 - log2((double)sharers) / 4.0;
  s->stats_on = TRUE;
  s->stat_base = stat_base;
}

void mockingjay_hit(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_MOCKINGJAY)
    return;
  Mj_Access acc;
  mj_req_access(cache, req, &acc);
  if (acc.writeback)
    return;
  uns set;
  int way = mj_find_way(cache, req->addr, &set);
  if (way < 0)
    return; /* served from elsewhere (--l1_pref_cache_enable's prefetch cache) */
  Mj_State* s = mj_state(cache);
  mj_update(s, set, way, &acc, TRUE, req->proc_id);
  mj_stat(s, req->proc_id, MJ_STAT_HIT);
}

Flag mockingjay_bypass_check(Cache* cache, Mem_Req* req, Flag dirty) {
  if (cache->repl_policy != REPL_MOCKINGJAY || !MOCKINGJAY_BYPASS || dirty)
    return FALSE;
  Mj_Access acc;
  mj_req_access(cache, req, &acc);
  if (acc.writeback)
    return FALSE;

  Addr tag, line_addr;
  uns set = ext_cache_index(cache, req->addr, &tag, &line_addr);
  for (uns w = 0; w < cache->assoc; w++) {
    if (!cache->entries[set][w].valid)
      return FALSE;
  }

  Mj_State* s = mj_state(cache);
  int max_etr;
  mj_victim(s, set, &max_etr);
  uns64 sig = mj_signature(s, acc.pc, FALSE, acc.prefetch, acc.core);
  return s->rdp_valid[sig] && !(s->rdp[sig] <= s->max_rd && s->rdp[sig] / MJ_GRANULARITY <= max_etr);
}

void mockingjay_bypass_commit(Cache* cache, Mem_Req* req) {
  ASSERT(req->proc_id, cache->repl_policy == REPL_MOCKINGJAY);
  Mj_Access acc;
  mj_req_access(cache, req, &acc);
  Addr tag, line_addr;
  uns set = ext_cache_index(cache, req->addr, &tag, &line_addr);
  Mj_State* s = mj_state(cache);
  mj_update(s, set, -1, &acc, FALSE, req->proc_id);
  mj_stat(s, req->proc_id, MJ_STAT_BYPASS);
}

Flag mockingjay_bypass(Cache* cache, Mem_Req* req, Flag dirty) {
  if (!mockingjay_bypass_check(cache, req, dirty))
    return FALSE;
  mockingjay_bypass_commit(cache, req);
  return TRUE;
}

void mockingjay_fill(Cache* cache, Mem_Req* req) {
  if (cache->repl_policy != REPL_MOCKINGJAY)
    return;
  g_mj_next_fill.valid = TRUE;
  g_mj_next_fill.cache = cache;
  mj_req_access(cache, req, &g_mj_next_fill.acc);
}
