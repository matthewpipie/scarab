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
 * File         : libs/cache_lib.h
 * Author       : HPS Research Group
 * Date         : 2/6/1998
 * Description  : Header for libs/cache_lib.c
 ***************************************************************************************/

#ifndef __CACHE_LIB_H__
#define __CACHE_LIB_H__

#include "globals/global_defs.h"

#include "libs/cache_index.h"
#include "libs/list_lib.h"

#ifdef __cplusplus
extern "C" {
#endif

/**************************************************************************************/

/* set data pointers to this initially */
#define INIT_CACHE_DATA_VALUE ((void*)0x8badbeef)

/**************************************************************************************/

typedef enum Repl_Policy_enum {
  REPL_TRUE_LRU,              /* actual least-recently-used replacement */
  REPL_RANDOM,                /* random replacement */
  REPL_NOT_MRU,               /* not-most-recently-used replacement */
  REPL_ROUND_ROBIN,           /* round-robin replacement */
  REPL_IDEAL,                 /* ideal replacement */
  REPL_ISO_PREF,              /* lru with some entries (isolated misses) higher priority */
  REPL_LOW_PREF,              /* prefetched data have lower priority */
  REPL_SHADOW_IDEAL,          /* ideal replacement with shadow cache */
  REPL_IDEAL_STORAGE,         /* if the data doesn't have a temporal locality then it
                                 isn't stored at the cache */
  REPL_MLP,                   /* mlp based replacement  -- uses MLP_REPL_POLICY */
  REPL_PARTITION,             /* Based on the partition*/
  REPL_RESTEER,               /* Prioritize the instr following a resteered branch or fetch barrier */
  REPL_STICKY_PRIORITY_LINES, /* Prioritize lines tagged with priority bit. */

  REPL_VOID,    /* void policy for loop ending and policy seperation */
  REPL_LRU_REF, /* least-recently-used replacement */
  REPL_NRU,     /* not recently used replacement */
  REPL_SRRIP,   /* static re-reference interval prediction */
  REPL_BRRIP,   /* bimodal re-reference interval prediction */
  REPL_DRRIP,   /* dynamic re-reference interval prediction */
  REPL_SHIP,    /* signature-based hit predictor */
  REPL_LACS,    /* locality-aware cost-sensitive replacement (Kharbutli & Sheikh, IEEE TC 2014) */
  REPL_ICARUS,  /* branch-history-contextualized criticality + reuse binning
                   (Kalbande et al., ASPLOS 2026) */

  NUM_REPL
} Repl_Policy;

/* The policies whose victim search is rrip_update_evict, where mlp_rrip_boost applies */
static inline Flag repl_is_rrip(Repl_Policy policy) {
  return policy == REPL_SRRIP || policy == REPL_BRRIP || policy == REPL_DRRIP || policy == REPL_SHIP;
}

/* --mlp_rrip_boost: how a marked line's boost acts on an RRIP policy (see memory.param.def) */
#define MLP_RRIP_BOOST_OFF 0
#define MLP_RRIP_BOOST_LAMBDA 1
#define MLP_RRIP_BOOST_GAMMA 2

typedef enum Cache_Repl_Signiture_enum {
  CACHE_REPL_SIGH_PC,
  CACHE_REPL_SIGH_MEM,
  CACHE_REPL_SIGH_NUM
} Cache_Repl_Signiture;

typedef struct Cache_Entry_struct {
  uns8 proc_id;
  Flag valid;               /* valid bit for the line */
  Addr tag;                 /* tag for the line */
  Addr tag_full;            /* original tag before folding */
  Addr base;                /* address of first element */
  Counter last_access_time; /* for replacement policy */
  Counter insertion_time;   /* for replacement policy */
  void* data;               /* pointer to arbitrary data */
  Flag pref;                /* extra replacement info */
  Flag dirty;               /* Dirty bit should have been here, however this is used only in warmup now */
  Addr pw_start_addr;       /* for uop cache: start addr of prediction window */

  uns8 reference_val; /* for re-reference replacement policy */
  Flag outcome;       /* for replacement policy */

  float mlp_boost; /* REPL_MLP: eviction-priority boost staged at fill (mlp_lin_data_lambda or
                       mlp_lin_instr_lambda if the filling load/fetch cleared its boundness
                       threshold, else 0) */
  Counter mlp_stamp; /* REPL_MLP with mlp_boost_in_fills: the set's fill count at the line's last
                        recency update (fill or hit) */
  uns8 mlp_ticks_left; /* REPL_MLP with mlp_boost_countdown: set ticks until the line's protection
                          expires; it is protected while this is nonzero */
  uns mlp_rrip_credit; /* RRIP with mlp_rrip_boost 2: aging steps the line still absorbs before its
                          RRPV rises; set to its boost at its fill and at every hit */
  uns8 mlp_mark;       /* REPL_MLP (stats only): which rule marked the line at its fill, MLP_MARK_* */
  Flag mlp_used;       /* stats only: a demand access hit the line since its fill (cache_mlp_demand_hit) */

  Flag icarus_critical; /* REPL_ICARUS: CIT said this (instruction) line was critical at fill
                            time; always FALSE for data lines */
  Flag icarus_reused;   /* REPL_ICARUS: hit at least once since it was filled */
} Cache_Entry;

// DO NOT CHANGE THIS ORDER
typedef enum Cache_Insert_Repl_enum {
  INSERT_REPL_DEFAULT = 0, /* Insert with default replacement information */
  INSERT_REPL_LRU,         /* Insert into LRU position */
  INSERT_REPL_LOWQTR,      /* Insert such that it is Quarter(Roughly) of the repl order*/
  INSERT_REPL_MID,         /* Insert such that it is Middle(Roughly) of the repl order*/
  INSERT_REPL_MRU,         /* Insert into MRU position */
  NUM_INSERT_REPL
} Cache_Insert_Repl;

struct Cache_struct {
  char name[MAX_STR_LENGTH + 1]; /* name to identify the cache (for debugging) */
  uns data_size;                 /* how big are the data items in each cache entry? (for malloc) */

  uns assoc;               /* associativity */
  uns num_lines;           /* number of lines in the cache */
  uns num_sets;            /* number of sets in the cache */
  uns line_size;           /* size in bytes of one line */
  Repl_Policy repl_policy; /* the replacement policy of the cache */

  Cache_Index_State cache_index_state;

  /* each mask looks like this:
   *      addr: MSB [tag_bits][set_bits][shift_bits] LSB
   *      set_mask:  00000000  11111111
   *      tag_mask:  11111111  00000000
   *    shift_mask:  00000000  00000000  1111111111
   * tag_pure_mask:  11111111
   */
  uns set_bits;       /* number of bits used in the set mask */
  uns tag_bits;       /* number of bits used in the tag mask */
  uns shift_bits;     /* number of bits to shift an address before using (assuming it is shifted) */
  Addr set_mask;      /* mask applied after shifting to get the index */
  Addr tag_mask;      /* mask used to get the tag after shifting */
  Addr offset_mask;   /* mask used to get the line offset */
  Addr tag_pure_mask; /* mask used to get the folded tag, no 0-paddings in LSB side */

  uns* repl_ctrs; /* replacement info */

  /* A dynamically allocated array of all of the cache entries. The array is two-dimensional, sets are row major. */
  Cache_Entry** entries;

  /* A linked list for each set in the cache that is used when simulating ideal replacement policies */
  List* unsure_lists;

  Flag perfect;                 /* is the cache perfect (for henry mem system) */
  uns repl_pref_thresh;         /* threshhold for how many entries are high-priority. */
  Cache_Entry** shadow_entries; /* A dynamically allocated array for shadow cache */
  uns* queue_end;               /* queue pointer for ideal storage */

  Counter num_demand_access;
  Counter last_update; /* last update cycle */

  uns* num_ways_allocted_core; /* For cache partitioning */
  uns* num_ways_occupied_core; /* For cache partitioning */
  uns* lru_index_core;         /* For cache partitioning */
  Counter* lru_time_core;      /* For cache partitioning */

  Flag tag_incl_offset; /* The uop cache is byte-addressable, so the tag includes offset bits as well */

  /* For BRRIP repl */
  Counter bimodal_count;

  /* For repl with predictor */
  void* predictor;

  Counter* mlp_set_fills; /* REPL_MLP: fills into each set so far, the clock for mlp_boost_in_fills */
  /* stats only: the line the last cache_insert replaced (valid FALSE if it took an invalid way) */
  Flag mlp_victim_valid;
  uns8 mlp_victim_mark;
  Flag mlp_victim_used;
};

/**************************************************************************************/
/* Strategy Design */
struct repl_policy_func {
  Repl_Policy repl_policy_type;

  void (*action_init)(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
  void (*action_repl)(Cache*, Cache_Entry*, uns8, Addr, Addr, Addr*, Addr*);

  void (*update_hit)(Cache*, uns, uns, void*);
  void (*update_insert)(Cache*, uns8, uns, uns, void*);
  Cache_Entry* (*update_evict)(Cache*, uns8, uns, uns*, void*, Flag);
};

/* Driven Table */
extern struct repl_policy_func repl_policy_func_table[NUM_REPL];

/* Strategy Function */
void init_cache_strategy(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void* cache_insert_strategy(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr);
void* cache_access_strategy(Cache* cache, Addr addr, Addr* line_addr, Flag* tag_aliasing, Flag update_repl);
Cache_Entry* cache_evict_strategy(Cache* cache, uns8 proc_id, uns set, uns* way);

const static Flag CACHE_DEBUG_ENABLE = FALSE;  // To be Changed into DEBUG_PARA

/**************************************************************************************/
/* prototypes */

void init_cache(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void init_cache_impl(Cache*, const char*, uns, uns, uns, uns, uns, Repl_Policy, Cache_Index_Config);
void* cache_access(Cache*, Addr, Addr*, Flag);
void* cache_access_impl(Cache*, Addr, Addr*, Flag*, Flag);
void* cache_insert(Cache*, uns8, Addr, Addr*, Addr*);
void* cache_insert_replpos(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr,
                           Cache_Insert_Repl insert_repl_policy, Flag isPrefetch);
void* cache_insert_lru(Cache*, uns8, Addr, Addr*, Addr*);
void cache_invalidate(Cache*, Addr, Addr*);
void cache_flush(Cache*);
void* get_next_repl_line(Cache*, uns8, Addr, Addr*, Flag*);
void* get_next_valid_repl_line(Cache* cache, uns8 proc_id, Addr addr);
uns ext_cache_index(Cache*, Addr, Addr*, Addr*);
Addr get_cache_line_addr(Cache*, Addr);
uns cache_get_invalid_line_count(Cache* cache, Addr addr);
void update_repl_resteer_policy(Cache*, Addr);

/* REPL_MLP, or an RRIP policy with mlp_rrip_boost: stage the boost the NEXT cache_insert_replpos
   (or, for RRIP, cache_insert_strategy) call should stamp on the new line. Call right before a
   fill-time cache_insert on such a cache; reset to 0 after. */
void cache_set_next_fill_mlp_boost(float boost);

/* REPL_MLP (stats only): which rule marked a line at its fill. Staged with the boost, by
   cache_set_next_fill_mlp_mark, and kept on the line until eviction. */
#define MLP_MARK_NONE 0
#define MLP_MARK_DATA 1
#define MLP_MARK_INSTR 2
void cache_set_next_fill_mlp_mark(uns8 mark);
/* The set addr maps to. */
uns cache_set_index(Cache* cache, Addr addr);
/* The mark of addr's line, or MLP_MARK_NONE if it is not resident. */
uns8 cache_mlp_mark(Cache* cache, Addr addr);
/* A demand access (load, store or instruction fetch) hit addr's line: note it used. Returns its
   mark, as cache_mlp_mark. */
uns8 cache_mlp_demand_hit(Cache* cache, Addr addr);
/* Whether addr's line is resident, marked, and past its protection: with mlp_boost_in_fills, its
   boost or more fills into its set since its last touch; with mlp_boost_countdown, no ticks left.
   A stack-position boost (neither) never lapses. Read it before the access renews the line. */
Flag cache_mlp_lapsed(Cache* cache, Addr addr);
/* Add to each count the cache's valid lines, data- and instruction-marked lines, and marked lines
   whose protection is still in effect (see MLP_OCC_INWINDOW in memory.stat.def). */
void cache_mlp_census(Cache* cache, Counter* valid, Counter* dmark, Counter* imark, Counter* inwindow);

/* REPL_LACS: stage the cost the NEXT cache_insert (strategy path) should stamp on the new
   line. Call right before a fill-time cache_insert on a cache running REPL_LACS; `valid`
   FALSE means "use the policy's default," not "use cost 0." Consumed and reset by
   lacs_update_insert. */
void cache_set_lacs_next_insert(Flag valid, uns cost);

/* REPL_LACS: this cache's eviction totals (real evictions, how many of those first had to
   age the set, how many had more than one same-cost candidate), for end-of-run stats. */
void cache_lacs_counters(Cache* cache, Counter* evictions, Counter* evict_aging, Counter* evict_ties);

/* REPL_ICARUS: stage the criticality bit the NEXT cache_insert[_replpos] call should stamp on
   the new line. Call right before a fill-time cache_insert on a cache running REPL_ICARUS;
   `valid` FALSE means "use the default (not critical)," not "critical == FALSE" specifically.
   Consumed and reset by icarus_update_insert. */
void cache_set_icarus_next_insert(Flag valid, Flag critical);

/* REPL_ICARUS: set the critical bit of the resident line holding `addr` (the CIT's
   criticality signal on a hit). Leaves LRU state alone. Returns TRUE if newly set. */
Flag cache_icarus_mark_critical(Cache* cache, Addr addr);

void* shadow_cache_insert(Cache* cache, uns set, Addr tag, Addr tag_full, Addr base);
void* access_shadow_lines(Cache* cache, uns set, Addr tag, Addr tag_full, Flag* tag_aliasing);
void* access_ideal_storage(Cache* cache, uns set, Addr tag, Addr tag_full, Addr addr, Flag* tag_aliasing);
void reset_cache(Cache*);
int cache_find_pos_in_lru_stack(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr);
void set_partition_allocate(Cache* cache, uns8 proc_id, uns num_ways);
uns get_partition_allocated(Cache* cache, uns8 proc_id);

/**************************************************************************************/

#ifdef __cplusplus
}
#endif

#endif /* #ifndef __CACHE_LIB_H__ */
