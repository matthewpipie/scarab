# REPL_MLP: recommended settings

REPL_MLP (`--mlc_cache_repl_policy 9`) is L2 LRU plus protection for lines whose misses stall the core. This page gives the settings that worked best and the runs behind them. Each parameter is documented in `src/memory/memory.param.def`.

## Recommended

```
--mlc_cache_repl_policy 9 --mlp_boost_in_fills 1 --mlp_lin_instr_lambda 64 --mlp_lin_data_lambda 16
```

| Parameter | Value | Default? | What it does |
|---|---|---|---|
| `mlc_cache_repl_policy` | 9 | no (0, LRU) | REPL_MLP at the L2 |
| `mlp_boost_in_fills` | 1 | no (0) | Protects a marked line for γ fills into its set after its last touch (fill-counted γ). With 0, the boost is added to the line's LRU stack position instead (λ). |
| `mlp_lin_instr_lambda` | 64 | no (0, off) | γ for instruction lines. 64 and 128 are about equally good with FDIP on. Only 64 has been run with FDIP off. |
| `mlp_instr_mark` | 2 | yes | Marks only fills a demand fetch reached, and drops those fetch was resteered away from while it waited on them |
| `mlp_instr_starve` | 2 | yes | Counts a waiting cycle only if the backend was also starved and the ROB head was not a load waiting on memory |
| `mlp_instr_starve_min` | 1 | yes | At least one such cycle |
| `mlp_lin_data_lambda` | 16 | no (0, off) | γ for data lines. 64 is better on datacenter but worse on every other suite. |
| `mlp_data_bound` | 3 | yes | Counts, per request, the cycles nothing retired because the ROB head was a load waiting on it, and no op issued to the functional units |
| `mlp_data_bound_min` | 16 | yes | At least 16 such cycles |
| `mlp_lin_data_thresh` | 0 | yes | No ratio test. 0.7, together with `--mlp_data_bound_min 0`, gives the old ratio marking. |

### Which lines get marked

**Data lines.** A line is marked at its L2 fill if, between a load merging into its request and the fill, there were at least 16 cycles in which the ROB head was a load waiting on that request, nothing retired, and no op issued to the functional units. Hardware needs a 5-bit saturating counter per MSHR, bumped from the ROB head in cycles the issue stage sent nothing. Every input is something the core can see.

**Instruction lines.** A line is marked at its L2 fill if at least one earlier cycle met all of these at once:

- **Fetch was stuck on this line.** Its request was still outstanding, fetch and the uop cache delivered no ops, and the icache stage stopped on the miss.
- **The backend was starved.** Rename passed no ops to the ROB, the ROB was not full, and rename was not stalled on registers.
- **The ROB head was not a load waiting on memory.**

In addition, a demand fetch must have reached the request, and fetch must not have been resteered away from it while waiting (unless a demand fetch reached it again before the fill).

- The mark is decided once and stays with the line until eviction. A hit restarts the protection window, but it does not re-decide the mark.
- The conditions only have to coincide. Nothing checks that this miss caused the starvation.
- Every input is something the core can see. None of it reads `off_path`.

### Cheaper hardware variant

`--mlp_boost_countdown 1` in place of `--mlp_boost_in_fills 1` keeps a 2-bit countdown per line and a tick counter per set. At γ64 it tied fill-counted γ: +1.77 vs +1.75 (mark 2 alone, FDIP on, Google top 2 simpoints). It has not been run with `mlp_instr_starve 2` or with FDIP off.

### Pitfall

Setting `mlp_lin_instr_lambda` without `--mlp_boost_in_fills 1` gives the stack-position mode.

| Suite | Stack-position λ (best) | Fill-counted γ (best) |
|---|---|---|
| Google, instruction boost | +1.44 | +1.57 |
| Datacenter, instruction boost | +8.52 | +8.52 |

These numbers are from #20, which used the older threshold marking.

## Results

**Setup:** Google, 12 workloads, top 6 simpoints (63), 10M warmup + 50M measured instructions, `golden_small`, policy changed at the L2 only, instruction boost only. Each number is the geomean IPC gain over L2 LRU with the same FDIP setting. With FDIP off (`--fdip_enable 0`), LRU's IPC is 28.6% lower than with FDIP on.

| Marking | FDIP on, γ64 | FDIP on, γ128 | FDIP off, γ64 |
|---|---|---|---|
| **Recommended: `mlp_instr_starve 2`, min 1** | **+1.99** | **+1.95** | **+13.25** |
| `mlp_instr_mark 2` alone (`--mlp_instr_starve 0`) | +1.75 | +1.15 | +13.37 |
| `mlp_instr_starve 1`: starved, any ROB head | +1.93 | +1.53 | |
| `mlp_instr_starve 3`: issue queue empty | +1.72 | +1.88 | |
| `mlp_instr_starve 4`: ROB empty | +1.62 | +1.79 | |
| `mlp_instr_starve 5`: no FUs busy | +1.80 | +1.24 | |
| Starved, and no load waiting on memory anywhere (not merged) | +1.81 | +1.92 | +12.70 |
| `mlp_instr_starve_min` 4 / 16 / 64 | | | +13.09 / +12.31 / +10.38 |
| Old ratio (`--mlp_instr_mark 0 --mlp_instr_starve 0`) | | | +12.71 |
| Ratio over starved cycles (`--mlp_instr_mark 0`) | | | +9.80 |

