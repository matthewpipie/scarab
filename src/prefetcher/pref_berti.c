/*
 * Copyright 2026 University of California Santa Cruz
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
 * File         : prefetcher/pref_berti.c
 * Author       : Litz Lab
 * Date         : 11/2026
 * Description  : Berti, after A. Navarro-Torres, B. Panda, J. Alastruey-Benede, P. Ibanez,
 *                V. Vinals-Yufera and A. Ros, "Berti: an Accurate Local-Delta Data
 *                Prefetcher", MICRO 2022. A dcache instance (TYPE_BERTI,DEST_DCACHE in
 *                --pref_dcache_prefetchers) that prefetches into the dcache or the MLC.
 *
 * Berti learns, for each load IP, the deltas (in lines, between two of the IP's loads)
 * that would have prefetched a line in time, and how often each would have: its coverage.
 * It prefetches with the deltas of high coverage only, which keeps it accurate.
 *
 * History table. Holds an IP's recent loads that missed the dcache, or that would have
 * missed without prefetching (first demand hits on prefetched lines), with their cycle.
 * Sets by the low IP bits, FIFO within a set.
 *
 * Timely deltas. When such a load gets its line, its fetch latency L is known: from its
 * miss to the fill, or for a prefetched line from the prefetch being queued to its fill.
 * A prefetch for it would have been in time if issued L cycles before the load, so the
 * IP's history entries at least that old give the timely deltas (this line minus theirs).
 * A search happens when a load miss fills the dcache, using its MSHR's timestamp, and when
 * a load first hits a prefetched line, using the latency kept with the line. A late
 * prefetch, one a load caught in flight, counts as that load's miss: its latency runs
 * from the prefetch being queued and its demand time is when the load caught it. A
 * latency that overflows its bits counts as 0 and nothing is learned.
 *
 * Table of deltas. One entry per IP (FIFO), with a search counter and up to
 * DELTAS_PER_ENTRY deltas, each with a coverage counter and a status. A search increments
 * the counter, and the coverage of each timely delta it found (adding the new ones). After
 * PHASE_SEARCHES searches the phase ends: each delta's coverage sets its status for the
 * next phase, and the counters restart. Above L1_WATERMARK percent of the searches it
 * prefetches into the dcache; above L2_WATERMARK into the MLC (L2_PREF_REPL if below
 * REPL_WATERMARK); otherwise not at all; at most MAX_PREF_DELTAS deltas prefetch, those of
 * highest coverage. A new delta takes a free slot, or replaces the delta of lowest
 * coverage whose status is NO_PREF or L2_PREF_REPL, or is dropped. Until an IP's first
 * phase ends it prefetches once it has WARMUP_SEARCHES searches, with the coverage so far
 * and WARMUP_L1_WATERMARK as the dcache watermark.
 *
 * Prefetching. Every dcache access by an IP in the table issues line + delta for each
 * prefetching delta of the IP, dcache ones first. A dcache prefetch goes to the MLC
 * instead while the dcache MSHRs are MSHR_WATERMARK percent full or more.
 *
 * Choices the paper leaves open: an IP's load to the line of its youngest history entry
 * is not recorded again (more loads to a line in flight would otherwise fill the history
 * with copies of it); a search's deltas are distinct, non-zero and fit DELTA_BITS; the
 * table of deltas allocates an entry on a search for an IP it lacks, even one that found
 * no timely delta; during warm-up the MLC watermark is unchanged; "at least eight deltas
 * have been gathered" is read as eight searches; the IP hash of the table of deltas
 * XOR-folds the IP. Scarab's dcache trains its prefetchers on load misses only, so stores
 * neither train Berti nor, having no entry, prefetch.
 ***************************************************************************************/

#include "prefetcher/pref_berti.h"

#include <stdlib.h>
#include <string.h>

#include "globals/assert.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "general.param.h"
#include "memory/memory.param.h"
#include "prefetcher/pref.param.h"
#include "prefetcher/pref_berti.param.h"

#include "memory/memory.h"
#include "memory/mshr.h"
#include "prefetcher/pref_common.h"

#include "statistics.h"

/**************************************************************************************/
/* Macros */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_PREF_BERTI, ##args)

/**************************************************************************************/
/* Global variables */

