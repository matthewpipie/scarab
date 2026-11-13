# REPL_MLP: recommended settings

REPL_MLP (`--mlc_cache_repl_policy 9`) is L2 LRU plus protection for lines whose misses stall the core. This page gives the settings that worked best and the runs behind them. Each parameter is documented in `src/memory/memory.param.def`.

## Recommended

```
--mlc_cache_repl_policy 9 --mlp_boost_in_fills 1 --mlp_lin_instr_lambda 64
```

| Parameter | Value | Default? | What it does |
|---|---|---|---|
| `mlc_cache_repl_policy` | 9 | no (0, LRU) | REPL_MLP at the L2 |
| `mlp_boost_in_fills` | 1 | no (0) | Protects a marked line for γ fills into its set after its last touch (fill-counted γ). With 0, the default countdown is used instead (see below). |
| `mlp_lin_instr_lambda` | 64 | no (0, off) | γ for instruction lines. 64 and 128 are about equally good with FDIP on. Only 64 has been run with FDIP off. |
| `mlp_instr_mark` | 2 | yes | Marks only fills a demand fetch reached, and drops those fetch was resteered away from while it waited on them |
| `mlp_instr_starve` | 2 | yes | Counts a waiting cycle only if the backend was also starved and the ROB head was not a load waiting on memory |
| `mlp_instr_starve_min` | 1 | yes | At least one such cycle |
| `mlp_lin_data_lambda` | 0 | yes | No data-side boost |

### Which lines get marked

A line is marked at its L2 fill if at least one earlier cycle met all of these at once:

- **Fetch was stuck on this line.** Its request was still outstanding, fetch and the uop cache delivered no ops, and the icache stage stopped on the miss.
- **The backend was starved.** Rename passed no ops to the ROB, the ROB was not full, and rename was not stalled on registers.
- **The ROB head was not a load waiting on memory.**

In addition, a demand fetch must have reached the request, and fetch must not have been resteered away from it while waiting (unless a demand fetch reached it again before the fill).

- The mark is decided once and stays with the line until eviction. A hit restarts the protection window, but it does not re-decide the mark.
- The conditions only have to coincide. Nothing checks that this miss caused the starvation.
- Every input is something the core can see. None of it reads `off_path`.

### Cheaper hardware variant (the default)

Without `--mlp_boost_in_fills 1`, REPL_MLP uses the countdown (`mlp_boost_countdown`, default 1). It keeps a 2-bit countdown per line and a tick counter per set. At γ64 it tied fill-counted γ: +1.77 vs +1.75 (mark 2 alone, FDIP on, Google top 2 simpoints). It has not been run with `mlp_instr_starve 2` or with FDIP off.

`mlp_boost_countdown_tiered` (default 1) changes the victim only when every line in the set is protected: it evicts the line with the fewest ticks left, instead of the least recently used. Over plain countdown it was +2.51 vs +2.48 (Google, 12 workloads, top 3 simpoints, γi64, γd16, #34).

### Pitfall

The stack-position mode (λ) needs both `--mlp_boost_countdown 0` and no `--mlp_boost_in_fills 1`.

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

## Not recommended yet

- **Data-side boost (`mlp_lin_data_lambda`)**
  - SPEC2006: its best is +0.13 (γ16), or +0.29 with #25's FU-wait condition. Plain DRRIP gets +1.0.
  - SPEC2017: it loses at every fixed setting tested.
  - Datacenter: it gained +1.89 (γ64), but it has not been run together with the instruction boost.
- **DRRIP as the base (`--mlp_rrip_boost`)**
  - On Google, DRRIP alone is −0.95 vs LRU.
  - DRRIP with instruction credits of 16 got +1.36 (mark 2 alone, top 2 simpoints), against +1.75 for LRU with γ64.
  - It was still rising at 16, and larger credits have not been run.

## Open

- γ above 64 with FDIP off.
- The countdown variant with `mlp_instr_starve 2`, and with FDIP off.
- `mlp_instr_starve 2` on suites other than Google. Datacenter instruction boost was +8.52 with the old threshold marking.
