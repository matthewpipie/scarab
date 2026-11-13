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
 * File         : libs/cache_lib.c
 * Author       : HPS Research Group
 * Date         : 2/6/1998
 * Description  : This is a library of cache functions.
 ***************************************************************************************/

#include "libs/cache_lib.h"

#include <stdlib.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"

#include "core.param.h"
#include "general.param.h"
#include "memory/memory.param.h"

#include "frontend/frontend_intf.h"

#include "statistics.h"

// DeleteMe
#define ideal_num_entries 256

/**************************************************************************************/
/* Macros */

#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_CACHE_LIB, ##args)

/* REPL_MLP: staged by memory.c right before a fill-time cache_insert on a cache running
   REPL_MLP, consumed by cache_insert_replpos when it stamps the new Cache_Entry. */
static float g_next_fill_mlp_boost = 0.0;

void cache_set_next_fill_mlp_boost(float boost) {
  g_next_fill_mlp_boost = boost;
}

static uns8 g_next_fill_mlp_mark = MLP_MARK_NONE;

void cache_set_next_fill_mlp_mark(uns8 mark) {
  g_next_fill_mlp_mark = mark;
}

/* REPL_LACS: staged by memory/lacs.c right before a fill-time cache_insert on a cache
   running REPL_LACS, consumed by lacs_update_insert (the strategy table's update_insert,
   since REPL_LACS -- unlike REPL_MLP -- sits in the strategy-table group). !valid means
   "the policy's own default," not "cost 0." */
static Flag g_lacs_next_valid = FALSE;
static uns g_lacs_next_cost = 0;

void cache_set_lacs_next_insert(Flag valid, uns cost) {
  g_lacs_next_valid = valid;
  g_lacs_next_cost = cost;
}

/* Stats only: remember the line an insert is about to replace (memory.c reads it for MLP_EVICT_*). */
static inline void mlp_note_victim(Cache* cache, Cache_Entry* victim) {
  cache->mlp_victim_valid = victim->valid;
  cache->mlp_victim_mark = victim->mlp_mark;
  cache->mlp_victim_used = victim->mlp_used;
}

/* REPL_MLP with mlp_boost_countdown: a marked line is protected for this many ticks of its set
   (a 2-bit countdown), and a set ticks every round(mlp_boost / MLP_COUNTDOWN_TICKS) fills. */
#define MLP_COUNTDOWN_TICKS 3

/* REPL_MLP keeps its boost as a countdown (mlp_boost_countdown, on by default) unless
   mlp_boost_in_fills asks for the fill-count age instead. */
static inline Flag mlp_countdown_on(void) {
  return MLP_BOOST_COUNTDOWN && !MLP_BOOST_IN_FILLS;
}

/* REPL_MLP with mlp_boost_countdown: (re)start a marked line's countdown, on its fill or a hit. */
static inline void mlp_countdown_start(Cache_Entry* entry) {
  entry->mlp_ticks_left = mlp_countdown_on() && entry->mlp_boost > 0.0 ? MLP_COUNTDOWN_TICKS : 0;
}

/* REPL_MLP with mlp_boost_countdown: after a fill into `set` at fill_way, decrement the countdown
   of each other protected line this fill is a tick for. A line whose countdown reaches 0 moves to
   MRU, as mlp_boost_in_fills ranks a marked line mlp_boost fills after its last touch as if
   touched then. */
static void mlp_countdown_tick(Cache* cache, uns8 proc_id, uns set, uns fill_way) {
  uns ii;
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];
    if (ii == fill_way || !entry->valid || !entry->mlp_ticks_left)
      continue;
    Counter period = MAX2((Counter)1, (Counter)(entry->mlp_boost / MLP_COUNTDOWN_TICKS + 0.5));
    if (cache->mlp_set_fills[set] % period || --entry->mlp_ticks_left)
      continue; /* not a tick for this line, or ticks left */
    entry->last_access_time = sim_time;
    STAT_EVENT(proc_id, MLP_COUNTDOWN_EXPIRE);
  }
}

/* From memory/icarus.c tell cache_insert() (which knows nothing of a line's criticality)
that the next fill should be marked critical, set when a line is determined to be critical
by icarus_fill, and reset to 0 after each fill.  */

static Flag g_icarus_next_valid = FALSE;
static Flag g_icarus_next_critical = FALSE;

void cache_set_icarus_next_insert(Flag valid, Flag critical) {
  g_icarus_next_valid = valid;
  g_icarus_next_critical = critical;
}

/**************************************************************************************/
/* Static Prototypes */

static inline uns cache_index(Cache* cache, Addr addr, Addr* tag, Addr* tag_full, Addr* line_addr);
static inline void update_repl_policy(Cache*, Cache_Entry*, uns, uns, Flag);
static inline Cache_Entry* find_repl_entry(Cache*, uns8, uns, uns*);

/* for ideal replacement */
static inline void* access_unsure_lines(Cache*, uns, Addr, Flag);
static inline Cache_Entry* insert_sure_line(Cache*, uns, Addr);
static inline void invalidate_unsure_line(Cache*, uns, Addr);

/**************************************************************************************/
/* Global Variables */

char rand_repl_state[31];

/**************************************************************************************/

static inline uns cache_index(Cache* cache, Addr addr, Addr* tag, Addr* tag_full, Addr* line_addr) {
  if (cache->tag_incl_offset) {
    *tag = *tag_full = addr & ~(cache->set_mask << cache->shift_bits);
    *line_addr = addr;  // When the tag incl offset, cache is BYTE-addressable
  } else {
    Addr _tag;
    _tag = *tag_full = addr >> cache->shift_bits;
    // chunk XOR folding
    if (cache->tag_bits < 64) {
      while ((_tag >> cache->tag_bits) != 0) {
        _tag = (_tag & cache->tag_pure_mask) ^ (_tag >> cache->tag_bits);
      }
    }
    *tag = _tag & cache->tag_pure_mask;
    *line_addr = addr & ~cache->offset_mask;
  }
  return cache_index_get_set(cache, addr);
}

uns ext_cache_index(Cache* cache, Addr addr, Addr* tag, Addr* line_addr) {
  Addr tag_full;
  return cache_index(cache, addr, tag, &tag_full, line_addr);
}

/**************************************************************************************/
/* init_cache: */

void init_cache(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                Repl_Policy repl_policy) {
  init_cache_impl(cache, name, cache_size, assoc, line_size, 64, data_size, repl_policy,
                  get_default_cache_index_config());
}

void init_cache_impl(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns tag_bits,
                     uns data_size, Repl_Policy repl_policy, Cache_Index_Config idx_cfg) {
  /* The offset and set index are bit fields (shift_bits, set_mask below), so both the line size and
     the set count must be powers of two. Otherwise the sets above the largest power of two are
     allocated but never indexed, and the cache silently holds less than configured. */
  ASSERTUM(0, line_size && !(line_size & (line_size - 1)), "%s: line size %u is not a power of two\n", name, line_size);
  ASSERTUM(0, assoc, "%s: associativity is 0\n", name);
  uns num_lines = cache_size / line_size;
  uns num_sets = cache_size / line_size / assoc;
  ASSERTUM(0, num_sets * assoc * line_size == cache_size && num_sets && !(num_sets & (num_sets - 1)),
           "%s: size %u / assoc %u / line size %u is not a power-of-two number of sets\n", name, cache_size, assoc,
           line_size);
  uns ii, jj;

  DEBUG(0, "Initializing cache called '%s'.\n", name);

  /* set the basic parameters */
  strncpy(cache->name, name, MAX_STR_LENGTH);
  cache->data_size = data_size;
  cache->num_lines = num_lines;
  cache->assoc = assoc;
  cache->num_sets = num_sets;
  cache->line_size = line_size;
  cache->repl_policy = repl_policy;

  /* set some fields to make indexing quick */
  cache->set_bits = LOG2(num_sets);
  cache->tag_bits = tag_bits;
  cache->shift_bits = LOG2(line_size);          /* use for shift amt. */
  cache->set_mask = N_BIT_MASK(LOG2(num_sets)); /* use after shifting */
  cache->tag_pure_mask = tag_bits == 64 ? N_BIT_MASK_64 : N_BIT_MASK(tag_bits);
  cache->tag_mask = cache->tag_pure_mask << LOG2(num_sets); /* use after shifting */
  cache->offset_mask = N_BIT_MASK(cache->shift_bits);       /* use before shifting */

  /* set up the index-hash state */
  cache_index_state_init(cache, idx_cfg);

  /* allocate memory for NMRU replacement counters  */
  if (repl_policy < REPL_VOID)
    cache->repl_ctrs = (uns*)calloc(num_sets, sizeof(uns));

  cache->mlp_set_fills = repl_policy == REPL_MLP ? (Counter*)calloc(num_sets, sizeof(Counter)) : NULL;
  cache->mlp_victim_valid = FALSE;
  cache->mlp_victim_mark = MLP_MARK_NONE;
  cache->mlp_victim_used = FALSE;
  if (repl_policy == REPL_MLP)
    ASSERTM(0, !MLP_BOOST_COUNTDOWN_NO_REVIVE || mlp_countdown_on(),
            "mlp_boost_countdown_no_revive requires mlp_boost_countdown without mlp_boost_in_fills\n");

  /* allocate memory for all the sets (pointers to line arrays)  */
  cache->entries = (Cache_Entry**)malloc(sizeof(Cache_Entry*) * num_sets);

  /* allocate memory for the unsure lists (if necessary) */
  if (cache->repl_policy == REPL_IDEAL)
    cache->unsure_lists = (List*)malloc(sizeof(List) * num_sets);

  /* allocate memory for all of the lines in each set */
  for (ii = 0; ii < num_sets; ii++) {
    cache->entries[ii] = (Cache_Entry*)malloc(sizeof(Cache_Entry) * assoc);
    /* allocate memory for all of the data elements in each line */
    for (jj = 0; jj < assoc; jj++) {
      cache->entries[ii][jj].valid = FALSE;
      cache->entries[ii][jj].mlp_ticks_left = 0;
      cache->entries[ii][jj].mlp_boost = 0.0;
      cache->entries[ii][jj].mlp_rrip_credit = 0;
      cache->entries[ii][jj].mlp_mark = MLP_MARK_NONE;
      cache->entries[ii][jj].mlp_used = FALSE;
      if (data_size) {
        cache->entries[ii][jj].data = (void*)malloc(data_size);
        memset(cache->entries[ii][jj].data, 0, data_size);
      } else
        cache->entries[ii][jj].data = INIT_CACHE_DATA_VALUE;
    }

    /* initialize the unsure lists (if necessary) */
    if (cache->repl_policy == REPL_IDEAL) {
      char list_name[MAX_STR_LENGTH + 1];
      // 21 guaruntees the string will always be smaller than MAX_STR_LENGTH
      snprintf(list_name, MAX_STR_LENGTH, "%.*s unsure [%d]", MAX_STR_LENGTH - 20, cache->name, ii);
      init_list(&cache->unsure_lists[ii], list_name, sizeof(Cache_Entry), USE_UNSURE_FREE_LISTS);
    }
  }
  cache->num_demand_access = 0;
  cache->last_update = 0;

  /* For cache partitioning */
  if (cache->repl_policy == REPL_PARTITION) {
    cache->num_ways_allocted_core = (uns*)calloc(NUM_CORES, sizeof(uns));
    cache->num_ways_occupied_core = (uns*)calloc(NUM_CORES, sizeof(uns));
    cache->lru_index_core = (uns*)calloc(NUM_CORES, sizeof(uns));
    cache->lru_time_core = (Counter*)calloc(NUM_CORES, sizeof(Counter));
  }

  /* allocate memory for the back-up lists (if necessary) */
  if (cache->repl_policy == REPL_SHADOW_IDEAL) {
    cache->shadow_entries = (Cache_Entry**)malloc(sizeof(Cache_Entry*) * num_sets);
    /* allocate memory for all of the lines in each set */
    for (ii = 0; ii < num_sets; ii++) {
      cache->shadow_entries[ii] = (Cache_Entry*)malloc(sizeof(Cache_Entry) * assoc);
      /* allocate memory for all of the data elements in each line */
      for (jj = 0; jj < assoc; jj++) {
        cache->shadow_entries[ii][jj].valid = FALSE;
        if (data_size) {
          cache->shadow_entries[ii][jj].data = (void*)malloc(data_size);
          memset(cache->shadow_entries[ii][jj].data, 0, data_size);
        } else
          cache->shadow_entries[ii][jj].data = INIT_CACHE_DATA_VALUE;
      }
    }
  }

  else if (cache->repl_policy == REPL_IDEAL_STORAGE) {
    cache->shadow_entries = (Cache_Entry**)malloc(sizeof(Cache_Entry*) * num_sets);
    cache->queue_end = (uns*)malloc(sizeof(uns) * num_sets);
    /* allocate memory for all of the lines in each set */
    for (ii = 0; ii < num_sets; ii++) {
      cache->shadow_entries[ii] = (Cache_Entry*)malloc(sizeof(Cache_Entry) * ideal_num_entries);
      /* allocate memory for all of the data elements in each line */
      for (jj = 0; jj < ideal_num_entries; jj++) {
        cache->shadow_entries[ii][jj].valid = FALSE;
        if (data_size) {
          cache->shadow_entries[ii][jj].data = (void*)malloc(data_size);
          memset(cache->shadow_entries[ii][jj].data, 0, data_size);
        } else
          cache->shadow_entries[ii][jj].data = INIT_CACHE_DATA_VALUE;
      }
      cache->queue_end[ii] = 0;
    }
  }

  if (repl_policy >= REPL_VOID)
    init_cache_strategy(cache, name, cache_size, assoc, line_size, data_size, repl_policy);

  cache->tag_incl_offset = FALSE;
}