static HWP_Info* berti_hwp_info;
static Berti_Core* berti_cores;      // [NUM_CORES]
static uns berti_line_bits;          // log2 of the dcache line size
static uns berti_page_line_bits;     // log2 of the dcache lines in a page
static uns berti_hist_set_bits;      // log2 of BERTI_HIST_SETS
static int64 berti_delta_limit;      // deltas lie in [-berti_delta_limit, berti_delta_limit)
static int* berti_found;             // [BERTI_MAX_TIMELY_DELTAS], the last search's timely deltas
static Berti_Status* berti_scratch;  // [BERTI_DELTAS_PER_ENTRY], statuses being computed

/**************************************************************************************/
/* Time: cycles, compared in their low BERTI_TIME_BITS bits as the hardware's timestamps */

static inline Counter berti_age(Counter now, Counter then) {
  return (now - then) & N_BIT_MASK(BERTI_TIME_BITS);
}

/**************************************************************************************/
/* History table */

static inline uns berti_hist_set(Addr ip) {
  return ip & (BERTI_HIST_SETS - 1);
}

static inline uns berti_hist_tag(Addr ip) {
  return (ip >> berti_hist_set_bits) & N_BIT_MASK(BERTI_HIST_IP_TAG_BITS);
}

/* The k-th youngest way of a set, from k = 0 */
static inline Berti_Hist_Entry* berti_hist_way(Berti_Core* berti, uns set, uns k) {
  uns way = (berti->hist_next[set] + BERTI_HIST_WAYS - 1 - k) % BERTI_HIST_WAYS;
  return &berti->hist[set * BERTI_HIST_WAYS + way];
}

/* The delta from hist_line to line, as their low BERTI_HIST_LINE_BITS bits give it */
static inline int64 berti_hist_delta(Addr line, Addr hist_line) {
  Addr diff = (line - hist_line) & N_BIT_MASK(BERTI_HIST_LINE_BITS);
  Addr sign = 1ULL << (BERTI_HIST_LINE_BITS - 1);
  return (diff & sign) ? (int64)diff - (int64)(sign << 1) : (int64)diff;
}

static void berti_hist_insert(uns8 proc_id, Addr ip, Addr line, Flag pref_hit) {
  Berti_Core* berti = &berti_cores[proc_id];
  uns set = berti_hist_set(ip);
  uns tag = berti_hist_tag(ip);
  Addr hist_line = line & N_BIT_MASK(BERTI_HIST_LINE_BITS);

  /* A load to the line of the IP's youngest entry adds nothing */
  for (uns k = 0; k < BERTI_HIST_WAYS; k++) {
    Berti_Hist_Entry* entry = berti_hist_way(berti, set, k);
    if (entry->valid && entry->ip_tag == tag) {
      if (entry->line == hist_line) {
        STAT_EVENT(proc_id, BERTI_HIST_SAME_LINE);
        return;
      }
      break;
    }
  }

  Berti_Hist_Entry* entry = &berti->hist[set * BERTI_HIST_WAYS + berti->hist_next[set]];
  entry->valid = TRUE;
  entry->ip_tag = tag;
  entry->line = hist_line;
  entry->time = cycle_count;
  berti->hist_next[set] = (berti->hist_next[set] + 1) % BERTI_HIST_WAYS;
  STAT_EVENT(proc_id, pref_hit ? BERTI_HIST_INSERT_PREF_HIT : BERTI_HIST_INSERT_MISS);
}

/* Collect into berti_found the timely deltas of a load of line by ip: those from the IP's
   history entries at least min_age cycles old, youngest first, each once. Returns how many. */
static uns berti_hist_search(uns8 proc_id, Addr ip, Addr line, Counter min_age) {
  Berti_Core* berti = &berti_cores[proc_id];
  uns set = berti_hist_set(ip);
  uns tag = berti_hist_tag(ip);
  uns found = 0;

  for (uns k = 0; k < BERTI_HIST_WAYS && found < BERTI_MAX_TIMELY_DELTAS; k++) {
    Berti_Hist_Entry* entry = berti_hist_way(berti, set, k);
    if (!entry->valid || entry->ip_tag != tag || berti_age(cycle_count, entry->time) < min_age)
      continue;
    int64 delta = berti_hist_delta(line, entry->line);
    if (delta == 0)
      continue;
    if (delta < -berti_delta_limit || delta >= berti_delta_limit) {
      STAT_EVENT(proc_id, BERTI_DELTA_TOO_LARGE);
      continue;
    }
    Flag seen = FALSE;
    for (uns i = 0; i < found; i++)
      seen |= berti_found[i] == delta;
    if (!seen)
      berti_found[found++] = (int)delta;
  }
  return found;
}

