# Handover trial dataset (MAIN 2026 paper)

9 CSV files, each corresponding to one independent N2 handover trial on the
OAI + FlexRIC testbed described in the paper. All trials were collected on
March 26-27, 2026 using the baseline throughput-based xApp (Algorithm 1).

## CSV format (long)

| column         | description                                |
|----------------|--------------------------------------------|
| `timestamp`    | UTC date-time with millisecond resolution  |
| `gnb`          | `source` or `target`                       |
| `metric_name`  | KPM/RRC metric name (e.g. `DRB.UEThpDl`)   |
| `metric_value` | numerical value (units per metric)         |

## Reported metrics

- `DRB.UEThpDl`, `DRB.UEThpUl` : per-UE throughput [kbps]
- `DRB.PdcpSduVolumeUL`        : PDCP UL SDU volume
- `DRB.RlcSduDelayDl`          : RLC DL delay [us]
- `RRU.PrbTotDl`, `RRU.PrbTotUl`: PRB usage counters
- `RSRP`, `RSRQ`, `SINR`       : UE-reported radio measurements

## Trial outcomes

See Table~IV in the paper (HIT, pre/post RSRP/RSRQ/SINR, RLC delay).
The aggregate statistics are computed over these 9 trials.