/**************************************************************************************/
/* cache_access: Does a cache lookup based on the address.  Returns a pointer
 * to the cache line data if it is found.  */

void* cache_access(Cache* cache, Addr addr, Addr* line_addr, Flag update_repl) {
  Flag tag_aliasing;
  return cache_access_impl(cache, addr, line_addr, &tag_aliasing, update_repl);
}

void* cache_access_impl(Cache* cache, Addr addr, Addr* line_addr, Flag* tag_aliasing, Flag update_repl) {
  Addr tag, tag_full;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);
  uns ii;
  void* line_data = NULL;

  if (cache->repl_policy >= REPL_VOID)
    line_data = cache_access_strategy(cache, addr, line_addr, tag_aliasing, update_repl);

  else if (cache->repl_policy == REPL_IDEAL_STORAGE)
    line_data = access_ideal_storage(cache, set, tag, tag_full, addr, tag_aliasing);

  else {
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* line = &cache->entries[set][ii];

      if (line->valid && line->tag == tag) {
        /* update replacement state if necessary */
        ASSERT(0, line->data);
        DEBUG(0, "Found line in cache '%s' at (set %u, way %u, base 0x%s)\n", cache->name, set, ii,
              hexstr64s(line->base));

        if (update_repl) {
          if (line->pref) {
            line->pref = FALSE;
          }
          cache->num_demand_access++;
          update_repl_policy(cache, line, set, ii, FALSE);
          DEBUG(0, "(%s, %d) [0x%x, 0x%x]: in access\n\n", cache->name, cache->repl_policy, cache->num_sets,
                cache->assoc);
        }

        *tag_aliasing = line->tag_full != tag_full;
        line_data = line->data;
      }
    }
  }

  /* Drive adaptive-index training/remapping on demand accesses. Done after the lookup
     so this access used the pre-update index function. No-op for static index hashes. */
  if (update_repl)
    cache_index_on_access(cache, addr, line_data != NULL);

  if (line_data)
    return line_data;

  /* if it's a miss and we're doing ideal replacement, look in the unsure list
   */
  if (cache->repl_policy == REPL_IDEAL) {
    DEBUG(0, "Checking unsure list '%s' at (set %u)\n", cache->name, set);
    return access_unsure_lines(cache, set, tag, update_repl);
  }

  if (cache->repl_policy == REPL_SHADOW_IDEAL) {
    DEBUG(0, "Checking shadow cache '%s' at (set %u), base 0x%s\n", cache->name, set, hexstr64s(addr));
    return access_shadow_lines(cache, set, tag, tag_full, tag_aliasing);
  }

  DEBUG(0, "Didn't find line in set %u in cache '%s' base 0x%s\n", set, cache->name, hexstr64s(addr));
  return NULL;
}

/**************************************************************************************/
/* cache_insert: returns a pointer to the data section of the new cache line.
   Sets line_addr to the address of the first block of the new line.  Sets
   repl_line_addr to the address of the first block that was replaced

   DON'T call this unless you are sure that the line is not in the
   cache (call after cache_access returned NULL)
*/

void* cache_insert(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr) {
  if (cache->repl_policy >= REPL_VOID)
    return cache_insert_strategy(cache, proc_id, addr, line_addr, repl_line_addr);

  return cache_insert_replpos(cache, proc_id, addr, line_addr, repl_line_addr, INSERT_REPL_DEFAULT, FALSE);
}
/**************************************************************************************/
/* cache_insert_replpos: returns a pointer to the data section of the new cache
   line.  Sets line_addr to the address of the first block of the new line.
   Sets repl_line_addr to the address of the first block that was replaced

   DON'T call this unless you are sure that the line is not in the
   cache (call after cache_access returned NULL)
*/

void* cache_insert_replpos(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr,
                           Cache_Insert_Repl insert_repl_policy, Flag isPrefetch) {
  Addr tag, tag_full;
  uns repl_index;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);
  Cache_Entry* new_line;

  // Sanity check. Ensure that we do not insert the same line twice
  cache_invalidate(cache, addr, line_addr);

  if (cache->repl_policy >= REPL_VOID)
    return cache_insert_strategy(cache, proc_id, addr, line_addr, repl_line_addr);

  if (cache->repl_policy == REPL_IDEAL) {
    new_line = insert_sure_line(cache, set, tag);
    *repl_line_addr = 0;
    cache->mlp_victim_valid = FALSE;
  } else {
    new_line = find_repl_entry(cache, proc_id, set, &repl_index);
    /* before insert the data into cache, if the cache has shadow entry */
    /* insert that entry to the shadow cache */
    if ((cache->repl_policy == REPL_SHADOW_IDEAL) && new_line->valid)
      shadow_cache_insert(cache, set, new_line->tag, new_line->tag_full, new_line->base);
    mlp_note_victim(cache, new_line);

    /* bug fixed. 4/26/04 if the entry is not valid, repl_line_addr should be set to 0 */
    if (new_line->valid)
      *repl_line_addr = new_line->base;
    else
      *repl_line_addr = 0;

    if (cache->repl_policy == REPL_MLP)
      cache->mlp_set_fills[set]++;
    if (cache->repl_policy == REPL_MLP && mlp_countdown_on() && new_line->valid && new_line->mlp_ticks_left)
      STAT_EVENT(proc_id, MLP_COUNTDOWN_EVICT_PROTECTED);
    DEBUG(0, "Replacing 2.2f(set %u, way %u, tag 0x%s, base 0x%s) in cache '%s' with base 0x%s\n", set, repl_index,
          hexstr64s(new_line->tag), hexstr64s(new_line->base), cache->name, hexstr64s(*line_addr));
  }

  new_line->proc_id = proc_id;
  new_line->valid = TRUE;
  new_line->tag = tag;
  new_line->tag_full = tag_full;
  new_line->base = *line_addr;
  new_line->last_access_time = sim_time;  // FIXME: this fixes valgrind warnings in update_prf_
  new_line->pref = isPrefetch;
  new_line->mlp_boost = g_next_fill_mlp_boost;
  g_next_fill_mlp_boost = 0.0;
  new_line->mlp_mark = g_next_fill_mlp_mark;
  g_next_fill_mlp_mark = MLP_MARK_NONE;
  new_line->mlp_used = FALSE;
  new_line->mlp_stamp = cache->mlp_set_fills ? cache->mlp_set_fills[set] : 0;
  mlp_countdown_start(new_line);

  new_line->pw_start_addr = addr;  // only means anything for uop cache

  switch (insert_repl_policy) {
    case INSERT_REPL_DEFAULT:
      update_repl_policy(cache, new_line, set, repl_index, TRUE);
      DEBUG(0, "(%s, %d) [0x%x, 0x%x]: in insert\n\n", cache->name, cache->repl_policy, cache->num_sets, cache->assoc);
      break;
    case INSERT_REPL_LRU:
      new_line->last_access_time = 123;  // Just choose a small number
      new_line->mlp_stamp = 0;
      break;
    case INSERT_REPL_MRU:
      new_line->last_access_time = sim_time;
      break;
    case INSERT_REPL_MID:     // Insert such that it is Middle(Roughly) of the repl order
    case INSERT_REPL_LOWQTR:  // Insert such that it is Quarter(Roughly) of the repl order
    {
      // first form the lru array
      Counter* access = (Counter*)malloc(sizeof(Counter) * cache->assoc);
      int ii, jj;
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (entry->valid)
          access[ii] = entry->last_access_time;
        else
          access[ii] = 0;
        // Sort
        for (jj = ii - 1; jj >= 0; jj--) {
          if (access[jj + 1] < access[jj]) {  // move data
            Counter temp = access[jj];
            access[jj] = access[jj + 1];
            access[jj + 1] = temp;
          } else {
            break;
          }
        }
      }
      if (insert_repl_policy == INSERT_REPL_MID) {
        new_line->last_access_time = access[cache->assoc / 2];
      } else if (insert_repl_policy == INSERT_REPL_LOWQTR) {
        new_line->last_access_time = access[cache->assoc / 4];
      }
      if (new_line->last_access_time == 0)
        new_line->last_access_time = sim_time;
      free(access);
    } break;
    default:
      ASSERT(0, FALSE);  // should never come here
  }
  if (cache->repl_policy == REPL_MLP && mlp_countdown_on())
    mlp_countdown_tick(cache, proc_id, set, repl_index);
  if (cache->repl_policy == REPL_IDEAL_STORAGE) {
    new_line->last_access_time = cache->assoc;
    /* debug */
    /* insert into the entry also */
    {
      uns lru_ind = 0;
      Counter lru_time = MAX_CTR;
      Cache_Entry* main_line;
      uns ii;

      /* first cache access */
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* line = &cache->entries[set][ii];

        if (line->tag == tag && line->valid) {
          /* update replacement state if necessary */
          ASSERT(0, line->data);
          line->last_access_time = sim_time;
          return new_line->data;
        }
      }
      /* looking for lru */
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid) {
          lru_ind = ii;
          break;
        }
        if (entry->last_access_time < lru_time) {
          lru_ind = ii;
          lru_time = cache->entries[set][ii].last_access_time;
        }
      }
      main_line = &cache->entries[set][lru_ind];
      main_line->valid = TRUE;
      main_line->tag = tag;
      main_line->tag_full = tag_full;
      main_line->base = *line_addr;
      main_line->last_access_time = sim_time;
    }
  }
  return new_line->data;
}

/**************************************************************************************/
/* invalidate_line: Invalidates based on the address.  */

void cache_invalidate(Cache* cache, Addr addr, Addr* line_addr) {
  Addr tag, tag_full;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);
  uns ii;

  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->entries[set][ii];
    if (line->tag == tag && line->valid) {
      line->tag = 0;
      line->valid = FALSE;
      line->base = 0;
    }
  }

  if (cache->repl_policy == REPL_IDEAL)
    invalidate_unsure_line(cache, set, tag);
}

