/*
 * Copyright 2020 HPS/SAFARI Research Groups
 * Copyright 2025 Litz Lab
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
 * File         : map_stage.c
 * Author       : HPS Research Group, Litz Lab
 * Date         : 2/4/1999, 3/2025
 * Description  :
 ***************************************************************************************/

#include "map_stage.h"

#include "globals/assert.h"
#include "globals/debug_stage.h"
#include "globals/global_defs.h"
#include "globals/global_types.h"
#include "globals/global_vars.h"
#include "globals/utils.h"

#include "debug/debug.param.h"
#include "debug/debug_macros.h"
#include "debug/debug_print.h"

#include "core.param.h"
#include "memory/memory.param.h"

#include "bp/bp.h"
#include "memory/mem_req.h"

#include "ft.h"
#include "map.h"
#include "map_rename.h"
#include "model.h"
#include "op_pool.h"
#include "statistics.h"
#include "thread.h"

/**************************************************************************************/
/* Macros */
#define DEBUG(proc_id, args...) _DEBUG(proc_id, DEBUG_MAP_STAGE, ##args)
#define STAGE_MAX_OP_COUNT RENAME_WIDTH
#define STAGE_MAX_DEPTH MAP_CYCLES

/**************************************************************************************/
/* Global Variables */

Map_Stage* map = NULL;

/**************************************************************************************/
/* Local prototypes */

static inline void stage_process_op(Op*);
static inline void map_stage_collect_stat(Flag, Flag);
static inline void map_stage_fetch_op(Stage_Data*);

/**************************************************************************************/
/* set_map_stage: */

void set_map_stage(Map_Stage* new_map) {
  map = new_map;
}

/**************************************************************************************/
/* init_map_stage: */

void init_map_stage(uns8 proc_id, const char* name) {
  uns ii;
  ASSERT(proc_id, map);
  ASSERT(proc_id, STAGE_MAX_DEPTH > 0);
  DEBUG(proc_id, "Initializing %s stage\n", name);

  memset(map, 0, sizeof(Map_Stage));
  map->proc_id = proc_id;

  map->sds = (Stage_Data*)malloc(sizeof(Stage_Data) * STAGE_MAX_DEPTH);
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    /* Stage_Data::name is only used for debugging/printing; reuse the long-lived
     * stage name pointer passed into init_map_stage().
     */
    cur->name = (char*)name;
    cur->max_op_count = STAGE_MAX_OP_COUNT;
    cur->ops = (Op**)malloc(sizeof(Op*) * STAGE_MAX_OP_COUNT);
  }
  map->last_sd = &map->sds[0];
  map->off_path = 0;
  map->next_op_num = 1;
  reset_map_stage();
}

/**************************************************************************************/
/* reset_map_stage: */

void reset_map_stage() {
  uns ii, jj;
  ASSERT(0, map);
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[ii];
    cur->op_count = 0;
    for (jj = 0; jj < STAGE_MAX_OP_COUNT; jj++)
      cur->ops[jj] = NULL;
  }

  map->reg_file_stall = FALSE;
}

/**************************************************************************************/
/* recover_map_stage: */

void recover_map_stage() {
  uns ii, jj, kk;
  map->off_path = 0;
  ASSERT(0, map);
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Flag flushed = FALSE;
    Stage_Data* cur = &map->sds[ii];
    cur->op_count = 0;

    for (jj = 0, kk = 0; jj < STAGE_MAX_OP_COUNT; jj++) {
      if (cur->ops[jj]) {
        if (IS_FLUSHING_OP(cur->ops[jj])) {
          op_select_bp_pred_info(cur->ops[jj], BP_PRED_MAIN);
          DEBUG(map->proc_id, "Recovery op found in Map stage:%u slot:%u op_num:%llu off_path:%u addr:0x%llx\n", ii, jj,
                (unsigned long long)cur->ops[jj]->op_num, cur->ops[jj]->off_path,
                (unsigned long long)cur->ops[jj]->inst->addr);
        }
        if (FLUSH_OP(cur->ops[jj])) {
          DEBUG(map->proc_id, "Map flushing op_num:%llu off_path:%u\n", (unsigned long long)cur->ops[jj]->op_num,
                cur->ops[jj]->off_path);
          flushed = TRUE;
          ASSERT(map->proc_id, cur->ops[jj]->off_path);
          if (cur->ops[jj]->parent_FT)
            ft_free_op(cur->ops[jj]);
          cur->ops[jj] = NULL;
        } else {
          Op* op = cur->ops[jj];
          cur->op_count++;
          cur->ops[jj] = NULL;  // collapse the ops
          cur->ops[kk++] = op;
        }
      }
    }

    if (cur->op_count > 0 && flushed) {
      Op* op = cur->ops[cur->op_count - 1];
      assert_ft_after_recovery(map->proc_id, op, bp_recovery_info->recovery_fetch_addr);
    }
  }

  if (map->next_op_num > bp_recovery_info->recovery_op_num) {
    map->next_op_num = bp_recovery_info->recovery_op_num + 1;
    DEBUG(map->proc_id, "Recovering map->next_op_num to %llu\n", map->next_op_num);
  }
}

