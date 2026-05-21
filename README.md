# xApp-Driven Multi-Metric Handover for 5G Open RAN

Artifact of the paper:

> A. Vicario, A. Pagano, A. Dino, D. Croce, I. Tinnirello,
> **"xApp-Driven Multi-Metric Handover in 5G Open RAN with COTS User Equipment"**,
> MAIN 2026 — Mediterranean Artificial Intelligence and Networking Conference.

This repository contains the source code of the multi-metric handover xApp,
the patches applied to OpenAirInterface (OAI) and FlexRIC for the
experiments, the gNB/CN configurations of our physical testbed, and the
raw CSV traces of the 14 handover trials reported in the paper.

## Highlights

- **Multi-metric scoring xApp** for N2 inter-frequency handover, integrating
  KPM-reported throughput and PRB utilization with UE-reported RSRP/RSRQ
  (3GPP TS 38.133), with hysteresis and cooldown safeguards.
- **OAI extension** to E2SM-RC **Control Style 3 -- Connected Mode
  Mobility**, allowing xApp-driven HO with target cell selected at runtime
  via NR-CGI.
- **Real-world dataset**: 14 N2 handover trials on a physical testbed with
  two USRP B210 cells and a Google Pixel 8a, collected in March--April
  2026.

## Repository layout

```
xapp/      C source code of the three xApps (baseline, timer, multi-metric)
patches/   git diff patches against upstream OAI and FlexRIC
conf/      gNB and 5G core configuration files used in the experiments
data/      raw CSV traces of the 14 handover trials + README
scripts/   helper Python scripts (CSV analysis, plots)
docs/      architecture diagrams, reproduction guide, known issues
```

## Quick start

1. **Set up upstream dependencies** (tested commits in `docs/REPRODUCE.md`):
   - OAI CN5G — https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-fed
   - OAI 5G RAN — https://gitlab.eurecom.fr/oai/openairinterface5g
   - FlexRIC near-RT RIC — https://gitlab.eurecom.fr/mosaic5g/flexric
2. **Apply the patches**:
   ```bash
   cd openairinterface5g && git apply /path/to/patches/oai-*.patch
   cd flexric             && git apply /path/to/patches/flexric-*.patch
   ```
3. **Drop the xApp into FlexRIC**:
   ```bash
   cp xapp/*.c xapp/CMakeLists.txt flexric/examples/xApp/c/advanced_handover/
   cd flexric/build && cmake .. && make xapp_ho_multimetric -j$(nproc)
   ```
4. **Copy the gNB/CN configs** from `conf/` into the corresponding paths
   of your OAI deployment.
5. **Run**: see `docs/REPRODUCE.md`.

## Dataset

`data/trials/` contains 14 CSV files (9 baseline xApp + 5 multi-metric
xApp), each describing one N2 handover trial with per-100\,ms KPM/RRC
telemetry from both the source and the target gNB. See
`data/trials/README.md` for the schema and the aggregated outcomes
(Table IV in the paper).

## Citation

If you use this artifact, please cite:

```bibtex
@inproceedings{vicario2026xapp,
  title     = {xApp-Driven Multi-Metric Handover in 5G Open RAN with COTS User Equipment},
  author    = {Vicario, Alessio and Pagano, Antonino and Dino, Alessandra and Croce, Daniele and Tinnirello, Ilenia},
  booktitle = {2026 Mediterranean Artificial Intelligence and Networking Conference (MAIN)},
  year      = {2026}
}
```

## License

MIT (see `LICENSE`). The xApp links against FlexRIC, which is released
under the BSD-3-Clause license; please refer to FlexRIC and OAI upstream
licenses for components covered by them.

## Contact

Alessio Vicario — `alessio.vicario@unipa.it` — University of Palermo