/**
 * @brief Return a pointer to the lru item in the cache set
 *
 * @param cache
 * @param proc_id
 * @param addr
 * @param repl_line_addr
 * @param valid
 * @return void*
 */
void* get_next_repl_line(Cache* cache, uns8 proc_id, Addr addr, Addr* repl_line_addr, Flag* valid) {
  Addr line_tag, line_tag_full, line_addr;
  uns repl_index;
  uns set_index = cache_index(cache, addr, &line_tag, &line_tag_full, &line_addr);
  Cache_Entry* new_line = find_repl_entry(cache, proc_id, set_index, &repl_index);

  *repl_line_addr = new_line->base;
  *valid = new_line->valid;
  return new_line->data;
}

/**
 * @brief Returns the cache lib entry that will be the next to be
   replaced. This call should not change any of the state information.
 *
 * @param cache
 * @param proc_id
 * @param set
 * @param way
 * @return Cache_Entry*
 */
Cache_Entry* find_repl_entry(Cache* cache, uns8 proc_id, uns set, uns* way) {
  int ii;

  if (cache->repl_policy >= REPL_VOID)
    return cache_evict_strategy(cache, proc_id, set, way);

  switch (cache->repl_policy) {
    case REPL_RESTEER:
    case REPL_SHADOW_IDEAL:
    case REPL_TRUE_LRU: {
      uns lru_ind = 0;
      Counter lru_time = MAX_CTR;
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid) {
          lru_ind = ii;
          break;
        }
        if (entry->last_access_time < lru_time) {
          lru_ind = ii;
          lru_time = cache->entries[set][ii].last_access_time;
        }
      }
      *way = lru_ind;
      return &cache->entries[set][lru_ind];
    } break;
    case REPL_RANDOM:
    case REPL_NOT_MRU:
    case REPL_ROUND_ROBIN:
    case REPL_LOW_PREF: {
      uns repl_index = cache->repl_ctrs[set];
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid)
          repl_index = ii;
      }
      *way = repl_index;
      return &cache->entries[set][repl_index];
    } break;
    case REPL_IDEAL:
      printf("ERROR: Can't determine entry to be replaced when using ideal replacement\n");
      ASSERT(0, FALSE);
      break;

    case REPL_IDEAL_STORAGE: {
      printf("ERROR: Check if ideal storage replacement works\n");
      ASSERT(0, FALSE);
      Cache_Entry* new_line = &(cache->shadow_entries[set][cache->queue_end[set]]);
      cache->queue_end[set] = (cache->queue_end[set] + 1) % ideal_num_entries;
      return new_line;
    } break;
    case REPL_PARTITION: {
      /**
       * @brief If cache is partitioned, the target partition is given by
       * num_ways_allocated_core[proc_id]. Here (where the partition is
       * enforced) the actual occption of everycore is calculated first, and
       * it's very likely the victim comes from the very over-occupied partition
       * instead of request's own partition.
       */
      uns8 way_proc_id;
      uns lru_ind = 0;
      uns total_assigned_ways = 0;

      for (way_proc_id = 0; way_proc_id < NUM_CORES; way_proc_id++) {
        cache->num_ways_occupied_core[way_proc_id] = 0;
        cache->lru_time_core[way_proc_id] = MAX_CTR;
        ASSERT(way_proc_id, cache->num_ways_allocted_core[way_proc_id]);
        total_assigned_ways += cache->num_ways_allocted_core[way_proc_id];
      }

      ASSERT(proc_id, total_assigned_ways <= cache->assoc);
      if (total_assigned_ways != cache->assoc) {
        printf("WARN: total allocated cache way smaller than all ways");
      }

      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid) {
          lru_ind = ii;
          *way = lru_ind;
          return &cache->entries[set][lru_ind];
        }
        cache->num_ways_occupied_core[entry->proc_id]++;
        if (entry->last_access_time < cache->lru_time_core[entry->proc_id]) {
          cache->lru_index_core[entry->proc_id] = ii;
          cache->lru_time_core[entry->proc_id] = entry->last_access_time;
        }
      }

      // find the core that overoccupies its partition the most
      int max_extra_occ = 0;
      int repl_proc_id = -1;
      for (way_proc_id = 0; way_proc_id < NUM_CORES; way_proc_id++) {
        if (cache->num_ways_allocted_core[way_proc_id] < cache->num_ways_occupied_core[way_proc_id]) {
          int extra_occ = cache->num_ways_occupied_core[way_proc_id] - cache->num_ways_allocted_core[way_proc_id];
          if (extra_occ > max_extra_occ) {
            max_extra_occ = extra_occ;
            repl_proc_id = way_proc_id;
          }
        }
      }

      int proc_id_extra_occ = cache->num_ways_occupied_core[proc_id] - cache->num_ways_allocted_core[proc_id];

      if (cache->num_ways_allocted_core[proc_id] > cache->num_ways_occupied_core[proc_id] ||
          max_extra_occ > proc_id_extra_occ + 1 ||
          ((max_extra_occ > proc_id_extra_occ) && ((proc_id + set) % NUM_CORES > (repl_proc_id + set) % NUM_CORES))) {
        /* the complicated condition above ensures unbiased
           distribution of over-occupancy in case a workload does
           not occupy its allocated way partition */
        ASSERT(0, repl_proc_id >= 0);
        lru_ind = cache->lru_index_core[repl_proc_id];
      } else {
        lru_ind = cache->lru_index_core[proc_id];
      }
      *way = lru_ind;
      return &cache->entries[set][lru_ind];
    }

    case REPL_MLP: {
      if (mlp_countdown_on()) {
        /* Evict the least recently used unprotected line, or the least recently used line if
           every line is protected. With every boost 0 nothing is protected: plain LRU.
           mlp_boost_countdown_tiered instead evicts the lowest mlp_ticks_left outright (0 still
           first), ties to the least recently used within that tier, so a line one tick from
           expiry goes before one freshly reset to 3 rather than being grouped with it. */
        uns best_ind = 0;
        for (ii = 0; ii < cache->assoc; ii++) {
          Cache_Entry* entry = &cache->entries[set][ii];
          if (!entry->valid) {
            *way = ii;
            return entry;
          }
          Cache_Entry* best = &cache->entries[set][best_ind];
          if (MLP_BOOST_COUNTDOWN_TIERED) {
            if (ii == 0 || entry->mlp_ticks_left < best->mlp_ticks_left ||
                (entry->mlp_ticks_left == best->mlp_ticks_left && entry->last_access_time < best->last_access_time))
              best_ind = ii;
            continue;
          }
          Flag prot = entry->mlp_ticks_left > 0, best_prot = best->mlp_ticks_left > 0;
          if (ii == 0 || (best_prot && !prot) ||
              (prot == best_prot && entry->last_access_time < best->last_access_time))
            best_ind = ii;
        }
        *way = best_ind;
        return &cache->entries[set][best_ind];
      }
      if (MLP_BOOST_IN_FILLS) {
        /* Evict argmin(mlp_stamp + mlp_boost), ties to the least recently used. mlp_stamp is the
           set's fill count at the line's last recency update, so a marked line ranks as if touched
           mlp_boost fills later than it was: its protection lapses mlp_boost fills after its last
           touch unless a hit renews it. With every boost 0 this is plain LRU. */
        uns best_ind = 0;
        double best_val = 0.0;
        for (ii = 0; ii < cache->assoc; ii++) {
          Cache_Entry* entry = &cache->entries[set][ii];
          if (!entry->valid) {
            *way = ii;
            return entry;
          }
          double val = (double)entry->mlp_stamp + (double)entry->mlp_boost;
          if (ii == 0 || val < best_val ||
              (val == best_val && entry->last_access_time < cache->entries[set][best_ind].last_access_time)) {
            best_val = val;
            best_ind = ii;
          }
        }
        *way = best_ind;
        return &cache->entries[set][best_ind];
      }
      /* Evict argmin(Recency + mlp_boost). Recency is the LRU stack position (0 = LRU), computed
         as a rank -- how many resident lines are older -- rather than compared via raw
         last_access_time, since mlp_boost (in stack positions) has to be added to something on
         the same scale. An invalid way wins outright, same as every other case. */
      uns best_ind = 0;
      double best_val = 0.0;
      Flag have_best = FALSE;
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid) {
          *way = ii;
          return entry;
        }
        uns rank = 0;
        int jj;
        for (jj = 0; jj < cache->assoc; jj++) {
          Cache_Entry* other = &cache->entries[set][jj];
          if (other->valid && other->last_access_time < entry->last_access_time)
            rank++;
        }
        double val = (double)rank + (double)entry->mlp_boost;
        if (!have_best || val < best_val) {
          best_val = val;
          best_ind = ii;
          have_best = TRUE;
        }
      }
      *way = best_ind;
      return &cache->entries[set][best_ind];
    } break;

    default:
      ASSERT(0, FALSE);
  }
}

/**************************************************************************************/
/* get_next_valid_repl_line: Returns the valid cache lib entry that will be replaced the
   soonest. This call should not change any of the state information. */
void* get_next_valid_repl_line(Cache* cache, uns8 proc_id, Addr addr) {
  int ii;
  Addr line_tag, line_tag_full, line_addr;
  Cache_Entry* entry = NULL;
  uns set = cache_index(cache, addr, &line_tag, &line_tag_full, &line_addr);

  switch (cache->repl_policy) {
    case REPL_RESTEER:
    case REPL_SHADOW_IDEAL:
    case REPL_TRUE_LRU: {
      uns lru_ind = 0;
      Counter lru_time = MAX_CTR;
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid) {
          continue;
        }
        if (entry->last_access_time < lru_time) {
          lru_ind = ii;
          lru_time = cache->entries[set][ii].last_access_time;
        }
      }
      if (lru_ind >= 0)
        entry = &cache->entries[set][lru_ind];
    } break;
    default:
      ASSERT(proc_id, FALSE);  // not implemented
  }
  if (entry) {
    return entry->data;
  }
  return NULL;
}

/**************************************************************************************/
/* update_repl_policy: */

static inline void update_repl_policy(Cache* cache, Cache_Entry* cur_entry, uns set, uns way, Flag repl) {
  switch (cache->repl_policy) {
    case REPL_IDEAL_STORAGE:
    case REPL_SHADOW_IDEAL:
    case REPL_TRUE_LRU:
    case REPL_PARTITION:
    /* REPL_MLP derives its Recency term from this same LRU stack -- without this case,
       last_access_time would never advance on a hit and every line would rank equally,
       leaving mlp_boost to decide the victim alone. */
    case REPL_MLP:
      cur_entry->last_access_time = sim_time;
      if (cache->mlp_set_fills)  // REPL_MLP only: the policies above share this case
        cur_entry->mlp_stamp = cache->mlp_set_fills[set];
      if (cache->repl_policy == REPL_MLP && mlp_countdown_on() && cur_entry->mlp_boost > 0.0 &&
          (!MLP_BOOST_COUNTDOWN_NO_REVIVE || cur_entry->mlp_ticks_left > 0))
        mlp_countdown_start(cur_entry); /* a fill or hit restarts a marked line's countdown, unless
                                            mlp_boost_countdown_no_revive and it already lapsed */
      break;
    case REPL_RANDOM: {
      char* old_rand_state = (char*)setstate(rand_repl_state);
      cache->repl_ctrs[set] = rand() % cache->assoc;
      setstate(old_rand_state);
    } break;
    case REPL_NOT_MRU:
      if (way == cache->repl_ctrs[set])
        cache->repl_ctrs[set] = CIRC_INC2(cache->repl_ctrs[set], cache->assoc);
      break;
    case REPL_ROUND_ROBIN:
      cache->repl_ctrs[set] = CIRC_INC2(cache->repl_ctrs[set], cache->assoc);
      break;
    case REPL_IDEAL:
      /* no need to do anything here (nothing changes when we hit on a sure
       * line) */
      /* all unsure hits are handled elsewhere */
      break;
    case REPL_LOW_PREF:
      /* low priority to prefetcher data */
      {
        int lru_ind = -1;
        Counter lru_time = MAX_CTR;
        int ii;
        // cache->repl_ctrs[set] = ....
        for (ii = 0; ii < cache->assoc; ii++) {
          Cache_Entry* entry = &cache->entries[set][ii];
          if (!entry->valid) {
            lru_ind = ii;
            break;
          }
          // compare between prefetcher
          if ((entry->last_access_time < lru_time) && cache->entries[set][ii].pref) {
            lru_ind = ii;
            lru_time = cache->entries[set][ii].last_access_time;
          }
        }
        if (lru_ind == -1) {
          for (ii = 0; ii < cache->assoc; ii++) {
            Cache_Entry* entry = &cache->entries[set][ii];
            if (entry->last_access_time < lru_time) {
              lru_ind = ii;
              lru_time = cache->entries[set][ii].last_access_time;
            }
          }
        }
        cache->repl_ctrs[set] = lru_ind;
      }
      break;
    case REPL_RESTEER:  // No update on access
      break;
    default:
      ASSERT(0, FALSE);
  }
}

