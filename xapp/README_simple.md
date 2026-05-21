# xapp_advanced_handover_simple

xApp per handover N2 tra due gNB basata su soglie di throughput e metriche radio.

## Funzionamento

1. Si connette al nearRT-RIC tramite FlexRIC
2. Sottoscrive KPM su tutti i gNB connessi (periodo 100 ms)
3. Sottoscrive RC per ricevere RSRP/RSRQ/SINR reali via SSB CSI path
4. Per ogni UE monitorato, logga tutte le metriche in CSV
5. Triggerare l'HO quando throughput DL+UL < 100 kbps oppure tramite `--ho-timer`
6. Il comando HO viene inviato una sola volta; dopo 30s la xApp termina

## Logica di trigger

```
(DL_thp + UL_thp) < 100 kbps  →  trigger HO
```

oppure, se `--ho-timer N` è specificato:

```
N secondi dopo il primo report KPM  →  trigger HO (ignora le soglie)
```

## Flags

```
./xapp_advanced_handover_simple [--ho-timer N] [--label NOME] [--comment "TESTO"] [args FlexRIC...]
```

| Flag | Argomento | Default | Descrizione |
|------|-----------|---------|-------------|
| `--ho-timer` | N (secondi) | disabilitato | Forza HO dopo N secondi ignorando la logica a soglie |
| `--label` | stringa | `default` | Inserita nel nome del file CSV (es. `load1mbps`) |
| `--comment` | stringa | vuoto | Scritto come `# commento` nella prima riga del CSV |

Tutti gli altri argomenti vengono passati direttamente a FlexRIC (`init_fr_args`), ad esempio `-c /path/to/flexric.conf`.

## Esempi

```bash
# Avvio base
./xapp_advanced_handover_simple -c /usr/local/etc/flexric/flexric.conf

# Con label e commento per distinguere il run
./xapp_advanced_handover_simple \
  --label "load1mbps" \
  --comment "UE vicino source, movimento da sinistra a destra" \
  -c /usr/local/etc/flexric/flexric.conf

# HO forzato dopo 15 secondi (baseline timer)
./xapp_advanced_handover_simple \
  --ho-timer 15 \
  --label "timer_15s" \
  -c /usr/local/etc/flexric/flexric.conf
```

## Output CSV

Il file viene creato nella directory corrente con il pattern:

```
handover_trigger_metrics_YYYYMMDD_HHMMSS_{label}_{ms}.csv
```

### Formato

```
# <commento>          ← presente solo se --comment è specificato
timestamp,gnb,metric_name,metric_value
2026-03-23 18:17:50.299,source,DRB.UEThpDl,8378.480
2026-03-23 18:17:50.299,source,RSRP,-95.0
...
2026-03-23 18:18:05.100,target,DRB.UEThpDl,12000.000
```

Il campo `gnb` vale `source`, `target`, oppure `unknown` se il gNB non è ancora stato classificato.

### Metriche raccolte

| Metrica | Via | Unità |
|---------|-----|-------|
| `DRB.UEThpDl` | KPM | kbps |
| `DRB.UEThpUl` | KPM | kbps |
| `DRB.RlcSduDelayDl` | KPM | ms |
| `DRB.PdcpSduVolumeUL` | KPM | kbyte |
| `RSRP` | RC (SSB CSI) | dBm |
| `RSRQ` | RC (SSB CSI) | dB |
| `SINR` | RC (SSB CSI, dopo fix SINR_index) | dB |

## Configurazione hardware

| Parametro | Valore |
|-----------|--------|
| Source gNB | B210, Global ID=0x12345, nr_cellid=12345678 |
| Target gNB | N310, Global ID=0x002, nr_cellid=12345679 |
| Soglia throughput | 100 kbps (DL+UL) |
| Reset anti-HO | 30 secondi |
| Warm-up | 10 secondi dall'avvio (HO bloccato) |