/**************************************************************************************/
/* Table of deltas */

/* XOR-folds the IP into BERTI_TABLE_TAG_BITS bits */
static inline uns berti_table_tag(Addr ip) {
  uns tag = 0;
  for (; ip; ip >>= BERTI_TABLE_TAG_BITS)
    tag ^= ip & N_BIT_MASK(BERTI_TABLE_TAG_BITS);
  return tag;
}

static Berti_Entry* berti_table_find(uns8 proc_id, Addr ip) {
  Berti_Core* berti = &berti_cores[proc_id];
  uns tag = berti_table_tag(ip);
  for (uns i = 0; i < BERTI_TABLE_SIZE; i++) {
    if (berti->table[i].valid && berti->table[i].tag == tag)
      return &berti->table[i];
  }
  return NULL;
}

static Berti_Entry* berti_table_alloc(uns8 proc_id, Addr ip) {
  Berti_Core* berti = &berti_cores[proc_id];
  Berti_Entry* entry = &berti->table[berti->table_next];
  berti->table_next = (berti->table_next + 1) % BERTI_TABLE_SIZE;
  STAT_EVENT(proc_id, entry->valid ? BERTI_TABLE_EVICT : BERTI_TABLE_FILL);
  entry->valid = TRUE;
  entry->tag = berti_table_tag(ip);
  entry->searches = 0;
  entry->learned = FALSE;
  memset(entry->deltas, 0, BERTI_DELTAS_PER_ENTRY * sizeof(Berti_Delta));
  return entry;
}

/* The statuses that the entry's coverage over its searches so far gives, into status[]:
   the deltas above BERTI_L2_WATERMARK, highest coverage first (ties to the lower slot), up
   to BERTI_MAX_PREF_DELTAS. Returns how many were above it but left out by that cap. */
static uns berti_statuses(Berti_Entry* entry, uns l1_watermark, Berti_Status* status) {
  uns searches = entry->searches;
  uns above = 0;
  for (uns i = 0; i < BERTI_DELTAS_PER_ENTRY; i++) {
    Berti_Delta* d = &entry->deltas[i];
    status[i] = BERTI_NO_PREF;
    above += d->valid && d->coverage * 100 > BERTI_L2_WATERMARK * searches;
  }

  uns chosen = 0;
  for (; chosen < MIN2(above, BERTI_MAX_PREF_DELTAS); chosen++) {
    int best = -1;
    for (uns i = 0; i < BERTI_DELTAS_PER_ENTRY; i++) {
      Berti_Delta* d = &entry->deltas[i];
      if (d->valid && status[i] == BERTI_NO_PREF && d->coverage * 100 > BERTI_L2_WATERMARK * searches &&
          (best < 0 || d->coverage > entry->deltas[best].coverage))
        best = i;
    }
    ASSERT(0, best >= 0);
    uns coverage = entry->deltas[best].coverage;
    status[best] = coverage * 100 > l1_watermark * searches           ? BERTI_L1_PREF
                   : coverage * 100 < BERTI_REPL_WATERMARK * searches ? BERTI_L2_PREF_REPL
                                                                      : BERTI_L2_PREF;
  }
  return above - chosen;
}

static void berti_end_phase(uns8 proc_id, Berti_Entry* entry) {
  uns capped = berti_statuses(entry, BERTI_L1_WATERMARK, berti_scratch);
  uns num[BERTI_L2_PREF_REPL + 1] = {0};
  for (uns i = 0; i < BERTI_DELTAS_PER_ENTRY; i++) {
    Berti_Delta* d = &entry->deltas[i];
    d->status = berti_scratch[i];
    d->coverage = 0;
    num[d->status] += d->valid;
  }
  entry->searches = 0;
  entry->learned = TRUE;

  STAT_EVENT(proc_id, BERTI_PHASE_END);
  INC_STAT_EVENT(proc_id, BERTI_PHASE_DELTAS_L1, num[BERTI_L1_PREF]);
  INC_STAT_EVENT(proc_id, BERTI_PHASE_DELTAS_L2, num[BERTI_L2_PREF]);
  INC_STAT_EVENT(proc_id, BERTI_PHASE_DELTAS_L2_REPL, num[BERTI_L2_PREF_REPL]);
  INC_STAT_EVENT(proc_id, BERTI_PHASE_DELTAS_NONE, num[BERTI_NO_PREF]);
  INC_STAT_EVENT(proc_id, BERTI_PHASE_DELTAS_CAPPED, capped);
  if (!num[BERTI_L1_PREF] && !num[BERTI_L2_PREF] && !num[BERTI_L2_PREF_REPL])
    STAT_EVENT(proc_id, BERTI_PHASE_END_NO_PREF);
  DEBUG(proc_id, "phase end: %u dcache, %u MLC, %u MLC-replaceable, %u capped\n", num[BERTI_L1_PREF],
        num[BERTI_L2_PREF], num[BERTI_L2_PREF_REPL], capped);
}