/**************************************************************************************/
/* update_repl_resteer_policy: Update repl policy for REPL_RESTEER.                   */
/*                             LRU counter is updated before access, when a resteer   */
/*                             is resolved or fetch barrier identified                */
void update_repl_resteer_policy(Cache* cache, Addr addr) {
  ASSERT(0, cache->repl_policy == REPL_RESTEER);
  int ii;
  Addr tag, tag_full;
  Addr line_addr;
  uns set = cache_index(cache, addr, &tag, &tag_full, &line_addr);
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->entries[set][ii];
    if (line->valid && line->tag == tag) {
      ASSERT(0, line->data);
      DEBUG(0, "updating access time REPL_RESTEER '%s' at (set %u, way %u, base 0x%s)\n", cache->name, set, ii,
            hexstr64s(line->base));
      line->last_access_time = sim_time;
    }
  }
}

/**************************************************************************************/
/* cache_get_invalid_line_count: Return the number of invalid lines in the set this   */
/*                               address maps to.                                     */

uns cache_get_invalid_line_count(Cache* cache, Addr addr) {
  Addr tag, tag_full;
  Addr line_addr;
  uns set = cache_index(cache, addr, &tag, &tag_full, &line_addr);
  uns invalid_entries = 0;
  for (int ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];
    if (!entry->valid) {  // Check for invalid entries
      invalid_entries++;
    }
  }
  return invalid_entries;
}

/**************************************************************************************/
/* access_unsure_lines: */

static inline void* access_unsure_lines(Cache* cache, uns set, Addr tag, Flag update_repl) {
  List* list = &cache->unsure_lists[set];
  Cache_Entry* temp;
  int ii;

  for (temp = (Cache_Entry*)list_start_head_traversal(list); temp; temp = (Cache_Entry*)list_next_element(list)) {
    ASSERT(0, temp->valid);
    if (temp->tag == tag) {
      for (ii = 0; ii < cache->assoc; ii++) {
        if (!cache->entries[set][ii].valid) {
          void* data = cache->entries[set][ii].data;
          memcpy(&cache->entries[set][ii], temp, sizeof(Cache_Entry));
          temp->data = data;
          ASSERT(0, dl_list_remove_current(list) == temp);
          ASSERT(0, ++cache->repl_ctrs[set] <= cache->assoc); /* repl ctr holds the sure count */
          if (cache->repl_ctrs[set] == cache->assoc) {
            for (temp = (Cache_Entry*)list_start_head_traversal(list); temp;
                 temp = (Cache_Entry*)list_next_element(list))
              free(temp->data);
            clear_list(&cache->unsure_lists[set]);
          }
          return cache->entries[set][ii].data;
        }
      }
      ASSERT(0, FALSE);
    }
  }
  return NULL;
}

/**************************************************************************************/
/* insert_sure_line: */

static inline Cache_Entry* insert_sure_line(Cache* cache, uns set, Addr tag) {
  List* list = &cache->unsure_lists[set];
  int ii;
  if (list_get_head(list) || cache->repl_ctrs[set] == cache->assoc) {
    /* if there is an unsure list already, or if we have all sure entries... */
    int count = 0;
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (entry->valid) {
        Cache_Entry* temp = (Cache_Entry*)dl_list_add_tail(list);
        memcpy(temp, entry, sizeof(Cache_Entry));
        temp->data = malloc(cache->data_size);
        memcpy(temp->data, entry->data, cache->data_size);
        entry->valid = FALSE;
        count++;
      }
    }
    ASSERT(0, count == cache->repl_ctrs[set]);
    cache->repl_ctrs[set] = 1;
    return &cache->entries[set][0];
  } else {
    /* there should be an invalid entry to use */
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (!entry->valid) {
        ASSERT(0, ++cache->repl_ctrs[set] <= cache->assoc);
        return entry;
      }
    }
    ASSERT(0, FALSE);
  }
}

/**************************************************************************************/
/* invalidate_unsure_line: */

static inline void invalidate_unsure_line(Cache* cache, uns set, Addr tag) {
  List* list = &cache->unsure_lists[set];
  Cache_Entry* temp;
  for (temp = (Cache_Entry*)list_start_head_traversal(list); temp; temp = (Cache_Entry*)list_next_element(list)) {
    ASSERT(0, temp->valid);
    if (temp->tag == tag) {
      free(temp->data);
      dl_list_remove_current(list);
      return;
    }
  }
}

void* access_shadow_lines(Cache* cache, uns set, Addr tag, Addr tag_full, Flag* tag_aliasing) {
  uns ii;
  uns lru_ind = 0;
  Counter lru_time = MAX_CTR;

  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->shadow_entries[set][ii];

    if (line->tag == tag && line->valid) {
      *tag_aliasing = line->tag_full != tag_full;
      int jj;
      /* found the entry in the shadow cache */
      ASSERT(0, line->data);
      DEBUG(0, "Found line in shadow cache '%s' at (set %u, way %u, base 0x%s)\n", cache->name, set, ii,
            hexstr64s(line->base));

      /* search for the oldest entry */

      for (jj = 0; jj < cache->assoc; jj++) {
        Cache_Entry* entry = &cache->entries[set][jj];
        if (!entry->valid) {
          lru_ind = jj;
          break;
        }
        if (entry->last_access_time < lru_time) {
          lru_ind = jj;
          lru_time = cache->entries[set][jj].last_access_time;
        }
      }

      if (lru_time < line->last_access_time) {
        Cache_Entry tmp_line;
        DEBUG(0,
              "shadow cache line will be swaped:\ncache->addr:0x%s "
              "cache->lru_time:%lld  shadow_tag:0x%s shadow_insert:%lld \n",
              hexstr64s((cache->entries[set][lru_ind]).tag), (uns64)(cache->entries[set][lru_ind]).last_access_time,
              hexstr64s(line->tag), (uns64)line->last_access_time);

        /* shadow entry can be inserted to main cache  */

        tmp_line = (cache->entries[set][lru_ind]);
        (cache->entries[set][lru_ind]) = *line;
        *line = tmp_line;
        line->last_access_time = (cache->entries[set][lru_ind]).last_access_time;
        (cache->entries[set][lru_ind]).last_access_time = sim_time;
        DEBUG(0,
              "shadow cache line is swaped\n cache->addr:0x%s "
              "cache->lru_time:%lld  shadow_tag:0x%s shadow_insert:%lld \n",
              hexstr64s((cache->entries[set][lru_ind]).tag), (uns64)(cache->entries[set][lru_ind]).last_access_time,
              hexstr64s(line->tag), (uns64)line->last_access_time);
        /* insert counter later */
        return (line->data);
      } else {
        /* make invalidate for that cache entry */
        DEBUG(0,
              "shadow cache can't find the replacment target: cache_tag:0x%s "
              "lru_time:%lld, insert_time:%lld\n",
              hexstr64s(cache->entries[set][lru_ind].tag), (uns64)lru_time, (uns64)line->last_access_time);
        line->valid = FALSE; /* so we don't have 2 copies for the same data */
      }
    }
  }

  DEBUG(0, "Didn't find line in set %u in shadow cache '%s' \n", set, cache->name);
  return NULL;
}

void* shadow_cache_insert(Cache* cache, uns set, Addr tag, Addr tag_full, Addr base) {
  int ii;
  Cache_Entry* new_line;

  /* within the shadow cache entry, we need to use the true replacement policy
   */

  uns lru_ind = 0;
  Counter lru_time = MAX_CTR;
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->shadow_entries[set][ii];
    if (!entry->valid) {
      lru_ind = ii;
      break;
    }
    if (entry->last_access_time < lru_time) {
      lru_ind = ii;
      lru_time = cache->shadow_entries[set][ii].last_access_time;
    }
  }

  new_line = &(cache->shadow_entries[set][lru_ind]);
  new_line->valid = TRUE;
  new_line->tag = tag;
  new_line->tag_full = tag_full;
  new_line->base = base;
  new_line->last_access_time = sim_time;
  DEBUG(0,
        "Insert Shadow cache (set %u, way %u, tag 0x%s, base 0x%s) "
        "last_access_time:%lld : sim_time:%lld\n",
        set, lru_ind, hexstr64s(tag), hexstr64s(base), (uns64)new_line->last_access_time, (uns64)sim_time);
  return new_line;
}

#define QUEUE_IND(num) (((num) + cache->queue_end[set]) % ideal_num_entries)

void* access_ideal_storage(Cache* cache, uns set, Addr tag, Addr tag_full, Addr addr, Flag* tag_aliasing) {
  uns ii;
  uns valid_start = 0;
  Cache_Entry* new_line;
  int main_entry_found = FALSE;

  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->entries[set][ii];
    if (line->tag == tag && line->valid) {
      line->last_access_time = sim_time;
      main_entry_found = TRUE;
    }
  }

  for (ii = 0; ii < ideal_num_entries; ii++) {
    Cache_Entry* line = &cache->shadow_entries[set][ii];
    if (line->tag == tag && line->valid) {
      *tag_aliasing = line->tag_full != tag_full;
      uns jj;
      ASSERT(0, line->data);
      if (ii == (cache->queue_end[set] + ideal_num_entries - 1) % ideal_num_entries)
        return line->data;
      for (jj = ((ii - cache->queue_end[set]) % ideal_num_entries); jj < ideal_num_entries; jj++) {
        Cache_Entry* cal_line = &cache->shadow_entries[set][QUEUE_IND(jj)];
        if (cal_line->valid) {
          cal_line->last_access_time--;
          DEBUG(0,
                "counter is decreasing. set:%d, queue_end:%u jj:%u ind:%u  "
                "counter:%lld, addr:0x%s\n",
                set, cache->queue_end[set], jj, QUEUE_IND(jj), cal_line->last_access_time, hexstr64s(cal_line->base));
          if ((cal_line->valid) && (cal_line->last_access_time == 0))
            valid_start = jj;
        }
      }
      for (jj = 0; jj <= valid_start; jj++) {
        Cache_Entry* cal_line = &cache->shadow_entries[set][QUEUE_IND(jj)];
        cal_line->valid = 0;
        DEBUG(0,
              "Last counter:%u is 0. invalidated ideal storage set:%d, jj:%u "
              "ind:%u counter:%lld, addr:0x%s\n",
              valid_start, set, jj, QUEUE_IND(jj), cal_line->last_access_time, hexstr64s(cal_line->base));
      }

      DEBUG(0, "data is found in ideal storage set%u \n", set);

      new_line = &(cache->shadow_entries[set][cache->queue_end[set]]);
      new_line->valid = TRUE;
      new_line->tag = tag;
      new_line->tag_full = tag_full;
      new_line->base = addr;
      new_line->last_access_time = cache->assoc;
      cache->queue_end[set] = (cache->queue_end[set] + 1) % ideal_num_entries;
      return (line->data);
    }
  }

  DEBUG(0, "Didn't find line in set %u in ideal_storage cache '%s' \n", set, cache->name);
  if (main_entry_found) {
    DEBUG(0, "Only_main set:%u addr:0x%s cycle_time:%lld\n", set, hexstr64s(addr), sim_time);
  }
  return NULL;
}

