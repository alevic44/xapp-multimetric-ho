#!/usr/bin/env python3
"""
analyze_metrics.py — Analizza CSV delle metriche di handover.
Genera un'immagine PNG separata per ogni metrica.
Colori distinti per source e target gNB.

Uso:
    python3 scripts/analyze_metrics.py <csv_file> --all
    python3 scripts/analyze_metrics.py csv/multi/ho_multi_25_17_35_41.csv --all --output-dir plots
"""

import pandas as pd
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt
import numpy as np
import argparse
import os
from datetime import datetime

# Colori distinti per source e target
COLOR_SOURCE = '#2166AC'   # blu
COLOR_TARGET = '#D6604D'   # rosso-arancio
COLOR_HO     = '#E31A1C'   # rosso per HO event


def create_timestamped_output_dir(base_dir="plots"):
    timestamp = datetime.now().strftime("%Y%m%d_%H%M%S")
    output_dir = f"{base_dir}_{timestamp}"
    os.makedirs(output_dir, exist_ok=True)
    return output_dir


def load_and_process_data(csv_file):
    if not os.path.exists(csv_file):
        print(f"Errore: file {csv_file} non trovato")
        return None

    df = pd.read_csv(csv_file, comment='#')
    df['datetime'] = pd.to_datetime(df['timestamp'])
    df['time_seconds'] = (df['datetime'] - df['datetime'].min()).dt.total_seconds()

    # Detect format: long (metric_name/metric_value) or wide (direct columns)
    if 'metric_name' in df.columns and 'metric_value' in df.columns:
        # Long format (xapp_advanced_handover_simple / timer)
        pivot_df = df.pivot_table(index=['time_seconds', 'gnb'], columns='metric_name',
                                  values='metric_value', aggfunc='first').reset_index()
        metric_mapping = {
            'DRB.UEThpDl': 'thp_dl', 'DRB.UEThpUl': 'thp_ul',
            'DRB.RlcSduDelayDl': 'rlc_delay', 'RRU.PrbTotDl': 'prb_dl',
            'RRU.PrbTotUl': 'prb_ul', 'RSRP': 'rsrp', 'RSRQ': 'rsrq', 'SINR': 'sinr',
        }
        pivot_df = pivot_df.rename(columns=metric_mapping)
        pivot_df['datetime'] = df.groupby(['time_seconds', 'gnb'])['datetime'].first().values
    else:
        # Wide format (xapp_ho_multimetric)
        pivot_df = df.copy()
        # Normalize gnb column name
        if 'gnb_id' in pivot_df.columns and 'gnb' not in pivot_df.columns:
            pivot_df['gnb'] = pivot_df['gnb_id'].map({0: 'source', 1: 'target'})
        elif 'gnb' in pivot_df.columns:
            # Already has gnb as source/target string
            pass

    print(f"Dati caricati: {len(df)} righe, periodo: {df['datetime'].min()} → {df['datetime'].max()}")
    return pivot_df


def _setup_ax(ax, xlabel='Time (s)', ylabel='', title=''):
    ax.set_xlabel(xlabel, fontsize=14)
    ax.set_ylabel(ylabel, fontsize=14)
    if title:
        ax.set_title(title, fontsize=14)
    ax.grid(True, alpha=0.3, linestyle='--')
    ax.tick_params(axis='both', which='major', width=2, length=6, labelsize=12)
    for spine in ax.spines.values():
        spine.set_linewidth(1.5)
    ax.legend(fontsize=12, loc='best')


def _split_source_target(df, col):
    """Split dataframe into source and target based on gnb column."""
    if 'gnb' not in df.columns:
        return df, pd.DataFrame()
    src = df[df['gnb'] == 'source'].dropna(subset=[col]) if col in df.columns else pd.DataFrame()
    tgt = df[df['gnb'] == 'target'].dropna(subset=[col]) if col in df.columns else pd.DataFrame()
    return src, tgt


def _save(fig, output_dir, name):
    path = os.path.join(output_dir, f'{name}.png')
    fig.savefig(path, dpi=300, bbox_inches='tight')
    plt.close(fig)
    print(f"  Salvato: {path}")


