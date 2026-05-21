# Reproduction guide

End-to-end steps to reproduce the experimental results of the paper.

## 1. Hardware

- 1x x86 host (Ubuntu 22.04, ≥16 GB RAM, USB 3.0)
- 2x USRP B210 SDRs (one acts as **source** gNB, the other as **target**)
- 1x COTS 5G UE (Google Pixel 8a in our setup) with an OpenCell SIM card
  programmed with the IMSI/key defined in `conf/oai_db*.sql`

## 2. Software

| Component         | Upstream                                                      | Commit / branch used in the paper |
|-------------------|---------------------------------------------------------------|-----------------------------------|
| OAI CN5G          | https://gitlab.eurecom.fr/oai/cn5g/oai-cn5g-fed              | `develop` @ Mar 2026              |
| OAI 5G RAN        | https://gitlab.eurecom.fr/oai/openairinterface5g             | `develop` @ Mar 2026              |
| FlexRIC           | https://gitlab.eurecom.fr/mosaic5g/flexric                   | `dev`     @ Mar 2026              |

Public forks pinned to the exact commits used in the paper:

- OAI: `https://github.com/alevic44/openairinterface5g` (branch `multimetric-ho`)
- FlexRIC: `https://github.com/alevic44/flexric` (branch `multimetric-ho`)

## 3. Apply the patches (only if not using our forks)

```bash
cd openairinterface5g
git apply /path/to/patches/oai-rc-style3-extension.patch
git apply /path/to/patches/oai-kpm-sinr-fix.patch
git apply /path/to/patches/oai-n2-handover-fixes.patch

cd ../flexric
git apply /path/to/patches/flexric-rc-style3.patch
```

## 4. Build

```bash
# OAI CN5G
cd oai-cn5g-fed/docker-compose && docker compose -f docker-compose-basic-nrf.yaml up -d

# OAI 5G RAN (gNB)
cd openairinterface5g/cmake_targets
./build_oai --gNB --ninja -w USRP

# FlexRIC
cd flexric && mkdir -p build && cd build
cmake .. -DXAPP_C_INSTALL=ON && make -j$(nproc)
```

## 5. Install the xApp

```bash
cp xapp/xapp_ho_multimetric.c                   flexric/examples/xApp/c/advanced_handover/
cp xapp/xapp_advanced_handover_simple.c         flexric/examples/xApp/c/advanced_handover/
cp xapp/xapp_advanced_handover_timer.c          flexric/examples/xApp/c/advanced_handover/
cp xapp/CMakeLists.txt                          flexric/examples/xApp/c/advanced_handover/

cd flexric/build && cmake .. && make xapp_ho_multimetric -j$(nproc)
```

## 6. Launch a handover trial

In four separate terminals:

```bash
# (1) 5G Core
cd oai-cn5g-fed/docker-compose
docker compose -f docker-compose-basic-nrf.yaml up

# (2) gNB source
sudo ./openairinterface5g/cmake_targets/ran_build/build/nr-softmodem \
     -O conf/gnb_b210.conf --sa --E2-agent

# (3) gNB target
sudo ./openairinterface5g/cmake_targets/ran_build/build/nr-softmodem \
     -O conf/gnb_b210bis.conf --sa --E2-agent

# (4) Multi-metric xApp
cd flexric/build/examples/xApp/c/advanced_handover
./xapp_ho_multimetric --threshold 0.30 --w-thp 0.60 --w-rsrp 0.20 \
                      --w-rsrq 0.20 --w-prb 0.10
```

Power on the UE; once attached and with iperf3 running, the xApp will
trigger an N2 handover when the composite score drops below 0.30 for 3
consecutive samples. The KPM/RRC trace is logged to a CSV file named
`ho_multi_<timestamp>.csv`.

## 7. Analyze a trial

```bash
python3 scripts/analyze_metrics.py ho_multi_<timestamp>.csv
```

For batch aggregation across multiple trials (as in Table IV of the paper)
see `data/trials/README.md`.