/**************************************************************************************/
/* get_cache_line_addr: */

Addr get_cache_line_addr(Cache* cache, Addr addr) {
  Addr tag, tag_full;
  Addr line_addr;
  cache_index(cache, addr, &tag, &tag_full, &line_addr);

  return line_addr;
}

/**************************************************************************************/
/* cache_insert_lru: returns a pointer to the data section of the new cache
   line.  Sets line_addr to the address of the first block of the new line.
   Sets repl_line_addr to the address of the first block that was replaced

   DON'T call this unless you are sure that the line is not in the
   cache (call after cache_access returned NULL)

   This function inserts the entry as LRU instead of MRU
*/

void* cache_insert_lru(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr) {
  Addr tag, tag_full;
  uns repl_index;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);
  Cache_Entry* new_line;

  if (cache->repl_policy >= REPL_VOID)
    cache_insert_strategy(cache, proc_id, addr, line_addr, repl_line_addr);

  if (cache->repl_policy == REPL_IDEAL) {
    new_line = insert_sure_line(cache, set, tag);
    *repl_line_addr = 0;
  } else {
    new_line = find_repl_entry(cache, proc_id, set, &repl_index);
    /* before insert the data into cache, if the cache has shadow entry */
    /* insert that entry to the shadow cache */
    if ((cache->repl_policy == REPL_SHADOW_IDEAL) && new_line->valid)
      shadow_cache_insert(cache, set, new_line->tag, new_line->tag_full, new_line->base);
    if (new_line->valid)  // bug fixed. 4/26/04 if the entry is not valid,
                          // repl_line_addr should be set to 0
      *repl_line_addr = new_line->base;
    else
      *repl_line_addr = 0;
    DEBUG(0,
          "Replacing (set %u, way %u, tag 0x%s, base 0x%s) in cache '%s' with "
          "base 0x%s\n",
          set, repl_index, hexstr64s(new_line->tag), hexstr64s(new_line->base), cache->name, hexstr64s(*line_addr));
  }

  new_line->proc_id = proc_id;
  new_line->valid = TRUE;
  new_line->tag = tag;
  new_line->tag_full = tag_full;
  new_line->base = *line_addr;
  update_repl_policy(cache, new_line, set, repl_index, TRUE);
  if (cache->repl_policy == REPL_TRUE_LRU)
    new_line->last_access_time = 137;

  if (cache->repl_policy == REPL_IDEAL_STORAGE) {
    new_line->last_access_time = cache->assoc;
    /* debug */
    /* insert into the entry also */
    {
      uns lru_ind = 0;
      Counter lru_time = MAX_CTR;
      Cache_Entry* main_line;
      uns ii;

      /* first cache access */
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* line = &cache->entries[set][ii];

        if (line->tag == tag && line->valid) {
          /* update replacement state if necessary */
          ASSERT(0, line->data);
          line->last_access_time = sim_time;
          return new_line->data;
        }
      }
      /* looking for lru */
      for (ii = 0; ii < cache->assoc; ii++) {
        Cache_Entry* entry = &cache->entries[set][ii];
        if (!entry->valid) {
          lru_ind = ii;
          break;
        }
        if (entry->last_access_time < lru_time) {
          lru_ind = ii;
          lru_time = cache->entries[set][ii].last_access_time;
        }
      }
      main_line = &cache->entries[set][lru_ind];
      main_line->valid = TRUE;
      main_line->tag = tag;
      main_line->tag_full = tag_full;
      main_line->base = *line_addr;
      main_line->last_access_time = sim_time;
    }
  }
  return new_line->data;
}

/**************************************************************************************/
/* reset cache: A function that initializes all lines to invalid state*/

void reset_cache(Cache* cache) {
  uns ii, jj;

  for (ii = 0; ii < cache->num_sets; ii++) {
    for (jj = 0; jj < cache->assoc; jj++) {
      cache->entries[ii][jj].valid = FALSE;
    }
  }

  // Clear any adaptive-index training state; keep the current index function.
  cache_index_state_reset(cache);
}

/**************************************************************************************/
/* cache_find_pos_in_lru_stack: returns the position of a cache line */
/* return -1 : cache miss  */
/*         0 : MRU         */
/*        ...........     */
/*   assoc-1 : LRU         */

int cache_find_pos_in_lru_stack(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr) {
  Addr tag, tag_full;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);
  uns ii;
  int position;
  Cache_Entry* hit_line = NULL;
  Flag hit = FALSE;

  for (ii = 0; ii < cache->assoc; ii++) {
    hit_line = &cache->entries[set][ii];

    if (hit_line->valid && hit_line->tag == tag) {
      hit = TRUE;
      break;
    }
  }

  if (!hit)
    return -1;

  ASSERT(0, hit_line);
  ASSERT(0, hit_line->proc_id == proc_id);
  position = 0;
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->entries[set][ii];
    if (hit_line->proc_id == line->proc_id && line->last_access_time > hit_line->last_access_time) {
      position++;
    }
  }
  return position;
}

void set_partition_allocate(Cache* cache, uns8 proc_id, uns num_ways) {
  // ASSERT(proc_id, cache->repl_policy == REPL_PARTITION);
  ASSERT(proc_id, L1_PART_ON);
  ASSERT(proc_id, cache->num_ways_allocted_core);
  cache->num_ways_allocted_core[proc_id] = num_ways;
}

uns get_partition_allocated(Cache* cache, uns8 proc_id) {
  ASSERT(proc_id, cache->repl_policy == REPL_PARTITION);
  ASSERT(proc_id, cache->num_ways_allocted_core);
  return cache->num_ways_allocted_core[proc_id];
}

/***************************************************************************************
 * Driven Table:
 *  Cache Replacement Policy Strategy
 *    Action:
 *      Change cache line
 *    Update:
 *      Only update the value of reference and prediction
 *
 * Design:
 * = Hit                        {-> update_hit()}
 * ----- do aging               [reference value]
 * ----- do promotion           [reference value]
 * ----- do prediction revise   [prediction value]
 * = Miss
 * === Eviction                 {-> update_evict()}
 * ----- do aging               [reference value]
 * ----- do prediction revise   [prediction value]
 * === Insertion                {-> update_insert()}
 * ----- do aging               [reference value]
 * ----- do promotion           [reference value]
 * ----- do prediction revise   [prediction value]
 ***************************************************************************************/

/**************************************************************************************/
/* Common Func */
static inline int cache_get_policy_index(Repl_Policy repl_policy) {
  int ii;
  int policy = -1;

  for (ii = 0; ii < NUM_REPL; ii++) {
    if (repl_policy_func_table[ii].repl_policy_type == repl_policy) {
      policy = ii;
      break;
    }
    if (repl_policy_func_table[ii].repl_policy_type == REPL_VOID)
      break;
  }

  return policy;
}

static inline uns cache_repl_signiture(Cache_Entry* cache_entry, Cache_Repl_Signiture sign_type) {
  uns sign = 0;
  switch (sign_type) {
    case CACHE_REPL_SIGH_PC:
      break;

    case CACHE_REPL_SIGH_MEM:
      sign = cache_entry->base;
      break;

    default:
      break;
  }

  return sign;
}

const static int CACHE_EVENT_HIT = 1;
const static int CACHE_EVENT_INSERT = 2;
const static int CACHE_EVENT_EVICT = 3;
static inline void cache_debug_print_set(Cache* cache, uns set, uns way, int event) {
  int ii;

  if (!CACHE_DEBUG_ENABLE)
    return;

  DEBUG(0, "Cache: (%s, %d) [0x%x, 0x%x]\n", cache->name, cache->repl_policy, cache->num_sets, cache->assoc);

  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->entries[set][ii];
    if (line == NULL)
      continue;
    DEBUG(0, "(%d <- 0x%x) [0x%x, 0x%x] : {0x%llx, 0x%x, 0x%llx, 0x%x, 0x%x}\n", event, way, set, ii, line->tag,
          line->valid, line->last_access_time, line->reference_val, line->outcome);
  }

  DEBUG(0, "\n");
}

/**************************************************************************************/
/* External Behavior of Calling Strategy (Policy) */
/*
  init_cache_strategy
  -- call sub internal func: action_init
  -- called by external func: init_cache
*/
void init_cache_strategy(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                         Repl_Policy repl_policy) {
  int policy = cache_get_policy_index(repl_policy);
  if (policy == -1)
    return;

  repl_policy_func_table[policy].action_init(cache, name, cache_size, assoc, line_size, data_size, repl_policy);
}

/*
  cache_insert_strategy
  -- call sub internal func: update_evict -> action_repl -> update_insert
  -- called by external func: cache_insert, cache_insert_replpos, cache_insert_lru
*/
void* cache_insert_strategy(Cache* cache, uns8 proc_id, Addr addr, Addr* line_addr, Addr* repl_line_addr) {
  Addr tag, tag_full;
  uns repl_index;
  Cache_Entry* new_line;
  int policy;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);

  // Get the selected strategy (policy)
  policy = cache_get_policy_index(cache->repl_policy);
  if (policy == -1)
    return NULL;

  DEBUG(0, "%s, %d: Insert Strategy\n", cache->name, cache->repl_policy);

  // update_evict -> action_repl -> update_insert
  // External func also directly call it
  new_line = repl_policy_func_table[policy].update_evict(cache, proc_id, set, &repl_index, NULL, FALSE);
  mlp_note_victim(cache, new_line);

  if (new_line->valid)
    *repl_line_addr = new_line->base;
  else
    *repl_line_addr = 0;
  repl_policy_func_table[policy].action_repl(cache, new_line, proc_id, tag, tag_full, line_addr, repl_line_addr);
  /* mlp_rrip_boost: the boost memory.c staged for this fill (0 if none) */
  new_line->mlp_boost = g_next_fill_mlp_boost;
  g_next_fill_mlp_boost = 0.0;
  new_line->mlp_mark = g_next_fill_mlp_mark;
  g_next_fill_mlp_mark = MLP_MARK_NONE;
  new_line->mlp_used = FALSE;
  repl_policy_func_table[policy].update_insert(cache, proc_id, set, repl_index, NULL);
  new_line->mlp_rrip_credit = MLP_RRIP_BOOST == MLP_RRIP_BOOST_GAMMA ? (uns)new_line->mlp_boost : 0;

  return new_line->data;
}

/*
  cache_access_strategy
  -- call sub internal func: update_hit
  -- called by external func: cache_access
*/
void* cache_access_strategy(Cache* cache, Addr addr, Addr* line_addr, Flag* tag_aliasing, Flag update_repl) {
  Addr tag, tag_full;
  uns set = cache_index(cache, addr, &tag, &tag_full, line_addr);
  uns ii;
  int policy;

  // Get the selected strategy (policy)
  policy = cache_get_policy_index(cache->repl_policy);
  if (policy == -1)
    return NULL;

  DEBUG(0, "%s, %d: Access Strategy\n", cache->name, cache->repl_policy);

  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* line = &cache->entries[set][ii];

    if (line->valid && line->tag == tag) {
      if (update_repl) {
        repl_policy_func_table[policy].update_hit(cache, set, ii, NULL);
        /* mlp_rrip_boost 2: a hit renews a marked line's credit along with its RRPV */
        if (MLP_RRIP_BOOST == MLP_RRIP_BOOST_GAMMA)
          line->mlp_rrip_credit = (uns)line->mlp_boost;
      }

      *tag_aliasing = line->tag_full != tag_full;
      return line->data;
    }
  }

  return NULL;
}

