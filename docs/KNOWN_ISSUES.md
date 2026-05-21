# Known issues and caveats

A short list of practical issues we encountered during the experimental
campaign, useful to anyone trying to reproduce or extend the work.

## 1. Spectrum overlap / DC leakage on USRP B210

When the two cells are configured on **contiguous** carriers (SSB
separation < 8 MHz), the USRP B210 zero-IF front-end injects spurious
tones around the local oscillator that leak into the adjacent carrier,
causing a ~10 dB uplink SNR degradation at the source gNB and spurious
RACH detections at the target gNB.

**Workaround**: use the SSB separation of 2.88 MHz reported in
`conf/gnb_b210*.conf` (84% spectral overlap of the 18.36 MHz occupied
bandwidths). Less is bad; more (>19 MHz) breaks the handover for an
unrelated reason (CFRA timing — see below).

## 2. COTS UE falls back to CBRA instead of CFRA

The Samsung / Pixel UE sometimes performs a Contention-Based RACH at the
target instead of the Contention-Free RACH expected after the
`reconfigurationWithSync`. This happens when the RRCReconfiguration
message is delivered too late on the source SRB and the UE has already
gone out-of-sync.

**Root cause**: race condition between the source-cell scheduler
out-of-sync timer and the SRB1 dispatch of the HandoverCommand. Patched
in `patches/oai-n2-handover-fixes.patch`.

## 3. KPM `RSRP` field stuck at -120 dBm

In our OAI build the per-UE KPM RSRP exporter reports a sentinel value of
-120 dBm until the first valid measurement is computed. We work around
this in the xApp by **overriding** the KPM RSRP with the RRC-reported
value coming from the UE Measurement Report (Format 1).

## 4. SINR saturation around 22.5 dB indoor

When the UE is close to the serving gNB, the OAI SINR estimator saturates
at ~22.5 dB. This is one of the reasons why we chose to weight throughput
more heavily than radio metrics in the composite score.

## 5. CSV writer is synchronous

The xApp logger calls `fflush()` after every KPM indication to preserve
traces in case of crash; for production deployments this should be made
asynchronous or replaced with the InfluxDB backend natively supported by
FlexRIC.

## 6. Multi-metric xApp logs only the serving cell

In the current code the multi-metric xApp logs only the cell currently
serving the UE. Statistics computed in the paper from these traces
required selecting trials in which the same xApp also captured the
target-cell telemetry. Logging both source and target unconditionally is
on the future-work list.