/* A search by ip found the found timely deltas in berti_found */
static void berti_train(uns8 proc_id, Addr ip, uns found) {
  Berti_Entry* entry = berti_table_find(proc_id, ip);
  if (!entry)
    entry = berti_table_alloc(proc_id, ip);
  entry->searches++;

  for (uns j = 0; j < found; j++) {
    Berti_Delta *match = NULL, *free_slot = NULL, *victim = NULL;
    for (uns i = 0; i < BERTI_DELTAS_PER_ENTRY && !match; i++) {
      Berti_Delta* d = &entry->deltas[i];
      if (!d->valid) {
        free_slot = free_slot ? free_slot : d;
      } else if (d->delta == berti_found[j]) {
        match = d;
      } else if ((d->status == BERTI_NO_PREF || d->status == BERTI_L2_PREF_REPL) &&
                 (!victim || d->coverage < victim->coverage)) {
        victim = d;
      }
    }
    if (match) {
      match->coverage++;
      continue;
    }
    Berti_Delta* d = free_slot ? free_slot : victim;
    if (!d) {
      STAT_EVENT(proc_id, BERTI_DELTA_DROP);
      continue;
    }
    STAT_EVENT(proc_id, free_slot ? BERTI_DELTA_INSERT : BERTI_DELTA_REPLACE);
    d->valid = TRUE;
    d->delta = berti_found[j];
    d->coverage = 1;
    d->status = BERTI_NO_PREF;
  }

  if (entry->searches >= BERTI_PHASE_SEARCHES)
    berti_end_phase(proc_id, entry);
}

/**************************************************************************************/
/* Prefetching */

static void berti_prefetch(uns8 proc_id, Addr line_addr, Addr ip) {
  Berti_Entry* entry = berti_table_find(proc_id, ip);
  STAT_EVENT(proc_id, entry ? BERTI_ACCESS_IP_HIT : BERTI_ACCESS_IP_MISS);
  if (!entry)
    return;

  Berti_Status* status = berti_scratch;
  if (entry->learned) {
    for (uns i = 0; i < BERTI_DELTAS_PER_ENTRY; i++)
      status[i] = entry->deltas[i].valid ? entry->deltas[i].status : BERTI_NO_PREF;
  } else if (entry->searches >= BERTI_WARMUP_SEARCHES) {
    berti_statuses(entry, BERTI_WARMUP_L1_WATERMARK, status);
    STAT_EVENT(proc_id, BERTI_ACCESS_WARMUP);
  } else {
    return;
  }

  Mshr* mshr = mem_core_file(proc_id, MRT_DFETCH);
  Flag l1_room = mshr_count(mshr) * 100 < BERTI_MSHR_WATERMARK * mshr->size;
  Addr line = line_addr >> berti_line_bits;

  /* The dcache deltas first, then the MLC ones */
  for (uns pass = 0; pass < 2; pass++) {
    Flag l1_pass = pass == 0;
    for (uns i = 0; i < BERTI_DELTAS_PER_ENTRY; i++) {
      if (status[i] == BERTI_NO_PREF || (status[i] == BERTI_L1_PREF) != l1_pass)
        continue;
      Addr target = line + (Addr)(int64)entry->deltas[i].delta;
      if (!BERTI_CROSS_PAGE && target >> berti_page_line_bits != line >> berti_page_line_bits) {
        STAT_EVENT(proc_id, BERTI_PREF_CROSS_PAGE);
        continue;
      }
      if (get_proc_id_from_cmp_addr(target << berti_line_bits) != proc_id) {
        STAT_EVENT(proc_id, BERTI_PREF_OUT_OF_RANGE);
        continue;
      }
      Flag to_l1 = status[i] == BERTI_L1_PREF && l1_room;
      if (status[i] == BERTI_L1_PREF && !l1_room)
        STAT_EVENT(proc_id, BERTI_PREF_L1_MSHR_BUSY);
      if (pref_addto_dest_req_queue(proc_id, to_l1 ? PREF_TO_DCACHE : PREF_TO_UMLC, target, berti_hwp_info->id))
        STAT_EVENT(proc_id, to_l1 ? BERTI_PREF_QUEUED_L1 : BERTI_PREF_QUEUED_L2);
      else
        STAT_EVENT(proc_id, BERTI_PREF_QUEUE_FULL);
    }
  }
}

