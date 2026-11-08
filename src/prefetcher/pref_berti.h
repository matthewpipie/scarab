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
 * File         : prefetcher/pref_berti.h
 * Author       : Litz Lab
 * Date         : 11/2026
 * Description  : Berti, a local-delta prefetcher (Navarro-Torres et al., "Berti: an
 *                Accurate Local-Delta Data Prefetcher", MICRO 2022), trained by the
 *                dcache and prefetching into the dcache or the MLC. See pref_berti.c.
 ***************************************************************************************/

#ifndef __PREF_BERTI_H__
#define __PREF_BERTI_H__

#include "pref_common.h"

/* Where a delta prefetches to in the current phase, set at the end of the previous one */
typedef enum Berti_Status_enum {
  BERTI_NO_PREF,
  BERTI_L1_PREF,      /* the dcache, while its MSHRs are below the watermark; else the MLC */
  BERTI_L2_PREF,      /* the MLC */
  BERTI_L2_PREF_REPL, /* the MLC, and a new delta may replace it */
} Berti_Status;

/* One load in the history table: a load miss, or a first demand hit on a prefetched line */
typedef struct Berti_Hist_Entry_Struct {
  Flag valid;
  uns ip_tag;
  Addr line;    /* low BERTI_HIST_LINE_BITS bits of the line address */
  Counter time; /* cycle of the access; only its low BERTI_TIME_BITS bits are compared */
} Berti_Hist_Entry;

typedef struct Berti_Delta_Struct {
  Flag valid;
  int delta;           /* in lines */
  uns coverage;        /* searches of this phase that found it timely */
  Berti_Status status; /* from the previous phase */
} Berti_Delta;

/* One IP in the table of deltas */
typedef struct Berti_Entry_Struct {
  Flag valid;
  uns tag;             /* hash of the IP */
  uns searches;        /* searches of this phase */
  Flag learned;        /* a phase has ended, so the statuses are set */
  Berti_Delta* deltas; /* [BERTI_DELTAS_PER_ENTRY] */
} Berti_Entry;

/* One core's Berti */
typedef struct Berti_Core_Struct {
  Berti_Hist_Entry* hist; /* [BERTI_HIST_SETS * BERTI_HIST_WAYS], set by set */
  uns* hist_next;         /* [BERTI_HIST_SETS], the way a set writes next */
  Berti_Entry* table;     /* [BERTI_TABLE_SIZE] */
  uns table_next;         /* the entry the table replaces next */
} Berti_Core;

/*************************************************************/
/* HWP Interface */
void pref_berti_init(HWP* hwp);
void pref_berti_dl0_miss(Addr line_addr, Addr load_PC);
void pref_berti_dl0_hit(Addr line_addr, Addr load_PC);
void pref_berti_dl0_pref_hit(Addr line_addr, Addr load_PC);
void pref_berti_dl0_load_fetched(uns8 proc_id, Addr line_addr, Addr load_PC, Counter demand_cycle, Counter latency,
                                 Flag pref_hit);

#endif /* __PREF_BERTI_H__ */
