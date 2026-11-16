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

  REPL_VOID,           /* void policy for loop ending and policy seperation */
  REPL_LRU_REF,        /* least-recently-used replacement */
  REPL_NRU,            /* not recently used replacement */
  REPL_SRRIP,          /* static re-reference interval prediction */
  REPL_BRRIP,          /* bimodal re-reference interval prediction */
  REPL_DRRIP,          /* dynamic re-reference interval prediction */
  REPL_SHIP,           /* signature-based hit predictor */
  REPL_LACS,           /* locality-aware cost-sensitive replacement (Kharbutli & Sheikh, IEEE TC 2014) */
  REPL_ICARUS,         /* branch-history-contextualized criticality + reuse binning
                          (Kalbande et al., ASPLOS 2026) */
  REPL_MOCKINGJAY,     /* reuse-distance prediction mimicking Belady's MIN (Shah, Jain & Lin,
                          HPCA 2022); MLC or L1 only, see memory/mockingjay.c */
  REPL_INST_PARTITION, /* fixed instruction/data way partition at the MLC -- see
                          --mlc_inst_part_ways and the REPL_INST_PARTITION section of
                          libs/cache_lib.c */
  REPL_EMISSARY,       /* protect up to N starvation-causing instruction lines per set
                          (Nagendra, Godala et al., ISCA 2023); MLC only, see
                          memory/emissary.c */
  REPL_CLIP,           /* Code Line Preservation (Jaleel et al., HPCA 2015): RRIP that inserts
                          data distant and never promotes it, set-sampled; MLC only -- see the
                          REPL_CLIP section of libs/cache_lib.c */
  REPL_SHIPPP,         /* SHiP++ (Young et al., CRC2 2017): SHiP-PC-S with its five enhancements
                          (--mlc_shippp_*); MLC only, see memory/shippp.c */

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

/* REPL_MLP's mlp_boost_countdown, and REPL_MOCKINGJAY's mlp_mj_boost: a marked line is protected for
   this many ticks of its set (a 2-bit countdown), and a set ticks every round(boost /
   MLP_COUNTDOWN_TICKS) units of the policy's clock (REPL_MLP: fills; Mockingjay: aging steps). */
#define MLP_COUNTDOWN_TICKS 3

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

  float mlp_boost;     /* RRIP with mlp_rrip_boost, or REPL_MOCKINGJAY with mlp_mj_boost: eviction-priority
                           boost staged at fill (see cache_set_next_fill_mlp_boost). Not used by REPL_MLP
                           itself -- see mlp_mark. */
  Counter mlp_stamp;   /* REPL_MLP with mlp_boost_in_fills: the set's fill count at the line's last
                          recency update (fill or hit) */
  uns8 mlp_ticks_left; /* REPL_MLP with mlp_boost_countdown, or REPL_MOCKINGJAY with mlp_mj_boost: set
                          ticks until the line's protection expires; it is protected while this is
                          nonzero */
  uns mlp_rrip_credit; /* RRIP with mlp_rrip_boost 2: aging steps the line still absorbs before its
                          RRPV rises; set to its boost at its fill and at every hit */
  uns8 mlp_mark;       /* stats only: which rule marked the line at its fill, MLP_MARK_* (the MLC
                          marks under any policy; see mlp_stage_fill_boost) */
  Flag mlp_used;       /* stats only: a demand access hit the line since its fill (cache_mlp_demand_hit) */

  uns8 cmix; /* stats only (cache_mix): CMIX_* bits, 0 until the cache's owner classifies the
                 line after inserting it (cache_cmix_fill) */

  Flag icarus_critical; /* REPL_ICARUS: CIT said this (instruction) line was critical at fill
                            time; always FALSE for data lines */
  Flag icarus_reused;   /* REPL_ICARUS: hit at least once since it was filled */

  Flag part_is_inst; /* REPL_INST_PARTITION: TRUE if this line was filled by an instruction-side
                         request (mem_req_type_is_inst: a demand fetch or an FDIP prefetch), set
                         once at fill via cache_set_inst_part_next_insert. Determines which of
                         the set's two way-quotas (--mlc_inst_part_ways instruction ways, the
                         rest data) this line counts against. */

  Flag emissary_p; /* EMISSARY: the line's priority bit (P = 1: high priority). Cleared at every
                       insert. On the icache, set at fill when the filling miss is selected; on
                       the MLC, set when the icache evicts its copy with P = 1 (memory/emissary.c).
                       Only REPL_EMISSARY reads it. */
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

  /* Who tells the replacement policy that a lookup missed (update_miss). FALSE: every insert
     counts as one miss, which holds for a cache that inserts on its own misses. TRUE: the owner
     calls cache_report_miss() once per miss instead -- needed where inserts are not misses, as at
     a level of an exclusive hierarchy, which is filled by demotions from the level above. */
  Flag repl_miss_by_report;

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
  void (*update_miss)(Cache*, uns8, uns, void*); /* a lookup missed in this set; NULL: not used */
};

/* Driven Table */
extern struct repl_policy_func repl_policy_func_table[NUM_REPL];

