# REPL_MLP

REPL_MLP (cache replacement policy 9) is LRU plus protection for lines whose misses stalled the core. When a line is filled, REPL_MLP decides once whether its miss stalled the core. If it did, the line is **marked** and gets a boost γ: one value for instruction lines, another for data lines. The replacement mode then decides how the boost protects the line from eviction. With both γ at 0 (the default), REPL_MLP is plain LRU.

This page lists every flag and its values, then the recommended settings and the runs behind them. The flags are defined in `src/memory/memory.param.def`.

## Recommended

```
--mlc_cache_repl_policy 9 --mlp_boost_in_fills 1 --mlp_lin_instr_lambda 64 --mlp_lin_data_lambda 16
```

This runs REPL_MLP at the L2. It protects each marked line for a set number of fills into its set after its last touch: 64 for instruction lines, 16 for data lines. Every other flag stays at its default. The runs behind it are under [Results](#results).

## How it works

1. **Marking.** Each fill into a cache running REPL_MLP is classified once, when it is filled:
   - A fill that a load waited on is a data fill. The [data rule](#data-marking) decides whether it is marked.
   - Otherwise, a fill that a demand fetch reached is an instruction fill. The [instruction rule](#instruction-marking) decides whether it is marked.
   - Nothing else is ever marked. That includes data prefetches no load waited on, instruction prefetches no demand fetch reached, store-only requests and writebacks.
2. **Boost.** A marked data line gets `mlp_lin_data_lambda`, a marked instruction line gets `mlp_lin_instr_lambda`, and every other line gets 0. The mark stays with the line until it is evicted. A hit renews the line's protection, but it does not re-decide the mark.
3. **Replacement.** The [replacement mode](#replacement-mode) decides what the boost means. By default it is a countdown measured in fills into the set.

Every input to marking is something the core can see, and none of it reads `off_path`. Wrong-path requests go through the same rules. The rules only check that a stall and an outstanding miss coincided, not that this miss caused the stall.

Under the default flags:

- **A data line is marked** if, between a load merging into its request and the fill, there were at least 16 cycles in which the ROB head was a load waiting on that request, nothing retired, and no op issued to the functional units. Hardware needs a 5-bit saturating counter per MSHR, bumped from the ROB head in cycles the issue stage sent nothing.
- **An instruction line is marked** if a demand fetch reached it, fetch was not resteered away from it while it waited (or a demand fetch reached it again before the fill), and in at least one cycle all of these held:
  - **Fetch was stuck on this line.** Its request was outstanding, fetch and the uop cache delivered no ops, and the icache stage stopped on the miss.
  - **The backend was starved.** Rename passed no ops to the ROB, the ROB was not full, and rename was not stalled on registers.
  - **The ROB head was not a load waiting on memory.**

## Flags

### Turning it on

| Flag | Default | Values |
|---|---|---|
| `mlc_cache_repl_policy` | `0` (LRU) | `9` runs REPL_MLP at the L2 (MLC). `16` to `19` (SRRIP, BRRIP, DRRIP, SHiP) take the boost only with [`mlp_rrip_boost`](#rrip-as-the-base). |
| `l1_cache_repl_policy` | `0` (LRU) | The same values at the last-level cache, which Scarab calls L1. |

Each cache running REPL_MLP marks its own fills. Every result on this page has REPL_MLP at the L2 only.

### Boost size

| Flag | Type | Default | Values |
|---|---|---|---|
| `mlp_lin_instr_lambda` | float | `0` | γ for marked instruction lines. `0` means no instruction boost. 64 is recommended. |
| `mlp_lin_data_lambda` | float | `0` | γ for marked data lines. `0` means no data boost. 16 is recommended. 64 is better on datacenter but worse on every other suite. |

Use values of 0 or more. The unit depends on the mode:

| Mode | Unit of γ | Precision |
|---|---|---|
| Countdown (default) | Fills into the set | Rounded to a tick of p = max(1, round(γ/3)) fills. Any γ below 4.5 protects for 3 fills. |
| Fill-counted (`mlp_boost_in_fills 1`) | Fills into the set | Exact. Fractions are compared as they are. |
| Stack position | LRU stack positions | γ at or above the associativity (16 on `golden_small`'s L2) means a marked line is never evicted while its set holds an unmarked line. |
| RRIP base | Ways (`mlp_rrip_boost 1`) or aging steps (`mlp_rrip_boost 2`) | Mode 2 truncates γ to an integer. |

### Instruction marking

An instruction fill can be marked only if a demand fetch reached it: either a demand fetch created the request, or it merged into a prefetch of the line before the fill. With FDIP on, most marked lines are late prefetches. Four flags decide which of these fills are marked.

A **stuck cycle** is one in which the icache was pointed at the line while its request was outstanding, fetch and the uop cache delivered no ops, and the icache stage stopped on the miss.

**`mlp_instr_mark`** (default `2`) is the base rule.

| Value | Marks |
|---|---|
| `0` | Fills whose share of stuck cycles is above `mlp_lin_instr_thresh` |
| `1` | Every fill a demand fetch reached |
| `2` | The same, minus the ones fetch abandoned: it was resteered away while stuck on the line, and no demand fetch reached the line again before the fill |
| Other | Aborts |

**`mlp_instr_starve`** (default `2`) adds a second test on top of the base rule. It counts the stuck cycles in which the backend also had no useful work, and the fill must have at least `mlp_instr_starve_min` of them. The value sets what "no useful work" means:

| Value | A stuck cycle counts if |
|---|---|
| `0` | Off: no second test, so `mlp_instr_mark` alone decides |
| `1` | The backend was starved: rename passed no ops to the ROB, the ROB was not full, and rename was not stalled on registers |
| `2` | As 1, and the ROB head was not a load waiting on memory |
| `3` | The issue queue was empty |
| `4` | The ROB was empty |
| `5` | At most `topdown_fu_exec_few` functional units were busy (default 0, so none) |
| Other | Aborts |

With `mlp_instr_mark 0`, these counted cycles also replace the stuck cycles in the ratio.

**`mlp_instr_starve_min`** (unsigned, default `1`) is the fewest counted cycles a fill needs. `0` drops the minimum, so `mlp_instr_starve` then only changes the ratio of `mlp_instr_mark 0`. It is ignored when `mlp_instr_starve` is 0.

**`mlp_lin_instr_thresh`** (float, default `0.7`) is used only by `mlp_instr_mark 0`. The window is every cycle the icache was pointed at the line while its request was outstanding. The fill is marked if stuck cycles ÷ window cycles is above the threshold. `0` marks any fill with one stuck cycle, and `1` or more marks nothing.

### Data marking

A data fill is one that at least one load merged into. From the time a load merges into the request until the fill, REPL_MLP keeps these counts for it. An **fu_wait** cycle is one in which at most `topdown_fu_exec_few` functional units were busy and the ROB was not empty.

| Count | Adds, each cycle |
|---|---|
| ROB-head stall cycles | 1 if nothing retired because the ROB head was a load waiting on this request |
| No-issue ROB-head stall cycles | 1 if it is a ROB-head stall cycle and no op issued to the functional units either |
| fu_wait cycles | 1 if it is an fu_wait cycle and at least one load is waiting on this request |
| fu_wait load-cycles | 1 per load waiting on this request, in an fu_wait cycle |
| Window load-cycles | 1 per load waiting on this request |

**`mlp_data_bound`** (default `3`) picks the count that is checked against the minimum:

| Value | Count |
|---|---|
| `0` | fu_wait load-cycles. A request several loads merged into counts faster. |
| `1` | fu_wait cycles, once per request however many loads wait on it |
| `2` | ROB-head stall cycles. Each stall cycle is charged to one request, where fu_wait charges every miss in flight. The default before #42. |
| `3` | No-issue ROB-head stall cycles: the cycles of `2` in which no new work started either |
| Other | Aborts |

**`mlp_data_bound_min`** (unsigned, default `16`): the fill is marked if the count reached this. `0` drops the minimum.

**`mlp_lin_data_thresh`** (float, default `0`): `0` means no ratio test. Above 0, the fill must also have fu_wait load-cycles ÷ window load-cycles above the threshold. `1` or more marks nothing.

Useful combinations:

| Flags | Marks |
|---|---|
| None (default) | Fills with at least 16 no-issue ROB-head stall cycles |
| `--mlp_data_bound 2` | Fills with at least 16 ROB-head stall cycles, the default before #42 |
| `--mlp_data_bound 0 --mlp_data_bound_min 0 --mlp_lin_data_thresh 0.7` | The old ratio marking, the default before #35 |
| `--mlp_data_bound_min 0` | Every fill a load waited on |

**`topdown_fu_exec_few`** (unsigned, default `0`, in `src/core.param.def`) sets how many busy functional units still count as "few". It affects `mlp_data_bound` 0 and 1, the data ratio, and `mlp_instr_starve 5`. It is shared with the top-down stats, so changing it also changes `TOPDOWN_EXEC_STALLS_CYCLES`.

### Replacement mode

Two flags pick one of three modes:

| `mlp_boost_in_fills` | `mlp_boost_countdown` | Mode |
|---|---|---|
| `1` | Either | Fill-counted |
| `0` (default) | `1` (default) | Countdown |
| `0` | `0` | Stack position |

How the three modes compare:

| | Countdown (default) | Fill-counted | Stack position |
|---|---|---|---|
| Victim | The least recently used unprotected line | The smallest stamp + γ, ties to the least recently used. The stamp is the set's fill count at the line's last touch. | The smallest LRU stack position (0 = LRU) + γ, ties to the lower way |
| How long a marked line is protected | 2p+1 to 3p fills after its last touch, about ⅔γ to γ | Exactly γ fills after its last touch | Until it is more than γ positions older than some unmarked line |
| If every line in the set is protected | The least recently used line, or with `tiered` the one with the fewest ticks left | The line closest to lapsing | Not applicable |
| A hit on a marked line | Restarts the countdown, even after it lapsed (unless `no_revive`) | Restarts protection | Moves it to MRU and keeps γ |
| When protection lapses | The line moves to MRU | The line ranks as if touched at the lapse | Not applicable |
| State beyond LRU and the mark | A 2-bit countdown per line, a fill counter per set | A fill-count stamp per line, a fill counter per set | None |

In the countdown, a marked line's counter is set to 3 when it is filled or hit. Its set ticks every p fills, and each tick takes 1 off. A line's own fill does not tick it. p depends on the line's own γ, so instruction and data lines tick at different rates.

In stack position mode, a marked line is evicted ahead of an unmarked line only if it is more than γ positions older. If γ is at least the associativity, that never happens. Marked lines then pile up until the whole set is marked, and an instruction line at γi 64 always outranks a data line at γd 16.

**`mlp_boost_in_fills`** (default `0`): `1` selects the fill-counted mode and overrides `mlp_boost_countdown`.

**`mlp_boost_countdown`** (default `1`): `1` selects the countdown, unless `mlp_boost_in_fills` is 1. `0` selects stack position, as long as `mlp_boost_in_fills` is 0.

**`mlp_boost_countdown_tiered`** (default `1`) matters only in the countdown, and only when every line in the set is protected. `1` evicts the line with the fewest ticks left, with ties going to the least recently used. `0` evicts the least recently used line. Ticks are compared, not fills. At γi 64 and γd 16, one instruction tick is 21 fills and one data tick is 5, so tiered is only a rough version of the fill-counted mode's "closest to lapsing".

**`mlp_boost_countdown_no_revive`** (default `0`) matters only in the countdown.

- `0`: every hit on a marked line restarts its countdown.
- `1`: a hit restarts it only if it has not reached 0 yet. A lapsed line stays unprotected until it is evicted and filled again, since a fill always starts the countdown. A countdown of 0 then means "unprotected" for every line, which can save a bit per line.
- Setting it to 1 when the countdown is not in use aborts at startup, on any cache running REPL_MLP.

### RRIP as the base

**`mlp_rrip_boost`** (default `0`) applies REPL_MLP's marking and γ to SRRIP, BRRIP, DRRIP or SHiP (replacement policies 16 to 19) instead of LRU. The replacement mode flags above do nothing here.

| Value | Meaning |
|---|---|
| `0` | Off. The RRIP policies neither mark nor boost. |
| `1` | γ in ways, permanent. The victim is the line with the smallest place in the RRIP eviction order + γ, so marked lines can hold about γ ways of a set however stale they are. The set ages until an unmarked line reaches the distant RRPV. |
| `2` | γ in aging steps, lapsing. A marked line absorbs its first γ aging steps after a fill or hit without its RRPV rising, as if it had been inserted or promoted γ levels lower. |

Values above 2 are not checked. They mark lines and count the marking stats, but evict like plain RRIP.

### Stats

All of these are in `memory.stat.<core>.out`.

| Stat | Counts |
|---|---|
| `MLP_MARKED_LOAD` | Marked data fills |
| `MLP_MARKED_FETCH` | Marked instruction fills |
| `MLP_MARKED_MERGE` | Marked fills that more than one memory op, or more than one request type, merged into |
| `MLP_FE_ABANDON` | Resteers away from a line fetch was stuck on, under every policy |
| `MLP_FE_ABANDON_REREACH` | Abandoned lines a demand fetch reached again before the fill |
| `MLP_COUNTDOWN_EXPIRE` | Countdowns that reached 0 |
| `MLP_COUNTDOWN_EVICT_PROTECTED` | Victims evicted while still protected, which happens only when the whole set is protected |
| `MLP_LD_BOUND_LOG2_*`, `MLP_LD_BOUNDREQ_LOG2_*`, `MLP_LD_HEAD_LOG2_*`, `MLP_LD_HEADNI_LOG2_*` | Under every policy, at each L2 fill a load waited on: the request's count for `mlp_data_bound` 0, 1, 2 and 3 in log2 buckets. `_0` is 0, `_k` is 2^(k−1) to 2^k − 1, and `_15` is 2^14 and up. |
| `REQ_COST_ROB_{MLC,LLC,MEM}` | Cycles nothing retired because the ROB head was a load waiting on a request (the count `mlp_data_bound 2` uses), by the level that served it. Stats only. |
| `REQ_COST_FE_{MLC,LLC,MEM}` | Cycles the backend got no ops while the on-path icache waited on a request, by the level that served it. Stats only. |
| `REQ_BOUND_LD_*`, `REQ_BOUND_FE_*` | REPL_MLP's fu_wait load-cycles and instruction stuck cycles, split the same way, for comparison with the costs |
| `MLP_MLC_ACC_*`, `MLP_MLC_HIT_{D,I}MARK_*` | L2 lookups by kind (load, store, fetch, prefetch, writeback), and those that hit a line marked at its fill |
| `MLP_MLC_HIT_{D,I}MARK_LAPSED` | Demand hits on a boosted line whose protection had already run out |
| `MLP_EVICT_{ALL,DMARK,IMARK}{,_UNUSED}` | L2 evictions, of marked lines, and those no demand hit since their fill |
| `MLP_RD1_*`, `MLP_RDN_*` | Reuse distances per line, in fills to its set, by the kind of the line's first fill (data-marked, data unmarked, instruction-marked, instruction unmarked, other). `RD1` is the first reuse, measured from that fill; `RDN` is every later reuse, measured from the previous one. Same log2 buckets as above. |
| `MLP_RDNEXT_{DMARK,IMARK,MARK}_{LOOKUPS,FILLS}_LOG2_*` | The gap from one demand lookup of a marked line to the next (data, instruction, either), in L2 demand lookups on the core and in fills to the set |
| `MLP_LINES_*`, `MLP_OCC_*` | Distinct lines over the measured region, and a census of the L2 every 4096 cycles: valid, marked, and boosted lines still protected |

The L2 marks every fill under any policy and whatever the λs. Marking just labels the line; only REPL_MLP (or RRIP with `mlp_rrip_boost`) boosts it. So `MLP_MARKED_*` and every stat split by mark count what the rules decide, under LRU too, and with `mlp_lin_data_lambda 0` `MLP_MARKED_LOAD` still counts data marks. Only boosted lines are ever protected, so `_LAPSED` and `MLP_OCC_INWINDOW` count only those. The LLC marks, and counts its fills in `MLP_MARKED_*`, only when it runs REPL_MLP itself.

## Results

### Instruction marking

**Setup:** Google, 12 workloads, top 6 simpoints (63), 10M warmup + 50M measured instructions, `golden_small`. The policy is changed at the L2 only, with the instruction boost only and fill-counted γ. Each number is the geomean IPC gain over L2 LRU with the same FDIP setting. With FDIP off (`--fdip_enable 0`), LRU's IPC is 28.6% lower than with FDIP on.

| Marking | FDIP on, γ64 | FDIP on, γ128 | FDIP off, γ64 |
|---|---|---|---|
| **Default: `mlp_instr_mark 2`, `mlp_instr_starve 2`, min 1** | **+1.99** | **+1.95** | **+13.25** |
| `mlp_instr_mark 2` alone (`--mlp_instr_starve 0`) | +1.75 | +1.15 | +13.37 |
| `mlp_instr_starve 1`: starved, any ROB head | +1.93 | +1.53 | |
| `mlp_instr_starve 3`: issue queue empty | +1.72 | +1.88 | |
| `mlp_instr_starve 4`: ROB empty | +1.62 | +1.79 | |
| `mlp_instr_starve 5`: no FUs busy | +1.80 | +1.24 | |
| Starved, and no load waiting on memory anywhere (not merged) | +1.81 | +1.92 | +12.70 |
| `mlp_instr_starve_min` 4 / 16 / 64 | | | +13.09 / +12.31 / +10.38 |
| Old ratio (`--mlp_instr_mark 0 --mlp_instr_starve 0`) | | | +12.71 |
| Ratio over starved cycles (`--mlp_instr_mark 0`) | | | +9.80 |

γ 64 and 128 are about equally good with FDIP on. Only 64 has been run with FDIP off.

#### Why `mlp_instr_starve 2`

- **It adjusts to how much the frontend already hides.**
  - With FDIP on, most candidate lines are late prefetches. Fetch waits only for the tail of the miss, which buffered ops often cover. Starve 2 drops a third of those lines, and that frees L2 space for data: L2 instruction misses rise 9%, but stall cost on data falls more.
  - With FDIP off, almost every miss starves the backend. Starve 2 then marks nearly everything mark 2 does and ties it.
- **Fixed thresholds lose in at least one setting.** That includes a higher minimum, a ratio, and a stricter idleness test. With FDIP off, every step stricter loses more.
- **The resteer filter matters in both settings.** With FDIP off, the old ratio mode marks the most lines but trails mark 2 by 0.58%. The likely cause is that it protects wrong-path lines.
- **The ROB-head check is the right scope.** Requiring no outstanding load anywhere drops lines that still starved the backend, since a load miss behind the head does not block retirement.

### Data marking

**Setup:** `golden_small`, top 6 simpoints, the recommended instruction boost (γi 64) and fill-counted γ in every row, data marking changed at the L2 only, 0 simpoints dropped.

- Google: 12 workloads (63 simpoints), 10M warmup + 50M measured instructions.
- SPEC2006: 55 workloads (294 simpoints). SPEC2017: 36 (168). Geekbench: 13 (78). Datacenter: 7 (36). All at 10M warmup + 10M measured.

Each number is the geomean IPC gain over L2 LRU. The last column is the geomean of the five suite gains, suites weighted equally. The marked share of load fills for the default is 34% (Google), 37% (SPEC2006), 21% (SPEC2017), 62% (Geekbench) and 53% (Datacenter). "ROB head" rows count every cycle the ROB head was a load waiting on the request and nothing retired (`--mlp_data_bound 2`); "no-issue" rows count only those cycles in which no op issued either (`--mlp_data_bound 3`).

| Data marking, γd 16 | Google | SPEC2006 | SPEC2017 | Geekbench | Datacenter | 5 suites |
|---|---|---|---|---|---|---|
| **Default: no-issue ≥ 16** | **+2.57** | **+0.39** | **+0.12** | **−0.06** | **+6.19** | **+1.81** |
| No-issue ≥ 8 | +2.52 | +0.41 | +0.10 | −0.03 | +6.20 | +1.81 |
| No-issue ≥ 32 | +2.53 | +0.27 | +0.12 | −0.07 | +6.15 | +1.77 |
| ROB head ≥ 16 (previous default) | +2.64 | +0.36 | +0.12 | −0.04 | +6.19 | +1.83 |
| ROB head ≥ 32 | +2.64 | +0.35 | +0.14 | −0.05 | +6.18 | +1.82 |
| ROB head ≥ 64 | +2.53 | +0.25 | +0.23 | −0.07 | +6.13 | +1.79 |
| ROB head ≥ 1 (1 bit) | +2.40 | +0.26 | +0.08 | −0.03 | +6.22 | +1.76 |
| Old ratio > 0.7 | +2.31 | +0.18 | −0.02 | −0.06 | +6.28 | +1.71 |
| fu_wait cycles ≥ 32 (`--mlp_data_bound 1`) | +2.21 | +0.33 | −0.11 | −0.05 | +6.33 | +1.71 |
| fu_wait load-cycles ≥ 128 (`--mlp_data_bound 0`) | +1.89 | +0.21 | +0.20 | −0.09 | +6.31 | +1.68 |
| Every fill a load waited on (`--mlp_data_bound_min 0`) | +2.31 | −0.10 | −0.15 | −0.04 | +6.28 | +1.63 |
| No data boost (γd 0) | +2.07 | 0.00 | +0.01 | +0.02 | +5.93 | +1.58 |
| ROB head ≥ 16, γd 64 | +1.55 | +0.18 | −0.11 | −0.16 | +6.65 | +1.58 |

The ratio row at γi 64, γd 16 is the best point of the 6×6 γi × γd sweep on `golden_small`. The ratio, γd 0 and LRU numbers come from that sweep (main f1fce3c). On smoke runs (sierra.a.4, sierra.a.6, sphinx3), this code with the old defaults matched it bit for bit. The no-issue rows come from a later sweep with the same setup (scarab 5fb642f).

#### Why no-issue cycles

- **It counts what the miss exposes.** A cycle in which retirement is blocked but the core still issues work is one where the miss is partly hidden. Counting only cycles with nothing issued is closer to the latency the miss actually costs, and to the top-down definition of memory-bound (execution stalled while a miss is outstanding).
- **It is chosen for that, not for its score.** Overall it is within noise of the ROB-head count (+1.81 vs +1.83). No-issue ≥ 16 and ROB head ≥ 32 mark the same share of load fills in every suite. At that matched share, no-issue is ahead on SPEC2006 (+0.39 vs +0.35), behind on Google (+2.57 vs +2.64), and within 0.02 on the other three.
- **It costs the same hardware.** The counter only also checks that the issue stage sent nothing that cycle.
- **Stricter idleness tests add nothing.** Counting only cycles in which dispatch was also blocked by a full ROB, load/store queue, issue queue or register file scored +1.78 at ≥ 16 (5fb642f, not in this code). The 512-entry ROB itself almost never fills first.

#### Why a ROB-head count

- **It charges each stall to one miss.** A cycle counts only for the request the ROB head is waiting on. The ratio and the fu_wait counts (`mlp_data_bound` 0 and 1) charge every miss in flight during a backend-bound cycle, so misses that overlap the one blocking retirement get marked too.
- **The threshold is flat.** No-issue minimums of 8 and 16 tie (+1.81), and 32 is +1.77. ROB-head minimums of 16, 32 and 64 are within 0.04 of each other, and even 1 (a single bit per MSHR) beats the ratio.
- **The fu_wait counts cannot replace the ratio.** Their best settings tie or trail it, and they lose on Google.
- **Where it gains and loses.** With ROB head ≥ 16, the biggest gains over the ratio are sphinx3 (+21.2 vs +13.3), mcf (+6.3 vs +3.1) and xalancbmk (+1.5 vs +0.8). The biggest losses are milc (−1.9 vs −1.5) and cactusADM (−1.9 vs −1.2). milc, leslie3d and cactusADM lose under every count tried.
- **γd 16, not 64.** γd 64 is best on datacenter (+6.65), but it is worse on the other four suites (Google +1.55).

### Replacement mode

- **Fill-counted vs stack position** (#20, older threshold marking, instruction boost only, best γ or λ of each):

  | Suite | Stack position | Fill-counted |
  |---|---|---|
  | Google | +1.44 | +1.57 |
  | Datacenter | +8.52 | +8.52 |

- **Countdown vs fill-counted:** at γ64 they tied, +1.77 vs +1.75 (`mlp_instr_mark 2` alone, FDIP on, Google top 2 simpoints).
- **Countdown variants** (#34; Google, 12 workloads, top 3 simpoints, γi 64, γd 16, with the ratio data marking that was the default then):

  | Variant | Gain over LRU |
  |---|---|
  | Countdown | +2.48 |
  | Countdown, `tiered` | +2.51 |
  | Countdown, `no_revive` | +2.05 |
  | Countdown, `tiered` and `no_revive` | +2.08 |

  `no_revive` loses because a hit on a lapsed line no longer protects it again. A longer γ might win the loss back, but that has not been run.

Fill-counted γ stays the recommendation because the countdown has not been compared with it under the current marking defaults.

## Not recommended yet

- **DRRIP as the base (`--mlp_rrip_boost`)**
  - On Google, DRRIP alone is −0.95 vs LRU.
  - DRRIP with instruction credits of 16 (`--mlp_rrip_boost 2`) got +1.36 (`mlp_instr_mark 2` alone, top 2 simpoints), against +1.75 for LRU with γ64.
  - It was still rising at 16, and larger credits have not been run.

## Open

- γ above 64 with FDIP off.
- The countdown against fill-counted γ under the current marking defaults, and with FDIP off.
- `no_revive` with a longer γ.
- `mlp_instr_starve 2` on suites other than Google. Datacenter instruction boost was +8.52 with the old threshold marking.
- No-issue minimums other than 8, 16 and 32, and ROB-head minimums other than 1, 16, 32 and 64.
- Data marking on `golden_small_incl`, where the γ sweep's best with the ratio was γi 256, γd 64.
- The ROB-head data marking with DRRIP as the base.
- REPL_MLP at the last-level cache.