### Why `mlp_instr_starve 2`

- **It adjusts to how much the frontend already hides.**
  - With FDIP on, most candidate lines are late prefetches. Fetch waits only for the tail of the miss, which buffered ops often cover. Starve 2 drops a third of those lines, and that frees L2 space for data: L2 instruction misses rise 9%, but stall cost on data falls more.
  - With FDIP off, almost every miss starves the backend. Starve 2 then marks nearly everything mark 2 does and ties it.
- **Fixed thresholds lose in at least one setting.** That includes a higher minimum, a ratio, and a stricter idleness test. With FDIP off, every step stricter loses more.
- **The resteer filter matters in both settings.** With FDIP off, the old ratio mode marks the most lines but trails mark 2 by 0.58%. The likely cause is that it protects wrong-path lines.
- **The ROB-head check is the right scope.** Requiring no outstanding load anywhere drops lines that still starved the backend, since a load miss behind the head does not block retirement.

### Data marking

**Setup:** `golden_small`, top 6 simpoints, the recommended instruction boost (γi 64) in every row, data marking changed at the L2 only, 0 simpoints dropped.

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
| fu_wait cycles per request ≥ 32 (`--mlp_data_bound 1`) | +2.21 | +0.33 | −0.11 | −0.05 | +6.33 | +1.71 |
| fu_wait load-cycles ≥ 128 (`--mlp_data_bound 0`) | +1.89 | +0.21 | +0.20 | −0.09 | +6.31 | +1.68 |
| Every fill a load waited on (`--mlp_data_bound_min 0`) | +2.31 | −0.10 | −0.15 | −0.04 | +6.28 | +1.63 |
| No data boost (γd 0) | +2.07 | 0.00 | +0.01 | +0.02 | +5.93 | +1.58 |
| ROB head ≥ 16, γd 64 | +1.55 | +0.18 | −0.11 | −0.16 | +6.65 | +1.58 |

The ratio row at γi 64, γd 16 is the best point of the 6×6 γi × γd sweep on `golden_small`. The ratio, γd 0 and LRU numbers come from that sweep (main f1fce3c). On smoke runs (sierra.a.4, sierra.a.6, sphinx3), this code with the old defaults matched it bit for bit. The no-issue rows come from a later sweep with the same setup (scarab 5fb642f).

### Why no-issue cycles

- **It counts what the miss exposes.** A cycle in which retirement is blocked but the core still issues work is one where the miss is partly hidden. Counting only cycles with nothing issued is closer to the latency the miss actually costs, and to the top-down definition of memory-bound (execution stalled while a miss is outstanding).
- **It is chosen for that, not for its score.** Overall it is within noise of the ROB-head count (+1.81 vs +1.83). No-issue ≥ 16 and ROB head ≥ 32 mark the same share of load fills in every suite. At that matched share, no-issue is ahead on SPEC2006 (+0.39 vs +0.35), behind on Google (+2.57 vs +2.64), and within 0.02 on the other three.
- **It costs the same hardware.** The counter only also checks that the issue stage sent nothing that cycle.
- **Stricter idleness tests add nothing.** Counting only cycles in which dispatch was also blocked by a full ROB, load/store queue, issue queue or register file scored +1.78 at ≥ 16 (5fb642f, not in this code). The 512-entry ROB itself almost never fills first.

### Why a ROB-head count

- **It charges each stall to one miss.** A cycle counts only for the request the ROB head is waiting on. The ratio and the fu_wait counts (`mlp_data_bound` 0 and 1) charge every miss in flight during a backend-bound cycle, so misses that overlap the one blocking retirement get marked too.
- **The threshold is flat.** No-issue minimums of 8 and 16 tie (+1.81), and 32 is +1.77. ROB-head minimums of 16, 32 and 64 are within 0.04 of each other, and even 1 (a single bit per MSHR) beats the ratio.
- **The fu_wait counts cannot replace the ratio.** Their best settings tie or trail it, and they lose on Google.
- **Where it gains and loses.** With ROB head ≥ 16, the biggest gains over the ratio are sphinx3 (+21.2 vs +13.3), mcf (+6.3 vs +3.1) and xalancbmk (+1.5 vs +0.8). The biggest losses are milc (−1.9 vs −1.5) and cactusADM (−1.9 vs −1.2). milc, leslie3d and cactusADM lose under every count tried.
- **γd 16, not 64.** γd 64 is best on datacenter (+6.65), but it is worse on the other four suites (Google +1.55).

## Not recommended yet

- **DRRIP as the base (`--mlp_rrip_boost`)**
  - On Google, DRRIP alone is −0.95 vs LRU.
  - DRRIP with instruction credits of 16 got +1.36 (mark 2 alone, top 2 simpoints), against +1.75 for LRU with γ64.
  - It was still rising at 16, and larger credits have not been run.

## Open

- γ above 64 with FDIP off.
- The countdown variant with `mlp_instr_starve 2`, and with FDIP off.
- `mlp_instr_starve 2` on suites other than Google. Datacenter instruction boost was +8.52 with the old threshold marking.
- No-issue minimums other than 8, 16 and 32, and ROB-head minimums other than 1, 16, 32 and 64.
- Data marking on `golden_small_incl`, where the γ sweep's best with the ratio was γi 256, γd 64.
- The data boost with the countdown variant, or with DRRIP as the base.