/* Strategy Function */
void init_cache_strategy(Cache*, const char*, uns, uns, uns, uns, Repl_Policy);
void* cache_insert_strategy(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr);
void* cache_access_strategy(Cache* cache, Addr addr, Addr* line_addr, Flag* tag_aliasing, Flag update_repl);
Cache_Entry* cache_evict_strategy(Cache* cache, uns8 proc_id, uns set, uns* way);
void cache_report_miss(Cache* cache, uns8 proc_id, Addr addr);

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
/* Whether addr's line is resident, was boosted at its fill, and is past its protection: with
   mlp_boost_in_fills, its boost or more fills into its set since its last touch; with
   mlp_boost_countdown, no ticks left. A stack-position boost (neither) never lapses. A marked line
   with no boost (the cache does not boost, or the side's lambda is 0) was never protected, so it
   does not lapse either. Read it before the access renews the line. */
Flag cache_mlp_lapsed(Cache* cache, Addr addr);
/* Add to each count the cache's valid lines, data- and instruction-marked lines, and marked lines
   whose protection is still in effect (see MLP_OCC_INWINDOW in memory.stat.def). */
void cache_mlp_census(Cache* cache, Counter* valid, Counter* dmark, Counter* imark, Counter* inwindow);

/* Stats only (cache_mix; CMIX_* in memory.stat.def): whether a line holds instructions or data,
   and whether it was brought in by a prefetch that no demand access has hit at this cache yet.
   A demand is an instruction fetch, load or store (on or off path); a prefetch is any request
   mem_req_type_is_prefetch names, unless a demand merged into it before its fill. */
#define CMIX_SET 0x1  /* classified: an insert leaves cmix 0 until the owner calls cache_cmix_fill */
#define CMIX_INST 0x2 /* an instruction line, else data */
#define CMIX_PREF 0x4 /* prefetched, no demand access here yet */
typedef enum Cmix_Cat_enum {
  CMIX_CAT_INST_DEMAND, /* instruction line a demand filled or has since hit */
  CMIX_CAT_INST_PREF,   /* instruction line prefetched, not yet demanded */
  CMIX_CAT_DATA_DEMAND,
  CMIX_CAT_DATA_PREF,
  CMIX_CAT_UNKNOWN, /* valid but never classified: an insert path cache_cmix_fill does not cover */
  CMIX_CAT_INVALID, /* empty way */
  CMIX_NUM_CATS
} Cmix_Cat;
static inline uns8 cmix_class(Flag inst, Flag pref) {
  return CMIX_SET | (inst ? CMIX_INST : 0) | (pref ? CMIX_PREF : 0);
}
/* Classify addr's line after a fill (cls from cmix_class). A line already classified -- a fill
   of a resident line, which keeps its way -- keeps its side and is only marked demanded, if
   this fill is a demand's. No-op if addr is not resident. */
void cache_cmix_fill(Cache* cache, Addr addr, uns8 cls);
/* A demand access hit addr's line: it is no longer an unused prefetch. */
void cache_cmix_demand(Cache* cache, Addr addr);
/* addr's line's CMIX_* bits, 0 if it is not resident; read a victim's before inserting over it. */
uns8 cache_cmix_get(Cache* cache, Addr addr);
/* Add the cache's lines, by Cmix_Cat, to counts. */
void cache_cmix_census(Cache* cache, Counter counts[CMIX_NUM_CATS]);

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

/* REPL_INST_PARTITION: stage whether the NEXT cache_insert[_replpos] call's fill is on the
   instruction side (mem_req_type_is_inst(req->type)) or the data side. Call right before a
   fill-time cache_insert on a cache running REPL_INST_PARTITION. Consumed and reset by
   inst_part_update_insert; also read (without being consumed) by inst_part_update_evict to
   break ties when both quotas are exactly met (see cache_lib.c). */
void cache_set_inst_part_next_insert(Flag is_inst);

/* EMISSARY: the P bit (Cache_Entry.emissary_p) on any cache. None of these touch replacement
   state. */
typedef enum Cache_Emissary_Mark_enum {
  CACHE_EMISSARY_ABSENT,      /* the cache does not hold the line */
  CACHE_EMISSARY_NEWLY_SET,   /* P went from 0 to 1 */
  CACHE_EMISSARY_ALREADY_SET, /* P was already 1 */
} Cache_Emissary_Mark;

/* Whether the line the next cache_insert of `addr` would evict is valid and has P = 1. */
Flag cache_emissary_victim_p(Cache* cache, uns8 proc_id, Addr addr);
/* Set P on the resident line holding `addr`, if there is one. */
Cache_Emissary_Mark cache_emissary_set_p(Cache* cache, Addr addr);
/* Clear every P bit in the cache; returns how many were set. */
Counter cache_emissary_clear_all(Cache* cache);
/* hist[k] += the number of sets holding k valid P = 1 lines; k is capped at nbuckets - 1. */
void cache_emissary_set_census(Cache* cache, Counter* hist, uns nbuckets);

/* REPL_CLIP: stage whether the request behind the NEXT replacement update of this cache -- a hit
   (cache_access with update_repl) or a fill (cache_insert[_replpos]) -- is code. Call it with
   valid TRUE right before that call and with valid FALSE right after, so a lookup that missed
   leaves nothing behind; the update consumes it, and an update with nothing staged is an error.
   No-op on a cache running another policy. */
void cache_clip_stage(Cache* cache, Flag valid, Flag is_code);

/* REPL_CLIP: a request's lookup of addr hit or missed. Call once per request (a writeback is
   not a lookup). If addr's set is sampled, it counts toward the code share CLIP's mode follows.
   No-op on a cache running another policy. */
void cache_clip_note_lookup(Cache* cache, uns8 proc_id, Addr addr, Flag is_code, Flag hit);

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