def plot_thp_dl(df, output_dir):
    if 'thp_dl' not in df.columns:
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, 'thp_dl')
    if not src.empty:
        ax.plot(src['time_seconds'], src['thp_dl'], color=COLOR_SOURCE, linewidth=1.5,
                label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt['thp_dl'], color=COLOR_TARGET, linewidth=1.5,
                linestyle='--', label='Target gNB', alpha=0.8)
    _setup_ax(ax, ylabel='Throughput DL (kbps)', title='DL Throughput')
    _save(fig, output_dir, 'thp_dl')


def plot_thp_ul(df, output_dir):
    if 'thp_ul' not in df.columns:
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, 'thp_ul')
    if not src.empty:
        ax.plot(src['time_seconds'], src['thp_ul'], color=COLOR_SOURCE, linewidth=1.5,
                label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt['thp_ul'], color=COLOR_TARGET, linewidth=1.5,
                linestyle='--', label='Target gNB', alpha=0.8)
    _setup_ax(ax, ylabel='Throughput UL (kbps)', title='UL Throughput')
    _save(fig, output_dir, 'thp_ul')


def plot_rsrp(df, output_dir):
    # Check multiple possible column names
    col = None
    for c in ['rsrp', 'rsrp_kpm']:
        if c in df.columns and df[c].notna().any():
            col = c
            break
    if col is None:
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, col)
    if not src.empty:
        ax.plot(src['time_seconds'], src[col], color=COLOR_SOURCE, linewidth=1.5,
                marker='o', markersize=3, label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt[col], color=COLOR_TARGET, linewidth=1.5,
                marker='s', markersize=3, linestyle='--', label='Target gNB', alpha=0.8)
    ax.axhline(y=-100, color='orange', linestyle=':', alpha=0.5, label='Weak threshold (-100 dBm)')
    _setup_ax(ax, ylabel='RSRP (dBm)', title='RSRP')
    _save(fig, output_dir, 'rsrp')


def plot_sinr(df, output_dir):
    col = None
    for c in ['sinr', 'sinr_kpm']:
        if c in df.columns and df[c].notna().any():
            col = c
            break
    if col is None:
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, col)
    if not src.empty:
        ax.plot(src['time_seconds'], src[col], color=COLOR_SOURCE, linewidth=1.5,
                marker='^', markersize=3, label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt[col], color=COLOR_TARGET, linewidth=1.5,
                marker='v', markersize=3, linestyle='--', label='Target gNB', alpha=0.8)
    ax.axhline(y=10, color='green', linestyle=':', alpha=0.5, label='Good threshold (10 dB)')
    _setup_ax(ax, ylabel='SINR (dB)', title='SINR')
    _save(fig, output_dir, 'sinr')


def plot_rsrq(df, output_dir):
    col = None
    for c in ['rsrq', 'rsrq_rrc']:
        if c in df.columns and df[c].notna().any():
            col = c
            break
    if col is None:
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, col)
    if not src.empty:
        ax.plot(src['time_seconds'], src[col], color=COLOR_SOURCE, linewidth=1.5,
                marker='s', markersize=3, label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt[col], color=COLOR_TARGET, linewidth=1.5,
                marker='D', markersize=3, linestyle='--', label='Target gNB', alpha=0.8)
    ax.axhline(y=-12, color='orange', linestyle=':', alpha=0.5, label='Weak threshold (-12 dB)')
    _setup_ax(ax, ylabel='RSRQ (dB)', title='RSRQ')
    _save(fig, output_dir, 'rsrq')


def plot_rlc_delay(df, output_dir):
    if 'rlc_delay' not in df.columns or not df['rlc_delay'].notna().any():
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, 'rlc_delay')
    if not src.empty:
        ax.bar(src['time_seconds'], src['rlc_delay'], color=COLOR_SOURCE, alpha=0.7,
               width=0.8, label='Source gNB')
    if not tgt.empty:
        ax.bar(tgt['time_seconds'], tgt['rlc_delay'], color=COLOR_TARGET, alpha=0.7,
               width=0.8, label='Target gNB')
    ax.axhline(y=20, color='red', linestyle='--', alpha=0.5, label='Typical threshold (20 ms)')
    _setup_ax(ax, ylabel='RLC Delay (ms)', title='RLC Delay')
    _save(fig, output_dir, 'rlc_delay')