/*
  cache_evict_strategy
  -- call sub internal func: update_evict
  -- called by external func: find_repl_entry
*/
Cache_Entry* cache_evict_strategy(Cache* cache, uns8 proc_id, uns set, uns* way) {
  Cache_Entry* new_line;
  int policy;

  policy = cache_get_policy_index(cache->repl_policy);
  if (policy == -1)
    return NULL;

  DEBUG(0, "%s, %d: Evict Strategy\n", cache->name, cache->repl_policy);

  /* True flag indicates it is called in the external eviction */
  new_line = repl_policy_func_table[policy].update_evict(cache, proc_id, set, way, NULL, TRUE);
  return new_line;
}

/**************************************************************************************/
/* General */

void general_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                         Repl_Policy repl_policy);
void general_action_repl(Cache* cache, Cache_Entry* new_line, uns8 proc_id, Addr tag, Addr tag_full, Addr* line_addr,
                         Addr* repl_line_addr);

/* The common setup (fields, entries, index hash) is done by init_cache_impl before it calls init_cache_strategy */
void general_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                         Repl_Policy repl_policy) {
}

void general_action_repl(Cache* cache, Cache_Entry* new_line, uns8 proc_id, Addr tag, Addr tag_full, Addr* line_addr,
                         Addr* repl_line_addr) {
  new_line->proc_id = proc_id;
  new_line->valid = TRUE;
  new_line->tag = tag;
  new_line->tag_full = tag_full;
  new_line->base = *line_addr;
}

/**************************************************************************************/
/* LRU */
void lru_update_hit(Cache* cache, uns set, uns way, void* arg);
void lru_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* lru_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

void lru_update_hit(Cache* cache, uns set, uns way, void* arg) {
  int ii;
  uns8 ref_orig = cache->entries[set][way].reference_val;

  // promotion
  cache->entries[set][way].reference_val = 0;

  // aging
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];

    if (ii == way)
      continue;

    if (!entry->valid)
      continue;

    if (entry->reference_val < ref_orig)
      entry->reference_val++;
  }

  cache_debug_print_set(cache, set, way, CACHE_EVENT_HIT);
}

void lru_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  int ii;

  // insertion
  cache->entries[set][way].reference_val = 0;

  // aging
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];

    if (ii == way)
      continue;

    if (!entry->valid)
      continue;

    entry->reference_val++;
  }

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

Cache_Entry* lru_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  int ii;
  uns8 oldest_ref = 0;

  // search the oldest line
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];

    if (!entry->valid) {
      *way = ii;
      break;
    }
    if (entry->reference_val > oldest_ref) {
      *way = ii;
      oldest_ref = entry->reference_val;
    }
  }

  cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
  return &cache->entries[set][*way];
}

/**************************************************************************************/
/* NRU */
void nru_update_hit(Cache* cache, uns set, uns way, void* arg);
void nru_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* nru_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

const static uns8 NRU_DISTANT_VAL = 1;

void nru_update_hit(Cache* cache, uns set, uns way, void* arg) {
  // promotion: near immediate -> RRPV = 0
  cache->entries[set][way].reference_val = 0;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_HIT);
}

void nru_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  // insertion: near immediate -> RRPV = 0
  cache->entries[set][way].reference_val = NRU_DISTANT_VAL;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

Cache_Entry* nru_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  int ii;
  Flag found = FALSE;

  /* A free way is taken before any valid line, and taking one ages nothing. (Folding this into
     the search below as "invalid or distant" in one pass let a distant valid line in a lower way
     beat a free way; with lines inserted at the distant value -- NRU, BRRIP, SHiP -- a set then
     filled only as its lines were reused.) */
  for (ii = 0; ii < cache->assoc; ii++) {
    if (!cache->entries[set][ii].valid) {
      *way = ii;
      cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
      return &cache->entries[set][*way];
    }
  }

  while (!found) {
    // eviction: search the distant line whose RRPV == 1
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (entry->reference_val == NRU_DISTANT_VAL) {
        *way = ii;
        found = TRUE;
        break;
      }
    }

    // aging: increment until there is a RRPV == 1
    if (found)
      break;
    for (ii = 0; ii < cache->assoc; ii++)
      cache->entries[set][ii].reference_val++;
  }

  cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
  return &cache->entries[set][*way];
}

/**************************************************************************************/
/* RRIP: SRRIP, BRRIP, DRRIP */
const static uns8 RRIP_M = 2;
const static uns8 RRIP_DISTANT_VAL = (1 << RRIP_M) - 1;

/**************************************************************************************/
/* SRRIP */
void srrip_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* rrip_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

void srrip_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  // insertion: long interval -> RRPV = 2^M - 2
  cache->entries[set][way].reference_val = RRIP_DISTANT_VAL - 1;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

/* mlp_rrip_boost 1: LIN (Qureshi et al., ISCA 2006) over the RRIP eviction order, the stack-position
   boost of REPL_MLP with RRIP in place of LRU. A line's position p is how many lines the plain search
   would evict before it (higher RRPV first, then lower way); the victim is argmin(p + mlp_boost),
   ties to the lower p. A marked line is passed over while an unmarked line sits fewer than mlp_boost
   places behind it, so marked lines can hold about mlp_boost ways of a set, however stale.

   The set ages until an UNmarked line reaches the distant RRPV (any line, if every line is marked):
   a marked line parked at the distant RRPV must not stop the rest of the set from aging, or lines
   hit once and never again would never climb back to be evicted. With every boost 0 this is the
   plain search. Every way is valid here. */
static Cache_Entry* rrip_evict_lin(Cache* cache, uns set, uns* way) {
  uns ii, jj;
  Flag any_unmarked = FALSE;
  for (ii = 0; ii < cache->assoc; ii++)
    if (cache->entries[set][ii].mlp_boost <= 0.0)
      any_unmarked = TRUE;

  for (;;) {
    Flag found = FALSE;
    for (ii = 0; ii < cache->assoc && !found; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      found = entry->reference_val == RRIP_DISTANT_VAL && (!any_unmarked || entry->mlp_boost <= 0.0);
    }
    if (found)
      break;
    for (ii = 0; ii < cache->assoc; ii++)
      if (cache->entries[set][ii].reference_val < RRIP_DISTANT_VAL)
        cache->entries[set][ii].reference_val++;
  }

  uns best_ind = 0, best_pos = 0;
  double best_val = 0.0;
  for (ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];
    uns pos = 0;
    for (jj = 0; jj < cache->assoc; jj++) {
      uns8 other = cache->entries[set][jj].reference_val;
      if (other > entry->reference_val || (other == entry->reference_val && jj < ii))
        pos++;
    }
    double val = (double)pos + (double)entry->mlp_boost;
    if (ii == 0 || val < best_val || (val == best_val && pos < best_pos)) {
      best_ind = ii;
      best_pos = pos;
      best_val = val;
    }
  }
  *way = best_ind;
  return &cache->entries[set][best_ind];
}

/* The RRIP victim search: a free way or a line at the distant RRPV, aging the set until there is one.
   SRRIP, BRRIP, DRRIP and SHiP all evict through it; they differ only in how they insert and promote.

   mlp_rrip_boost 2: a line with mlp_rrip_credit left is not yet distant, and an aging step spends a
   credit instead of raising its RRPV, as if the line had been inserted or promoted that many levels
   lower. Its protection lapses once the credits are spent, unless a hit renews them. With no credits
   (every other mode) this is the plain search. mlp_rrip_boost 1 picks the victim in rrip_evict_lin. */
Cache_Entry* rrip_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  int ii;
  Flag found = FALSE;

  /* A free way is taken before any valid line, and taking one ages nothing. (Folding this into
     the search below as "invalid or distant" in one pass let a distant valid line in a lower way
     beat a free way; with lines inserted at the distant value -- NRU, BRRIP, SHiP -- a set then
     filled only as its lines were reused.) */
  for (ii = 0; ii < cache->assoc; ii++) {
    if (!cache->entries[set][ii].valid) {
      *way = ii;
      cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
      return &cache->entries[set][*way];
    }
  }

  if (MLP_RRIP_BOOST == MLP_RRIP_BOOST_LAMBDA) {
    rrip_evict_lin(cache, set, way);
    cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
    return &cache->entries[set][*way];
  }

  while (!found) {
    // eviction: search the distant line whose RRPV == 2^M - 1
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (entry->reference_val == RRIP_DISTANT_VAL && entry->mlp_rrip_credit == 0) {
        *way = ii;
        found = TRUE;
        break;
      }
    }

    // aging: increment until there is a RRPV == 2^M-1 (spending mlp_rrip_credit first)
    if (found)
      break;
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (entry->mlp_rrip_credit > 0)
        entry->mlp_rrip_credit--;
      else if (entry->reference_val < RRIP_DISTANT_VAL)
        entry->reference_val++;
    }
  }

  cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
  return &cache->entries[set][*way];
}

/**************************************************************************************/
/* BRRIP */
const static uns BRRIP_BIMODAL_PARA = 32;
const static uns BRRIP_BIMODAL_SRAND_NUM = 0;
void brrip_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                       Repl_Policy repl_policy);
void brrip_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);

void brrip_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                       Repl_Policy repl_policy) {
  general_action_init(cache, name, cache_size, assoc, line_size, data_size, repl_policy);

  // Counter Impl
  cache->bimodal_count = 0;

  // Pseudo-Random Impl
  srand(BRRIP_BIMODAL_SRAND_NUM);
}

void brrip_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  Flag bimodal_para;

  // insertion: most in distant future
  // Counter Impl
  /*
  cache->bimodal_count = (cache->bimodal_count + 1) % BRRIP_BIMODAL_PARA;
  bimodal_para = cache->bimodal_count == 0 ? TRUE : FALSE;
  */

  // Pseudo-Random Impl
  bimodal_para = rand() % BRRIP_BIMODAL_PARA <= BRRIP_BIMODAL_PARA - 2 ? TRUE : FALSE;

  if (bimodal_para) {
    // insertion in distant future
    cache->entries[set][way].reference_val = RRIP_DISTANT_VAL;
    DEBUG(0, "BRRIP insert in distant: %d, %d\n", bimodal_para, cache->entries[set][way].reference_val);
  } else {
    // insertion in long-interval future
    cache->entries[set][way].reference_val = RRIP_DISTANT_VAL - 1;
    DEBUG(0, "BRRIP insert in long-interval: %d, %d\n", bimodal_para, cache->entries[set][way].reference_val);
  }

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

/**************************************************************************************/
/* DRRIP -- Jaleel, Theobald, Steely & Emer, "High Performance Cache Replacement Using
 * Re-Reference Interval Prediction (RRIP)", ISCA 2010, Sec. 4: set dueling between SRRIP and
 * BRRIP.
 *
 *  - Leader sets always insert with SRRIP or always with BRRIP. Every other set (a follower)
 *    inserts with whichever of the two is currently missing less.
 *  - PSEL, a --drrip_psel_bits saturating counter, starts at its midpoint. A miss in an SRRIP
 *    leader increments it and a miss in a BRRIP leader decrements it, so PSEL's top bit set
 *    means SRRIP's leaders have been missing more, and followers then insert with BRRIP.
 *  - A miss is counted once per fill, in update_insert. Nothing is counted in update_evict,
 *    which also runs for get_next_repl_line's peek at the victim and again every time a
 *    refused fill is retried.
 *  - Hits promote to RRPV 0 (nru_update_hit); the victim search is rrip_update_evict.
 *
 * One set in every four leads each policy. */