/**************************************************************************************/
/* debug_map_stage: */

void debug_map_stage() {
  uns ii;
  for (ii = 0; ii < STAGE_MAX_DEPTH; ii++) {
    Stage_Data* cur = &map->sds[STAGE_MAX_DEPTH - ii - 1];
    DPRINTF("# %-10s  op_count:%d\n", cur->name, cur->op_count);
    DPRINTF("# %-10s  op_nums:", cur->name);
    print_stage_op_nums(GLOBAL_DEBUG_STREAM, cur->ops, cur->op_count);
    DPRINTF("\n");
    print_op_array(GLOBAL_DEBUG_STREAM, cur->ops, STAGE_MAX_OP_COUNT, STAGE_MAX_OP_COUNT);
  }
}

/**************************************************************************************/
/* map_cycle: */

void update_map_stage(Stage_Data* src_sd) {
  /* stall if the renaming table is full */
  if (!reg_file_available(STAGE_MAX_OP_COUNT)) {
    map->reg_file_stall = TRUE;
    DEBUG(map->proc_id,
          "Map Stage stalled (reg_file_full) last_sd_op_num:%s last_sd_op_count:%d src_op_num:%s src_op_count:%d\n",
          (map->last_sd->op_count && map->last_sd->ops[0]) ? unsstr64(map->last_sd->ops[0]->op_num) : "none",
          map->last_sd->op_count, (src_sd->op_count && src_sd->ops[0]) ? unsstr64(src_sd->ops[0]->op_num) : "none",
          src_sd->op_count);
    STAT_EVENT(map->proc_id, MAP_STAGE_STALL_ITSELF);
    return;
  }
  map->reg_file_stall = FALSE;
  STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STALL_ITSELF);

  Flag stall = (map->last_sd->op_count > 0);
  Flag starved = (src_sd->op_count == 0);
  map_stage_collect_stat(stall, starved);

  /* do all the intermediate stages */
  for (int ii = 0; ii < STAGE_MAX_DEPTH - 1; ii++) {
    Stage_Data* cur = &map->sds[ii];
    Stage_Data* prev = &map->sds[ii + 1];

    if (cur->op_count)
      continue;

    Op** temp = cur->ops;
    cur->ops = prev->ops;
    prev->ops = temp;
    cur->op_count = prev->op_count;
    prev->op_count = 0;
  }

  /* do the first map stage */
  if (map->sds[STAGE_MAX_DEPTH - 1].op_count == 0 && !starved) {
    map_stage_fetch_op(src_sd);
  }

  /* if the last map stage is stalled, don't re-process the ops  */
  if (stall) {
    DEBUG(map->proc_id, "Map Stage stalled op_num:%s last_sd_op_count:%d\n",
          (map->last_sd->op_count && map->last_sd->ops[0]) ? unsstr64(map->last_sd->ops[0]->op_num) : "none",
          map->last_sd->op_count);
    return;
  }

  /* now map the ops in the last map stage */
  for (int ii = 0; ii < map->last_sd->op_count; ii++) {
    Op* op = map->last_sd->ops[ii];
    ASSERT(map->proc_id, op != NULL);
    stage_process_op(op);
  }
}

/**************************************************************************************/
/* Local methods */

/* REPL_MLP mlp_data_branch: a load still in the machine that src names as its feed load. */
static inline Op* mlp_live_feed(Op* src, Counter src_unique) {
  if (!src || !src->op_pool_valid || src->unique_num != src_unique)
    return NULL;
  Op* load = src->mlp_feed_load;
  if (!load || !load->op_pool_valid || load->unique_num != src->mlp_feed_load_unique)
    return NULL;
  return load;
}

/* REPL_MLP mlp_data_branch: whether op is a branch the mode looks at (see memory.param.def). */
Flag mlp_branch_in_scope(Op* op) {
  ASSERTM(op->proc_id, MLP_DATA_BRANCH <= 2, "mlp_data_branch %u is not 0, 1 or 2\n", MLP_DATA_BRANCH);
  Cf_Type t = op->uop->cf_type;
  if (MLP_DATA_BRANCH == 1)
    return t == CF_CBR;
  if (MLP_DATA_BRANCH == 2)
    return t == CF_CBR || t == CF_IBR || t == CF_ICALL || t == CF_ICO;
  return FALSE;
}