def plot_prb_dl(df, output_dir):
    if 'prb_dl' not in df.columns or not df['prb_dl'].notna().any():
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, 'prb_dl')
    if not src.empty:
        ax.plot(src['time_seconds'], src['prb_dl'], color=COLOR_SOURCE, linewidth=1.5,
                label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt['prb_dl'], color=COLOR_TARGET, linewidth=1.5,
                linestyle='--', label='Target gNB', alpha=0.8)
    _setup_ax(ax, ylabel='PRB DL', title='PRB DL Allocation')
    _save(fig, output_dir, 'prb_dl')


def plot_prb_ul(df, output_dir):
    if 'prb_ul' not in df.columns or not df['prb_ul'].notna().any():
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, 'prb_ul')
    if not src.empty:
        ax.plot(src['time_seconds'], src['prb_ul'], color=COLOR_SOURCE, linewidth=1.5,
                label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt['prb_ul'], color=COLOR_TARGET, linewidth=1.5,
                linestyle='--', label='Target gNB', alpha=0.8)
    _setup_ax(ax, ylabel='PRB UL', title='PRB UL Allocation')
    _save(fig, output_dir, 'prb_ul')


def plot_score(df, output_dir):
    if 'score' not in df.columns or not df['score'].notna().any():
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    src, tgt = _split_source_target(df, 'score')
    if not src.empty:
        ax.plot(src['time_seconds'], src['score'], color=COLOR_SOURCE, linewidth=1.5,
                label='Source gNB', alpha=0.8)
    if not tgt.empty:
        ax.plot(tgt['time_seconds'], tgt['score'], color=COLOR_TARGET, linewidth=1.5,
                linestyle='--', label='Target gNB', alpha=0.8)
    ax.axhline(y=0.35, color='red', linestyle='--', alpha=0.5, label='HO Threshold (0.35)')
    _setup_ax(ax, ylabel='Score', title='Multi-Metric Score')
    _save(fig, output_dir, 'score')


def plot_ho_triggered(df, output_dir):
    if 'ho_triggered' not in df.columns:
        return
    fig, ax = plt.subplots(figsize=(12, 5))
    ho_events = df[df['ho_triggered'] == 1]
    ax.plot(df['time_seconds'], df['ho_triggered'], color='lightgray', linewidth=0.5)
    if not ho_events.empty:
        ax.scatter(ho_events['time_seconds'], ho_events['ho_triggered'],
                   color=COLOR_HO, s=50, zorder=5, label='HO Triggered')
    _setup_ax(ax, ylabel='HO Triggered', title='Handover Events')
    _save(fig, output_dir, 'ho_triggered')


def main():
    parser = argparse.ArgumentParser(description='Analizza dati CSV delle metriche di handover')
    parser.add_argument('csv_file', help='File CSV con i dati delle metriche')
    parser.add_argument('--output-dir', default='plots', help='Directory base di output')
    parser.add_argument('--metrics', nargs='+',
                        default=['thp_dl', 'thp_ul', 'rsrp', 'sinr', 'rsrq',
                                 'rlc_delay', 'prb_dl', 'prb_ul', 'score', 'ho_triggered'],
                        help='Metriche da generare')
    parser.add_argument('--all', action='store_true', help='Genera tutti i grafici')
    args = parser.parse_args()

    df = load_and_process_data(args.csv_file)
    if df is None:
        return

    output_dir = create_timestamped_output_dir(args.output_dir)

    all_metrics = ['thp_dl', 'thp_ul', 'rsrp', 'sinr', 'rsrq',
                   'rlc_delay', 'prb_dl', 'prb_ul', 'score', 'ho_triggered']
    metrics = all_metrics if args.all else args.metrics

    plot_funcs = {
        'thp_dl': plot_thp_dl, 'thp_ul': plot_thp_ul,
        'rsrp': plot_rsrp, 'sinr': plot_sinr, 'rsrq': plot_rsrq,
        'rlc_delay': plot_rlc_delay, 'prb_dl': plot_prb_dl, 'prb_ul': plot_prb_ul,
        'score': plot_score, 'ho_triggered': plot_ho_triggered,
    }

    for m in metrics:
        if m in plot_funcs:
            plot_funcs[m](df, output_dir)

    print(f"\nAnalisi completata! Risultati in {output_dir}/")


if __name__ == "__main__":
    main()