#define DRRIP_FOLLOWER 0
#define DRRIP_LEADER_SRRIP 1
#define DRRIP_LEADER_BRRIP 2

typedef struct Drrip_State_struct {
  uns8* role; /* per set: DRRIP_FOLLOWER or the policy it leads */
  uns psel;
  uns psel_max;
} Drrip_State;

void drrip_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                       Repl_Policy repl_policy);
void drrip_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);

static inline Drrip_State* drrip_state(Cache* cache) {
  return (Drrip_State*)cache->predictor;
}

void drrip_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                       Repl_Policy repl_policy) {
  Drrip_State* s;
  int ii;
  int shift_dedicated = 0;

  brrip_action_init(cache, name, cache_size, assoc, line_size, data_size, repl_policy);
  ASSERTM(0, DRRIP_PSEL_BITS >= 1 && DRRIP_PSEL_BITS <= 30, "--drrip_psel_bits must be in [1, 30] (got %u)\n",
          DRRIP_PSEL_BITS);

  s = (Drrip_State*)calloc(1, sizeof(Drrip_State));
  s->role = (uns8*)calloc(cache->num_sets, sizeof(uns8));
  for (ii = 0; ii + 4 <= cache->num_sets; ii = ii + 4) {
    s->role[ii + shift_dedicated % 4] = DRRIP_LEADER_BRRIP;
    s->role[ii + (shift_dedicated + 1) % 4] = DRRIP_LEADER_SRRIP;
    shift_dedicated = (shift_dedicated + 1) % 4;
  }
  s->psel_max = (1u << DRRIP_PSEL_BITS) - 1;
  s->psel = 1u << (DRRIP_PSEL_BITS - 1);
  cache->predictor = s;
}

void drrip_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  Drrip_State* s = drrip_state(cache);
  Flag use_brrip;

  switch (s->role[set]) {
    case DRRIP_LEADER_SRRIP:
      if (s->psel < s->psel_max)
        s->psel++;
      use_brrip = FALSE;
      break;
    case DRRIP_LEADER_BRRIP:
      if (s->psel > 0)
        s->psel--;
      use_brrip = TRUE;
      break;
    default:
      /* The top bit of PSEL: SRRIP's leaders have missed more. */
      use_brrip = (s->psel >> (DRRIP_PSEL_BITS - 1)) & 1 ? TRUE : FALSE;
      break;
  }

  if (use_brrip)
    brrip_update_insert(cache, proc_id, set, way, arg);
  else
    srrip_update_insert(cache, proc_id, set, way, arg);

  DEBUG(0, "DRRIP insert: set 0x%x role %u psel %u brrip %d\n", set, s->role[set], s->psel, use_brrip);
}

/**************************************************************************************/
/* SHiP */
void ship_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                      Repl_Policy repl_policy);
void ship_update_hit(Cache* cache, uns set, uns way, void* arg);
void ship_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* ship_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

/* Signiture History Counter Table */
struct ship_shct {
  Hash_Table shct_hash;
  Cache_Repl_Signiture shct_key_tpye;
};

void ship_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                      Repl_Policy repl_policy) {
  int ii, jj;
  uns num_sets = cache_size / line_size / assoc;
  general_action_init(cache, name, cache_size, assoc, line_size, data_size, repl_policy);

  /* allocate history table */
  cache->predictor = malloc(sizeof(struct ship_shct));
  struct ship_shct* cache_shct = (struct ship_shct*)cache->predictor;
  init_hash_table(&cache_shct->shct_hash, "cache repl ship shct", NODE_TABLE_SIZE, sizeof(Counter));
  cache_shct->shct_key_tpye = CACHE_REPL_SIGH_MEM;

  /* init outcome and sign for each line */
  for (ii = 0; ii < num_sets; ii++) {
    for (jj = 0; jj < assoc; jj++) {
      cache->entries[ii][jj].outcome = FALSE;
    }
  }
}

void ship_update_hit(Cache* cache, uns set, uns way, void* arg) {
  // promotion: near future -> RRPV = 0
  cache->entries[set][way].reference_val = 0;

  // prediction update
  cache->entries[set][way].outcome = TRUE;
  struct ship_shct* cache_shct = (struct ship_shct*)cache->predictor;
  Counter* cache_shct_entry = (Counter*)hash_table_access(
      &cache_shct->shct_hash, cache_repl_signiture(&cache->entries[set][way], cache_shct->shct_key_tpye));
  (*cache_shct_entry)++;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_HIT);
}

void ship_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  struct ship_shct* cache_shct = (struct ship_shct*)cache->predictor;
  Flag new_entry = FALSE;
  Counter* cache_shct_entry = (Counter*)hash_table_access_create(
      &cache_shct->shct_hash, cache_repl_signiture(&cache->entries[set][way], cache_shct->shct_key_tpye), &new_entry);
  if (new_entry)
    *cache_shct_entry = 0;

  if (*cache_shct_entry == 0) {
    // insertion in distant future
    cache->entries[set][way].reference_val = RRIP_DISTANT_VAL;
  } else {
    // insertion in long-interval future
    cache->entries[set][way].reference_val = RRIP_DISTANT_VAL - 1;
  }

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

Cache_Entry* ship_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  Cache_Entry* line = rrip_update_evict(cache, proc_id, set, way, arg, if_external);

  // if it is called by external eviction, do not change the prediction value
  if (if_external)
    return line;

  // prediction update: train on the evicted block if it was never reused. A free (invalid) way
  // held no block, so there is nothing to learn from it.
  if (line->valid && !line->outcome) {
    cache->entries[set][*way].outcome = TRUE;
    struct ship_shct* cache_shct = (struct ship_shct*)cache->predictor;
    Counter* cache_shct_entry = (Counter*)hash_table_access(
        &cache_shct->shct_hash, cache_repl_signiture(&cache->entries[set][*way], cache_shct->shct_key_tpye));

    if (cache_shct_entry != NULL && (*cache_shct_entry) > 0)
      (*cache_shct_entry)--;
  }
  line->outcome = FALSE;

  cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
  return line;
}

/**************************************************************************************/
/* LACS -- Kharbutli & Sheikh, "LACS: A Locality-Aware Cost-Sensitive Cache Replacement
 * Algorithm", IEEE TC 2014.
 *
 *  - Each line's reference_val holds a learned cost, 0..LACS_MAX_COST: memory/lacs.c trains
 *    it per line address from how many instructions the core issued while that line's miss
 *    was outstanding (few issued -> the core stalled on it -> high cost; many -> the miss
 *    was hidden -> low cost), and stages the trained cost here via cache_set_lacs_next_insert
 *    right before the fill's cache_insert -- the same stage-before-fill shape REPL_MLP's
 *    g_next_fill_mlp_boost already uses, just consumed by the strategy table's update_insert
 *    instead of cache_insert_replpos, since REPL_LACS (unlike REPL_MLP) sits after REPL_SHIP
 *    in the enum and dispatches through the strategy table.
 *  - Eviction: a free way is taken first (same reasoning as the RRIP family -- inserting at
 *    cost 0 could otherwise starve empty ways the same way inserting at the distant RRPV
 *    did before 213826f). Otherwise the victim is a cost-0 line; if none exists, every line
 *    in the set is aged down by the set's minimum cost first (so at least one reaches 0),
 *    then one of the (possibly several) cost-0 lines is picked by a cache-private RNG.
 *  - Hits promote: reference_val saturates up toward LACS_MAX_COST on reuse, the mirror image
 *    of training's "this address's misses are usually hidden" signal.
 *  - The RNG is private to the cache (via predictor), not shared: REPL_RANDOM's rand_repl_state
 *    is never initstate()'d (an existing latent bug, not touched here) and BRRIP/DRRIP's
 *    rand() shares libc's single global stream with anything else in the process that calls
 *    it -- neither is a safe building block to extend. It only advances on a real eviction
 *    (if_external == FALSE); get_next_repl_line's peek reads the same state without moving it,
 *    so the peek and the real eviction that immediately follows it agree on the same line by
 *    construction -- nothing else can touch this cache's RNG state or any line's reference_val
 *    in a cache between those two calls in that flow. */
#define LACS_MAX_COST 3
#define LACS_INIT_COST 1
#define LACS_MAX_ASSOC 1024

typedef struct Lacs_State_struct {
  uns32 rng_state;
  Counter evictions;
  Counter evict_aging;
  Counter evict_ties;
} Lacs_State;

void lacs_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                      Repl_Policy repl_policy);
void lacs_update_hit(Cache* cache, uns set, uns way, void* arg);
void lacs_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* lacs_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

static inline Lacs_State* lacs_state(Cache* cache) {
  return (Lacs_State*)cache->predictor;
}

/* xorshift32. Never returns 0 once seeded away from it, so '% num_candidates' below never
   divides by a state that can't advance. */
static inline uns32 lacs_rng_next(uns32 state) {
  state ^= state << 13;
  state ^= state >> 17;
  state ^= state << 5;
  return state;
}

/* A distinct, nonzero, deterministic (reproducible across runs) seed per cache that ends up
   running LACS -- there may be more than one (MLC and LLC can both be configured this way). */
static uns32 g_lacs_seed = 0x9e3779b9u;

void lacs_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                      Repl_Policy repl_policy) {
  Lacs_State* s = (Lacs_State*)calloc(1, sizeof(Lacs_State));
  ASSERTM(0, assoc <= LACS_MAX_ASSOC, "%s: associativity %u exceeds LACS_MAX_ASSOC (%u)\n", name, assoc,
          LACS_MAX_ASSOC);
  g_lacs_seed = lacs_rng_next(g_lacs_seed);
  s->rng_state = g_lacs_seed;
  cache->predictor = s;
}

void lacs_update_hit(Cache* cache, uns set, uns way, void* arg) {
  Cache_Entry* entry = &cache->entries[set][way];

  // Step G: promotion on reuse
  if (entry->reference_val < LACS_MAX_COST)
    entry->reference_val++;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_HIT);
}

void lacs_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  cache->entries[set][way].reference_val =
      g_lacs_next_valid ? (uns8)MIN2(g_lacs_next_cost, (uns)LACS_MAX_COST) : LACS_INIT_COST;
  g_lacs_next_valid = FALSE;
  g_lacs_next_cost = 0;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

Cache_Entry* lacs_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  int ii;
  uns8 min_cost = LACS_MAX_COST;
  uns candidates[LACS_MAX_ASSOC];
  uns num_candidates = 0;
  Lacs_State* s = lacs_state(cache);

  /* A free way is taken before any valid line, and taking one ages nothing -- same as every
     RRIP-family search (see 213826f). */
  for (ii = 0; ii < cache->assoc; ii++) {
    if (!cache->entries[set][ii].valid) {
      *way = ii;
      cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
      return &cache->entries[set][*way];
    }
  }

  // Step C: every way is valid here, so the set's minimum cost is well-defined.
  for (ii = 0; ii < cache->assoc; ii++)
    min_cost = MIN2(min_cost, cache->entries[set][ii].reference_val);
  for (ii = 0; ii < cache->assoc; ii++) {
    if (cache->entries[set][ii].reference_val == min_cost)
      candidates[num_candidates++] = ii;
  }

  *way = candidates[s->rng_state % num_candidates];

  if (!if_external) {
    // Aging: bring the set down to the minimum cost, so at least one line reads 0 next time.
    if (min_cost > 0) {
      for (ii = 0; ii < cache->assoc; ii++)
        cache->entries[set][ii].reference_val -= min_cost;
      s->evict_aging++;
    }
    if (num_candidates > 1)
      s->evict_ties++;
    s->evictions++;
    s->rng_state = lacs_rng_next(s->rng_state);
  }

  cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
  return &cache->entries[set][*way];
}

