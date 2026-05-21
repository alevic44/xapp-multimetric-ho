# xapp_ho_multimetric

xApp per handover N2 tra due gNB con algoritmo di scoring pesato multi-metrica e anti-ping-pong.

## Funzionamento

1. Si connette al nearRT-RIC tramite FlexRIC
2. Sottoscrive KPM su tutti i gNB connessi (periodo 100 ms)
3. Raccoglie RSRP, SINR, CQI, throughput DL/UL, PRB usage
4. Calcola uno **score normalizzato [0,1]** ad ogni campione
5. Triggerare l'HO quando lo score scende sotto soglia per N campioni consecutivi
6. Permette trigger manuale premendo `h` + Enter

## Algoritmo di scoring

Ogni metrica viene normalizzata nel proprio range fisico, poi combinata con pesi:

```
score = w_rsrp * norm(RSRP) + w_sinr * norm(SINR) + w_cqi * norm(CQI)
      + w_thp  * norm(Thp)  + w_prb  * norm(PRB)
```

### Pesi configurati

| Metrica | Peso | Range di normalizzazione |
|---------|------|--------------------------|
| RSRP | 0.35 | [-140, -44] dBm |
| SINR | 0.30 | [-32, +31.5] dB (TS 38.133) |
| CQI  | 0.15 | [0, 15] |
| Thp (DL+UL) | 0.15 | [0, 100000] kbps |
| PRB  | 0.05 | [0, 100] |

### Parametri anti-ping-pong

| Parametro | Valore |
|-----------|--------|
| `SCORE_THRESHOLD` | 0.35 — sotto questa soglia lo score è "basso" |
| `HO_HYSTERESIS_SAMPLES` | 3 — campioni consecutivi sotto soglia richiesti |
| `MIN_HO_INTERVAL_US` | 5 000 000 µs (5 s) — minimo tra due HO successivi |
| Periodo KPM | 100 ms |

## Flags

Nessun flag custom: questa xApp accetta solo gli argomenti standard di FlexRIC.

```
./xapp_ho_multimetric -c /path/to/flexric.conf
```

Per triggera HO manualmente durante l'esecuzione: premere **`h` + Enter**.

## Output CSV

Il file viene creato nella directory corrente con il pattern:

```
ho_multimetric_YYYYMMDD_HHMMSS_{ms}.csv
```

### Formato

```
timestamp,ue_id,rsrp_dBm,sinr_dB,cqi,thp_dl_kbps,thp_ul_kbps,prb_dl,score,ho_triggered
2026-03-23 18:17:50.299,12345,−95.0,22.5,12,8000.0,200.0,30,0.71,0
...
```

### Metriche raccolte

| Metrica | Via | Unità |
|---------|-----|-------|
| RSRP | RC (SSB CSI path) | dBm |
| RSRQ | RC (SSB CSI path) | dB |
| SINR | RC (SSB CSI, fix SINR_index TS 38.133) | dB |
| CQI | KPM | [0–15] |
| `DRB.UEThpDl` | KPM | kbps |
| `DRB.UEThpUl` | KPM | kbps |
| PRB DL | KPM | [0–100] |

## Configurazione hardware

| Parametro | Valore |
|-----------|--------|
| Source gNB | B210, Global ID=0x12345, nr_cellid=12345678 |
| Target gNB | N310, Global ID=0x002, nr_cellid=12345679 |

## Differenze rispetto a xapp_advanced_handover_simple

| Aspetto | simple | multimetric |
|---------|--------|-------------|
| Trigger | Soglia throughput | Scoring pesato 5 metriche |
| Anti-ping-pong | No | Sì (isteresi N=3 + cooldown 5s) |
| Flag `--label`/`--comment` | Sì | No |
| Flag `--ho-timer` | Sì | No (usa `h`+Enter per HO manuale) |
| RSRP/SINR nel CSV | Sì (via RC callback) | Sì (via RC callback) |