/* REPL_MLP mlp_data_branch: whether an in-scope branch counts as hard. Mode 1 reads the TAGE
   provider confidence the predictor gave this op; mode 2 cheats with the simulator's knowledge that
   this on-path branch mispredicts and recovers at exec. */
static Flag mlp_branch_is_hard(Op* op) {
  if (MLP_DATA_BRANCH == 1)
    return op->bp_confidence >= 0 && op->bp_confidence < 32 && ((MLP_DATA_BRANCH_CONF >> op->bp_confidence) & 1);
  return !op->off_path && op->bp_pred_info && op->bp_pred_info->recovery_point == RECOVER_AT_EXEC;
}

/* REPL_MLP mlp_data_branch, at rename: the op takes the youngest in-flight load among its register
   sources' feed loads (a load takes itself, its value being the load's), modeling a load tag per
   physical register. A hard in-scope branch flags that load, and the request the load is waiting
   on if it has one (a later request it creates or joins copies the flag; memory.c), so the line's
   MLC fill is marked. A source whose producer or feed load has left the machine names no load: its
   data is back, so its fill has happened. */
static void mlp_branch_rename(Op* op) {
  if (!MLP_DATA_BRANCH)
    return;
  Op* feed = NULL;
  if (op->uop->mem_type == MEM_LD) {
    feed = op;
  } else {
    for (uns ii = 0; ii < op->num_srcs; ii++) {
      if (op->src_info[ii].type != REG_DATA_DEP)
        continue;
      Op* load = mlp_live_feed(op->src_info[ii].op, op->src_info[ii].unique_num);
      if (load && (!feed || load->unique_num > feed->unique_num))
        feed = load;
    }
  }
  op->mlp_feed_load = feed;
  op->mlp_feed_load_unique = feed ? feed->unique_num : 0;

  if (!op->uop->cf_type || !mlp_branch_in_scope(op))
    return;
  op->mlp_br_fed = feed != NULL;
  op->mlp_br_hard = mlp_branch_is_hard(op);
  if (feed && op->mlp_br_hard) {
    feed->mlp_feeds_hard_branch = TRUE;
    if (feed->req && op_get_done_cycle(feed) == MAX_CTR)
      feed->req->feeds_hard_branch = TRUE;
  }
}

static inline void stage_process_op(Op* op) {
  ASSERT(map->proc_id, map->proc_id == td->proc_id);

  if (op->uop->cf_type == CF_CBR)
    STAT_EVENT(op->proc_id, op->uop->num_src_regs ? MAP_CBR_WITH_SRC_REGS : MAP_CBR_NO_SRC_REGS);

  /* REPL_MLP mlp_data_branch: before map_wakeup_list_add invalidates retired sources */
  mlp_branch_rename(op);

  /* register renaming allocation */
  reg_file_rename(op);

  /* setting wake up lists */
  map_wakeup_list_add(op, model->wake_hook);
}

static inline void map_stage_collect_stat(Flag stall, Flag starved) {
  if (map->off_path) {
    STAT_EVENT(map->proc_id, MAP_STAGE_OFF_PATH);
    return;
  }

  if (stall)
    STAT_EVENT(map->proc_id, MAP_STAGE_STALLED);
  else
    STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STALLED);

  if (starved)
    STAT_EVENT(map->proc_id, MAP_STAGE_STARVED);
  else
    STAT_EVENT(map->proc_id, MAP_STAGE_NOT_STARVED);
}

static inline void map_stage_fetch_op(Stage_Data* src_sd) {
  Stage_Data* first_sd = &map->sds[STAGE_MAX_DEPTH - 1];
  int op_count_before_fetch = src_sd->op_count;

  for (int ii = 0; ii < op_count_before_fetch; ii++) {
    Op* op = src_sd->ops[ii];
    ASSERT(map->proc_id, op->op_num == map->next_op_num);
    DEBUG(map->proc_id, "Fetching opnum=%llu at idx=%i\n", op->op_num, ii);

    op_set_map_cycle(op, cycle_count);
    first_sd->ops[ii] = op;
    first_sd->op_count++;

    src_sd->ops[ii] = NULL;
    src_sd->op_count--;

    map->next_op_num++;
    if (op->off_path) {
      map->off_path = 1;
    }
  }

  // TODO: probably should count number of on-path ops.
  if (!map->off_path)
    STAT_EVENT(map->proc_id, MAP_STAGE_RECEIVED_OPS_0 + first_sd->op_count);

  // Any stage can receive a mix of on/off-path ops in a single cycle.
  ASSERT(map->proc_id, first_sd->op_count <= MAP_STAGE_RECEIVED_OPS_MAX);
}