void cache_lacs_counters(Cache* cache, Counter* evictions, Counter* evict_aging, Counter* evict_ties) {
  Lacs_State* s = lacs_state(cache);
  *evictions = s ? s->evictions : 0;
  *evict_aging = s ? s->evict_aging : 0;
  *evict_ties = s ? s->evict_ties : 0;
}

/**************************************************************************************/
/* ICARUS -- Kalbande, Jedhe Deshmukh, Ros & Panda, "ICARUS: Criticality and Reuse based
 * Instruction Caching for Datacenter Applications", ASPLOS 2026.
 *
 * Two bits per line -- icarus_critical (staged at fill time, or set later on an MLC hit, by
 * memory/icarus.c from its branch-history-contextualized Critical Instruction Table; always
 * FALSE for data lines) and icarus_reused (set on any hit, cleared on insert) -- place every
 * line into one of four priority bins: [0,0] < [0,1] < [1,1] < [1,0]. Each bin has a
 * watermark (the number of lines it keeps); eviction scans bins lowest-priority-first and
 * only takes a victim from a bin whose current occupancy in this set is above its watermark,
 * picking the least-recently-used line within that bin (this codebase has no tree-PLRU, so
 * last_access_time -- stamped here on insert/hit, not by the generic LRU path, since the
 * strategy-table-driven policies don't touch it themselves -- stands in for it). */
void icarus_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                        Repl_Policy repl_policy);
void icarus_update_hit(Cache* cache, uns set, uns way, void* arg);
void icarus_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg);
Cache_Entry* icarus_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external);

static inline uns icarus_bin_of(Cache_Entry* entry) {
  return (entry->icarus_critical ? 2u : 0u) | (entry->icarus_reused ? 1u : 0u);
}

/* Priority order, lowest first: [0,0], [0,1], [1,1], [1,0]. */
static const uns ICARUS_BIN_PRIORITY[4] = {0u, 1u, 3u, 2u};

static inline uns icarus_watermark(uns bin) {
  switch (bin) {
    case 0:
      return ICARUS_WMARK_00;
    case 1:
      return ICARUS_WMARK_01;
    case 3:
      return ICARUS_WMARK_11;
    case 2:
      return ICARUS_WMARK_10;
    default:
      return 0;
  }
}

void icarus_action_init(Cache* cache, const char* name, uns cache_size, uns assoc, uns line_size, uns data_size,
                        Repl_Policy repl_policy) {
  general_action_init(cache, name, cache_size, assoc, line_size, data_size, repl_policy);
  // Sum of watermarks must be less than or equal to ways per set
  ASSERTM(0, ICARUS_WMARK_00 + ICARUS_WMARK_01 + ICARUS_WMARK_11 + ICARUS_WMARK_10 <= assoc,
          "REPL_ICARUS watermarks (%u+%u+%u+%u=%u) exceed %s's associativity (%u) -- pass "
          "--icarus_wmark_* values that sum to <= assoc, or --mlc_assoc=16 to match the paper's "
          "L2 config\n",
          ICARUS_WMARK_00, ICARUS_WMARK_01, ICARUS_WMARK_11, ICARUS_WMARK_10,
          ICARUS_WMARK_00 + ICARUS_WMARK_01 + ICARUS_WMARK_11 + ICARUS_WMARK_10, name, assoc);
}

// On a hit, mark the icarus reused bit as true and update LRU state
void icarus_update_hit(Cache* cache, uns set, uns way, void* arg) {
  cache->entries[set][way].icarus_reused = TRUE;
  cache->entries[set][way].last_access_time = cycle_count;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_HIT);
}

// Handles insertion under the icarus policy.
void icarus_update_insert(Cache* cache, uns8 proc_id, uns set, uns way, void* arg) {
  // If global per-insertion criticality is set, mark line as critical
  cache->entries[set][way].icarus_critical = g_icarus_next_valid ? g_icarus_next_critical : FALSE;
  // Line hasnt been reused yet
  cache->entries[set][way].icarus_reused = FALSE;
  cache->entries[set][way].last_access_time = cycle_count;
  // Reset for next insertion
  g_icarus_next_valid = FALSE;
  g_icarus_next_critical = FALSE;

  cache_debug_print_set(cache, set, way, CACHE_EVENT_INSERT);
}

static inline void icarus_stat_evict(uns8 proc_id, uns bin) {
  switch (bin) {
    case 0:
      STAT_EVENT(proc_id, MLC_ICARUS_EVICT_00);
      break;
    case 1:
      STAT_EVENT(proc_id, MLC_ICARUS_EVICT_01);
      break;
    case 3:
      STAT_EVENT(proc_id, MLC_ICARUS_EVICT_11);
      break;
    case 2:
      STAT_EVENT(proc_id, MLC_ICARUS_EVICT_10);
      break;
    default:
      break;
  }
}

Cache_Entry* icarus_update_evict(Cache* cache, uns8 proc_id, uns set, uns* way, void* arg, Flag if_external) {
  int ii;

  // First elect to evict an invalid way
  for (ii = 0; ii < cache->assoc; ii++) {
    if (!cache->entries[set][ii].valid) {
      *way = ii;
      cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
      return &cache->entries[set][*way];
    }
  }

  // Loop through all bins from low prio to high prio
  for (int bb = 0; bb < 4; bb++) {
    uns bin = ICARUS_BIN_PRIORITY[bb];
    uns count = 0;
    int lru_way = -1;
    Counter lru_time = 0;
    
    // Count number of ways in set in our bin
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (icarus_bin_of(entry) != bin)
        continue;
      count++;
      if (lru_way == -1 || entry->last_access_time < lru_time) {
        lru_way = ii;
        lru_time = entry->last_access_time;
      }
    }

    // If this bin has more entries than its watermark, elect to evict from this bin
    // (Fig. 9: "#lines > w"; a watermark is the number of lines a bin keeps, so a bin at
    // exactly its watermark is protected)
    if (count > icarus_watermark(bin)) {
      *way = lru_way;
      if (!if_external)
        icarus_stat_evict(proc_id, bin);
      cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
      return &cache->entries[set][*way];
    }
  }

  /* No bin is above its watermark, so every bin holds exactly its watermark (possible when the
     watermarks sum to assoc, as the defaults do). Fig. 9 then evicts the LRU line of [0,0]; if
     [0,0] is empty (watermark 0), take the lowest-priority bin that has a line. */
  int lru_way = -1;
  for (int bb = 0; bb < 4 && lru_way == -1; bb++) {
    for (ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (icarus_bin_of(entry) == ICARUS_BIN_PRIORITY[bb] &&
          (lru_way == -1 || entry->last_access_time < cache->entries[set][lru_way].last_access_time))
        lru_way = ii;
    }
  }
  ASSERT(proc_id, lru_way != -1);
  *way = lru_way;
  if (!if_external)
    icarus_stat_evict(proc_id, icarus_bin_of(&cache->entries[set][lru_way]));
  cache_debug_print_set(cache, set, *way, CACHE_EVENT_EVICT);
  return &cache->entries[set][*way];
}

/* REPL_MLP (stats only): whether a resident marked line is still protected. */
static inline Flag mlp_protected(Cache* cache, uns set, Cache_Entry* entry) {
  if (MLP_BOOST_COUNTDOWN)
    return entry->mlp_ticks_left > 0;
  if (MLP_BOOST_IN_FILLS && cache->mlp_set_fills)
    return (double)(cache->mlp_set_fills[set] - entry->mlp_stamp) < (double)entry->mlp_boost;
  return TRUE;
}

/* addr's resident line, or NULL; *set gets its set. */
static Cache_Entry* mlp_find_line(Cache* cache, Addr addr, uns* set) {
  Addr tag, tag_full, line_addr;
  *set = cache_index(cache, addr, &tag, &tag_full, &line_addr);
  for (uns ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[*set][ii];
    if (entry->valid && entry->tag == tag)
      return entry;
  }
  return NULL;
}

uns cache_set_index(Cache* cache, Addr addr) {
  Addr tag, tag_full, line_addr;
  return cache_index(cache, addr, &tag, &tag_full, &line_addr);
}

uns8 cache_mlp_mark(Cache* cache, Addr addr) {
  uns set;
  Cache_Entry* entry = mlp_find_line(cache, addr, &set);
  return entry ? entry->mlp_mark : MLP_MARK_NONE;
}

uns8 cache_mlp_demand_hit(Cache* cache, Addr addr) {
  uns set;
  Cache_Entry* entry = mlp_find_line(cache, addr, &set);
  if (!entry)
    return MLP_MARK_NONE;
  entry->mlp_used = TRUE;
  return entry->mlp_mark;
}

Flag cache_mlp_lapsed(Cache* cache, Addr addr) {
  uns set;
  Cache_Entry* entry = mlp_find_line(cache, addr, &set);
  return entry && entry->mlp_mark != MLP_MARK_NONE && !mlp_protected(cache, set, entry);
}

void cache_mlp_census(Cache* cache, Counter* valid, Counter* dmark, Counter* imark, Counter* inwindow) {
  for (uns set = 0; set < cache->num_sets; set++) {
    for (uns ii = 0; ii < cache->assoc; ii++) {
      Cache_Entry* entry = &cache->entries[set][ii];
      if (!entry->valid)
        continue;
      (*valid)++;
      if (entry->mlp_mark == MLP_MARK_NONE)
        continue;
      if (entry->mlp_mark == MLP_MARK_DATA)
        (*dmark)++;
      else
        (*imark)++;
      if (mlp_protected(cache, set, entry))
        (*inwindow)++;
    }
  }
}

/* REPL_ICARUS: the CIT's criticality signal for a line already in the cache (see
   icarus_mlc_hit). Sets the critical bit without touching LRU state; returns TRUE if the bit
   was newly set, FALSE if the line was already critical or isn't present. */
Flag cache_icarus_mark_critical(Cache* cache, Addr addr) {
  Addr tag, tag_full, line_addr;
  uns set = cache_index(cache, addr, &tag, &tag_full, &line_addr);
  for (uns ii = 0; ii < cache->assoc; ii++) {
    Cache_Entry* entry = &cache->entries[set][ii];
    if (entry->valid && entry->tag == tag) {
      Flag newly = !entry->icarus_critical;
      entry->icarus_critical = TRUE;
      return newly;
    }
  }
  return FALSE;
}

/**************************************************************************************/
/* Driven Table */

// clang-format off
struct repl_policy_func repl_policy_func_table[NUM_REPL] = {
  { REPL_LRU_REF, general_action_init,  general_action_repl,  lru_update_hit,     lru_update_insert,    lru_update_evict    },
  { REPL_NRU,     general_action_init,  general_action_repl,  nru_update_hit,     nru_update_insert,    nru_update_evict    },
  { REPL_SRRIP,   general_action_init,  general_action_repl,  nru_update_hit,     srrip_update_insert,  rrip_update_evict   },
  { REPL_BRRIP,   brrip_action_init,    general_action_repl,  nru_update_hit,     brrip_update_insert,  rrip_update_evict   },
  { REPL_DRRIP,   drrip_action_init,    general_action_repl,  nru_update_hit,     drrip_update_insert,  rrip_update_evict   },
  { REPL_SHIP,    ship_action_init,     general_action_repl,  ship_update_hit,    ship_update_insert,   ship_update_evict   },
  { REPL_LACS,    lacs_action_init,     general_action_repl,  lacs_update_hit,    lacs_update_insert,   lacs_update_evict   },
  { REPL_ICARUS,  icarus_action_init,   general_action_repl,  icarus_update_hit,  icarus_update_insert, icarus_update_evict },
  { REPL_VOID,    NULL,                 NULL,                 NULL,               NULL,                 NULL                },
};
// clang-format on

/**************************************************************************************/
