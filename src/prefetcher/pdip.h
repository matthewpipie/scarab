/* Copyright 2026 HPS/SAFARI Research Groups
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
 * File         : prefetcher/pdip.h
 * Description  : Priority Directed Instruction Prefetching (PDIP).
 *                Godala et al., "PDIP: Priority Directed Instruction Prefetching", ASPLOS'24.
 ***************************************************************************************/

#ifndef __PDIP_H__
#define __PDIP_H__

#include "globals/global_types.h"

#ifdef __cplusplus
extern "C" {
#endif

struct Op_struct;

/* Which event a front-end-critical (FEC) line is blamed on (Op.pdip_trigger_kind). */
typedef enum Pdip_Trigger_Kind_enum {
  PDIP_TRIG_NONE,
  PDIP_TRIG_RESTEER,   /* the mispredicting / BTB-missing branch whose flush exposed the miss */
  PDIP_TRIG_LASTTAKEN, /* no resteer: the last taken branch retired when the miss began */
} Pdip_Trigger_Kind;

void alloc_mem_pdip(uns num_cores);
void init_pdip(uns proc_id);

/* Decoupled front end (MAIN_BP): a fetch target for `line` entered the FTQ (table lookup), and
   the FTQ is full (FDIP has its full lead time again since the last resteer). off_path is
   for stats only. */
void pdip_ftq_push(uns proc_id, Addr line, Flag off_path);
void pdip_note_ftq_full(uns proc_id);

/* Icache stage. A demand fetch missed both the uop cache and the icache on `line`; each cycle
   fetch delivered nothing while waiting on it; the line arrived; an op was served from the
   icache (stamped if it is the first from an armed FEC line). */
void pdip_demand_miss(uns proc_id, Addr line);
void pdip_miss_stall_cycle(uns proc_id, Addr line, Flag iq_empty);
void pdip_demand_fill(uns proc_id, Addr line);
void pdip_mark_op(uns proc_id, struct Op_struct* op);

/* Branch recovery (called from recover_icache_stage, with bp_recovery_info current).
   fetch_redirected: the icache stage leaves what it was fetching. */
void pdip_recover(uns proc_id, Flag fetch_redirected);

/* Retirement: track the last taken branch; train the table on a stamped op. */
void pdip_retire(struct Op_struct* op);

/* Once per cycle, after the icache stage, the DFE and FDIP: drain the prefetch queue. */
void update_pdip(uns proc_id);

#ifdef __cplusplus
}
#endif

#endif /* __PDIP_H__ */