/**************************************************************************************/
/* HWP interface */

void pref_berti_init(HWP* hwp) {
  if (!pref_hwp_enabled(hwp))
    return;

  ASSERTM(0,
          pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_DCACHE) &&
              !pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_UMLC) &&
              !pref_hwp_instance_enabled(hwp, PREF_TRAIN_LEVEL_UL1),
          "Berti is implemented at the dcache only: list it in --pref_dcache_prefetchers\n");
  ASSERTM(0, pref_hwp_instance_dest(hwp, PREF_TRAIN_LEVEL_DCACHE) == PREF_TO_DCACHE,
          "Berti picks the dcache or the MLC for each prefetch: use TYPE_BERTI,DEST_DCACHE\n");
  ASSERTM(0, MLC_PRESENT, "Berti sends its MLC prefetches to the MLC: it needs --mlc_present 1\n");
  ASSERTM(0, is_power_of_2(BERTI_HIST_SETS) && BERTI_HIST_WAYS >= 1,
          "berti_hist_sets (%u) must be a power of two and berti_hist_ways (%u) at least 1\n", BERTI_HIST_SETS,
          BERTI_HIST_WAYS);
  ASSERTM(0, BERTI_HIST_IP_TAG_BITS <= 32 && BERTI_TABLE_TAG_BITS >= 1 && BERTI_TABLE_TAG_BITS <= 32,
          "berti_hist_ip_tag_bits (%u) must be at most 32, and berti_table_tag_bits (%u) 1 to 32\n",
          BERTI_HIST_IP_TAG_BITS, BERTI_TABLE_TAG_BITS);
  ASSERTM(0,
          BERTI_DELTA_BITS >= 2 && BERTI_DELTA_BITS <= 31 && BERTI_DELTA_BITS <= BERTI_HIST_LINE_BITS &&
              BERTI_HIST_LINE_BITS <= 58,
          "berti_delta_bits (%u) must be 2 to 31 and at most berti_hist_line_bits (%u), itself at most 58\n",
          BERTI_DELTA_BITS, BERTI_HIST_LINE_BITS);
  ASSERTM(0, BERTI_LATENCY_BITS >= 1 && BERTI_LATENCY_BITS < BERTI_TIME_BITS && BERTI_TIME_BITS <= 63,
          "berti_latency_bits (%u) must be at least 1 and below berti_time_bits (%u), itself at most 63\n",
          BERTI_LATENCY_BITS, BERTI_TIME_BITS);
  ASSERTM(
      0,
      BERTI_TABLE_SIZE >= 1 && BERTI_DELTAS_PER_ENTRY >= 1 && BERTI_MAX_TIMELY_DELTAS >= 1 && BERTI_PHASE_SEARCHES >= 1,
      "berti_table_size, berti_deltas_per_entry, berti_max_timely_deltas and berti_phase_searches must be at "
      "least 1\n");
  ASSERTM(0,
          BERTI_L2_WATERMARK <= BERTI_L1_WATERMARK && BERTI_L2_WATERMARK <= BERTI_WARMUP_L1_WATERMARK &&
              BERTI_L1_WATERMARK <= 100 && BERTI_WARMUP_L1_WATERMARK <= 100 && BERTI_REPL_WATERMARK <= 100 &&
              BERTI_MSHR_WATERMARK <= 100,
          "Berti's watermarks are percentages, and berti_l2_watermark may not exceed the dcache ones\n");
  ASSERTM(0, is_power_of_2(BERTI_PAGE_SIZE) && BERTI_PAGE_SIZE >= DCACHE_LINE_SIZE,
          "berti_page_size (%u) must be a power of two of at least a dcache line\n", BERTI_PAGE_SIZE);

  hwp->hwp_info->enabled = TRUE;
  berti_hwp_info = hwp->hwp_info;
  berti_line_bits = LOG2(DCACHE_LINE_SIZE);
  berti_page_line_bits = LOG2(BERTI_PAGE_SIZE) - berti_line_bits;
  berti_hist_set_bits = LOG2(BERTI_HIST_SETS);
  berti_delta_limit = (int64)1 << (BERTI_DELTA_BITS - 1);
  berti_found = (int*)calloc(BERTI_MAX_TIMELY_DELTAS, sizeof(int));
  berti_scratch = (Berti_Status*)calloc(BERTI_DELTAS_PER_ENTRY, sizeof(Berti_Status));

  berti_cores = (Berti_Core*)calloc(NUM_CORES, sizeof(Berti_Core));
  for (uns proc_id = 0; proc_id < NUM_CORES; proc_id++) {
    Berti_Core* berti = &berti_cores[proc_id];
    berti->hist = (Berti_Hist_Entry*)calloc(BERTI_HIST_SETS * BERTI_HIST_WAYS, sizeof(Berti_Hist_Entry));
    berti->hist_next = (uns*)calloc(BERTI_HIST_SETS, sizeof(uns));
    berti->table = (Berti_Entry*)calloc(BERTI_TABLE_SIZE, sizeof(Berti_Entry));
    for (uns i = 0; i < BERTI_TABLE_SIZE; i++)
      berti->table[i].deltas = (Berti_Delta*)calloc(BERTI_DELTAS_PER_ENTRY, sizeof(Berti_Delta));
  }
}

