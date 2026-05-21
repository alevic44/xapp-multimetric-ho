# xapp_advanced_handover_timer

xApp per handover N2 tra due gNB con trigger basato esclusivamente su un timer fisso.
Utile come **baseline deterministico** per confrontare i risultati con le xApp a soglie o scoring.

## Funzionamento

1. Si connette al nearRT-RIC tramite FlexRIC
2. Sottoscrive KPM su tutti i gNB connessi
3. Al primo report KPM ricevuto, avvia il timer
4. Esattamente **7 secondi** dopo il primo report, invia il comando HO
5. Il comando viene inviato **una sola volta** per tutta la durata della xApp
6. Le metriche radio vengono collezionate ma **non influenzano il trigger**

## Flags

Nessun flag custom: accetta solo gli argomenti standard di FlexRIC.

```
./xapp_advanced_handover_timer -c /path/to/flexric.conf
```

## Esempio

```bash
./xapp_advanced_handover_timer -c /usr/local/etc/flexric/flexric.conf
```

## Configurazione hardware

| Parametro | Valore |
|-----------|--------|
| Source gNB | B210, Global ID=0x12345, nr_cellid=12345678 |
| Target gNB | N310, Global ID=0x002, nr_cellid=12345679 |
| Delay HO | 7 secondi dal primo report KPM |

## Differenze rispetto alle altre xApp

| Aspetto | timer | simple | multimetric |
|---------|-------|--------|-------------|
| Trigger | Timer fisso 7s | Soglia throughput | Scoring pesato 5 metriche |
| Deterministico | Sì | No | No |
| Anti-ping-pong | N/A (un solo HO) | No | Sì |
| Flag `--label`/`--comment` | No | Sì | No |
| Flag `--ho-timer` | N/A (hardcoded 7s) | Sì | No |
| CSV logging | No | Sì | Sì |
| Uso | Baseline | Test di campo | Ricerca |