/* Every dcache access looks its IP up in the table of deltas; a load miss also goes into
   the history table. */
void pref_berti_dl0_miss(Addr line_addr, Addr load_PC) {
  if (!load_PC)
    return;
  uns8 proc_id = get_proc_id_from_cmp_addr(line_addr);
  berti_prefetch(proc_id, line_addr, load_PC);
  berti_hist_insert(proc_id, load_PC, line_addr >> berti_line_bits, FALSE);
}

void pref_berti_dl0_hit(Addr line_addr, Addr load_PC) {
  if (load_PC)
    berti_prefetch(get_proc_id_from_cmp_addr(line_addr), line_addr, load_PC);
}

void pref_berti_dl0_pref_hit(Addr line_addr, Addr load_PC) {
  if (load_PC)
    berti_prefetch(get_proc_id_from_cmp_addr(line_addr), line_addr, load_PC);
}

/* A load that would have missed without prefetching got its line, after latency cycles
   (see dl0_load_fetched in pref_common.h): search the history table for its timely deltas.
   A first hit on a prefetched line also goes into the history table. */
void pref_berti_dl0_load_fetched(uns8 proc_id, Addr line_addr, Addr load_PC, Counter demand_cycle, Counter latency,
                                 Flag pref_hit) {
  if (!load_PC)
    return;
  Addr line = line_addr >> berti_line_bits;

  /* The latency as a difference of BERTI_TIME_BITS-bit timestamps, kept in BERTI_LATENCY_BITS */
  Counter lat = latency & N_BIT_MASK(BERTI_TIME_BITS);
  if (lat > N_BIT_MASK(BERTI_LATENCY_BITS))
    lat = 0;
  if (!lat) {
    STAT_EVENT(proc_id, BERTI_SEARCH_NO_LATENCY);
  } else {
    ASSERT(proc_id, cycle_count >= demand_cycle);
    Flag late = !pref_hit && cycle_count - demand_cycle < latency;
    STAT_EVENT(proc_id, pref_hit ? BERTI_SEARCH_PREF_HIT : late ? BERTI_SEARCH_LATE_PREF : BERTI_SEARCH_MISS);
    INC_STAT_EVENT(proc_id, BERTI_SEARCH_LATENCY, lat);
    /* In time means issued lat cycles before the load, which was cycle_count - demand_cycle ago */
    uns found = berti_hist_search(proc_id, load_PC, line, cycle_count - demand_cycle + lat);
    STAT_EVENT(proc_id, BERTI_TIMELY_DELTAS_0 + MIN2(found, BERTI_TIMELY_DELTAS_8_UP - BERTI_TIMELY_DELTAS_0));
    berti_train(proc_id, load_PC, found);
  }

  if (pref_hit)
    berti_hist_insert(proc_id, load_PC, line, TRUE);
}
