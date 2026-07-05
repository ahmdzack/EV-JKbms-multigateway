"""
generate_charts.py
==================
Membuat 6 grafik dari data ekspor BMS (format CSV dengan separator ";" dan
desimal ",").

Grafik yang dihasilkan:
  1. chart_soc.png              — State of Charge vs waktu
  2. chart_voltage_current.png  — Pack voltage & current (dual axis)
  3. chart_temperature.png      — Suhu 3 sensor vs waktu
  4. chart_cell_imbalance.png   — Cell voltage min/avg/max + delta imbalance
  5. chart_signal_quality.png   — RSSI & SNR per gateway (boxplot)
  6. chart_snr_rssi_scatter.png — Scatter SNR vs RSSI (data valid vs corrupt)

Kebutuhan:
  pip install pandas matplotlib

Cara pakai:
  python generate_charts.py
  python generate_charts.py --file data_lain.csv --output ./output_folder
"""

import argparse
import os
import pandas as pd
import matplotlib.pyplot as plt
import matplotlib.dates as mdates


# ─── KONFIGURASI DEFAULT ──────────────────────────────────────────────────────
DEFAULT_FILE   = "bms_export.csv"
DEFAULT_OUTPUT = "."

# Threshold validasi — sesuaikan dengan spesifikasi BMS/pack kamu
SOC_MIN, SOC_MAX           = 0, 100        # %
PACK_V_MAX                 = 200           # Volt (24S LiFePO4 max ~88V)
CURRENT_A_MAX              = 200           # Ampere absolut


# ─── HELPER ───────────────────────────────────────────────────────────────────
def load_and_clean(filepath: str, rejected_filepath: str = None) -> pd.DataFrame:
    df = pd.read_csv(filepath, sep=";", decimal=",")
    df["timestamp"] = pd.to_datetime(df["timestamp"])
    df["is_corrupt"] = False   # semua baris di file utama = sudah valid

    if rejected_filepath and os.path.exists(rejected_filepath):
        rej = pd.read_csv(rejected_filepath, sep=";", decimal=",")
        rej["timestamp"] = pd.to_datetime(rej["timestamp"])
        rej["is_corrupt"] = True
        full_df = pd.concat([df, rej], ignore_index=True).sort_values("timestamp")
    else:
        full_df = df.copy()

    clean = df.sort_values("timestamp").reset_index(drop=True)
    return clean, full_df

def save(fig, path: str):
    fig.tight_layout()
    fig.savefig(path, dpi=150, bbox_inches="tight")
    plt.close(fig)
    print(f"[OK]   Tersimpan -> {path}")


# ─── CHART 1: SoC vs Waktu ────────────────────────────────────────────────────
def chart_soc(df: pd.DataFrame, out: str):
    fig, ax = plt.subplots(figsize=(10, 4.5))

    ax.plot(df["timestamp"], df["soc"],
            color="#2563eb", linewidth=1.5, label="SoC")

    ax.set_title("State of Charge (SoC) vs Waktu")
    ax.set_ylabel("SoC (%)")
    ax.set_xlabel("Waktu")
    ax.set_ylim(0, 100)
    ax.grid(alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.autofmt_xdate()

    save(fig, out)


# ─── CHART 2: Pack Voltage & Current (dual axis) ─────────────────────────────
def chart_voltage_and_current_separate(df: pd.DataFrame, out_voltage: str, out_current: str):
    """
    Dipisah jadi dua file terpisah — masing-masing dengan skala sendiri,
    tidak ada twin-axis yang berpotensi menyesatkan.
    """
    # ── Chart A: Pack Voltage ──────────────────────────────────
    fig1, ax1 = plt.subplots(figsize=(10, 4))

    v_min, v_max = df["pack_v"].min(), df["pack_v"].max()
    v_range = v_max - v_min
    v_pad = max(v_range * 0.15, 0.05)

    ax1.plot(df["timestamp"], df["pack_v"], color="#dc2626", linewidth=1.5)
    ax1.set_ylim(v_min - v_pad, v_max + v_pad)
    ax1.set_title("Pack Voltage vs Waktu")
    ax1.set_ylabel("Pack Voltage (V)")
    ax1.set_xlabel("Waktu")
    ax1.grid(alpha=0.3)
    ax1.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    ax1.text(0.02, 0.02, f"Rentang aktual: {v_min:.2f}–{v_max:.2f} V (Δ={v_range:.2f} V)",
              transform=ax1.transAxes, fontsize=8, color="#dc2626",
              verticalalignment="bottom",
              bbox=dict(facecolor="white", alpha=0.7, edgecolor="none"))
    fig1.autofmt_xdate()
    save(fig1, out_voltage)

    # ── Chart B: Current ───────────────────────────────────────
    fig2, ax2 = plt.subplots(figsize=(10, 4))

    a_min, a_max = df["current_a"].min(), df["current_a"].max()
    a_range = a_max - a_min
    a_pad = max(a_range * 0.15, 0.05)

    ax2.plot(df["timestamp"], df["current_a"], color="#16a34a", linewidth=1.5)
    ax2.set_ylim(a_min - a_pad, a_max + a_pad)
    ax2.set_title("Current vs Waktu")
    ax2.set_ylabel("Current (A)")
    ax2.set_xlabel("Waktu")
    ax2.grid(alpha=0.3)
    ax2.axhline(0, color="gray", linestyle="--", linewidth=0.8, alpha=0.6)
    ax2.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig2.autofmt_xdate()
    save(fig2, out_current)
    
# ─── CHART 3: Suhu 3 Sensor ───────────────────────────────────────────────────
def chart_temperature(df: pd.DataFrame, out: str):
    fig, ax = plt.subplots(figsize=(10, 4.5))

    ax.plot(df["timestamp"], df["temp_mos"],
            color="#ea580c", linewidth=1.3, label="Temp MOS")
    ax.plot(df["timestamp"], df["temp_t1"],
            color="#0891b2", linewidth=1.3, label="Temp T1")
    ax.plot(df["timestamp"], df["temp_t2"],
            color="#7c3aed", linewidth=1.3, label="Temp T2")

    ax.set_title("Suhu Sistem (MOSFET & Baterai) vs Waktu")
    ax.set_ylabel("Suhu (°C)")
    ax.set_xlabel("Waktu")
    ax.legend(loc="upper right")
    ax.grid(alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.autofmt_xdate()

    save(fig, out)


# ─── CHART 4: Cell Voltage Statistik + Delta ─────────────────────────────────
def chart_cell_imbalance(df: pd.DataFrame, out: str):
    fig, (ax1, ax2) = plt.subplots(2, 1, figsize=(10, 7), sharex=True)

    # Panel atas: min / avg / max
    ax1.plot(df["timestamp"], df["cell_max_mv"],
             color="#dc2626", linewidth=1.2, label="Max")
    ax1.plot(df["timestamp"], df["avg_cell_mv"],
             color="#2563eb", linewidth=1.2, label="Avg")
    ax1.plot(df["timestamp"], df["cell_min_mv"],
             color="#16a34a", linewidth=1.2, label="Min")
    ax1.fill_between(df["timestamp"],
                     df["cell_min_mv"], df["cell_max_mv"],
                     color="#94a3b8", alpha=0.15, label="Spread")

    ax1.set_title("Statistik Tegangan Sel (Min / Avg / Max) vs Waktu")
    ax1.set_ylabel("Tegangan (mV)")
    ax1.legend(loc="upper right")
    ax1.grid(alpha=0.3)

    # Panel bawah: delta (imbalance)
    ax2.plot(df["timestamp"], df["cell_delta_mv"],
             color="#b91c1c", linewidth=1.5)
    ax2.set_title("Cell Voltage Delta (Imbalance) vs Waktu")
    ax2.set_ylabel("Delta (mV)")
    ax2.set_xlabel("Waktu")
    ax2.grid(alpha=0.3)
    ax2.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.autofmt_xdate()

    save(fig, out)


# ─── CHART 5: RSSI & SNR per Gateway (boxplot) ───────────────────────────────
def chart_signal_quality(df: pd.DataFrame, out: str):
    gateways   = sorted(df["gateway_id"].unique())
    colors     = ["#2563eb", "#dc2626", "#16a34a", "#ea580c"]   # warna per gateway

    rssi_data  = [df[df["gateway_id"] == g]["rssi"].values for g in gateways]
    snr_data   = [df[df["gateway_id"] == g]["snr"].values  for g in gateways]

    fig, (ax1, ax2) = plt.subplots(1, 2, figsize=(11, 4.5))

    def styled_boxplot(ax, data, labels, title, ylabel):
        bp = ax.boxplot(data, tick_labels=labels, patch_artist=True)
        for patch, col in zip(bp["boxes"], colors):
            patch.set_facecolor(col)
            patch.set_alpha(0.4)
        ax.set_title(title)
        ax.set_ylabel(ylabel)
        ax.grid(alpha=0.3, axis="y")

    styled_boxplot(ax1, rssi_data, gateways, "RSSI per Gateway", "RSSI (dBm)")
    styled_boxplot(ax2, snr_data,  gateways, "SNR per Gateway",  "SNR (dB)")
    ax2.axhline(0, color="gray", linestyle="--", linewidth=0.8,
                label="SNR = 0 dB")
    ax2.legend(fontsize=9)

    fig.suptitle("Perbandingan Kualitas Sinyal LoRa per Gateway",
                 fontsize=13, y=1.02)
    save(fig, out)


# ─── CHART 6: Scatter SNR vs RSSI (valid vs corrupt) ─────────────────────────
def chart_snr_rssi_scatter(df_all: pd.DataFrame, out: str):
    """
    df_all harus DataFrame PENUH (sebelum dibersihkan),
    karena kita perlu tahu baris mana yang corrupt untuk di-highlight.
    """
    normal  = df_all[~df_all["is_corrupt"]]
    corrupt = df_all[df_all["is_corrupt"]]

    fig, ax = plt.subplots(figsize=(8, 6))

    ax.scatter(normal["rssi"], normal["snr"],
               c="#2563eb", alpha=0.6, s=35,
               edgecolors="none",
               label=f"Valid (n={len(normal)})")

    ax.scatter(corrupt["rssi"], corrupt["snr"],
               c="#dc2626", alpha=0.9, s=60,
               marker="x", linewidths=2,
               label=f"Corrupt (n={len(corrupt)})")

    ax.axhline(0, color="gray", linestyle="--",
               linewidth=0.8, alpha=0.7, label="SNR = 0 dB")

    ax.set_title("SNR vs RSSI — Data Valid vs Corrupt")
    ax.set_xlabel("RSSI (dBm)")
    ax.set_ylabel("SNR (dB)")
    ax.legend(loc="upper left")
    ax.grid(alpha=0.3)

    save(fig, out)

    # ─── CHART BARU: RSSI vs Waktu per Gateway ───────────────────────────────────
def chart_rssi_vs_time_gateway(df_all: pd.DataFrame, out: str):
    """
    PENTING: pakai df_all (data PENUH, belum difilter is_corrupt),
    bukan clean_df — karena titik yang justru mau diselidiki (RSSI drop
    ekstrem) kemungkinan besar termasuk baris yang ditandai corrupt.
    Kalau pakai clean_df, titik paling informatif malah hilang.
    """
    fig, ax = plt.subplots(figsize=(11, 5))

    colors = {"Workshop": "#2563eb", "gedung_sipil": "#dc2626", "technomart": "#16a34a"}
    for g, c in colors.items():
        sub = df_all[df_all["gateway_id"] == g]
        ax.scatter(sub["timestamp"], sub["rssi"], c=c, alpha=0.6, s=25,
                   label=f"{g} (n={len(sub)})")

    ax.set_title("RSSI vs Waktu per Gateway")
    ax.set_xlabel("Waktu")
    ax.set_ylabel("RSSI (dBm)")
    ax.legend(loc="lower left")
    ax.grid(alpha=0.3)
    ax.xaxis.set_major_formatter(mdates.DateFormatter("%H:%M"))
    fig.autofmt_xdate()

    save(fig, out)


# ─── MAIN ─────────────────────────────────────────────────────────────────────
def main():
    parser = argparse.ArgumentParser(
        description="Generate chart BMS dari file CSV ekspor InfluxDB")
    parser.add_argument("--file",   default=DEFAULT_FILE,
                        help=f"Path ke file CSV (default: {DEFAULT_FILE})")
    parser.add_argument("--output", default=DEFAULT_OUTPUT,
                        help=f"Folder output gambar (default: {DEFAULT_OUTPUT})")
    parser.add_argument("--rejected", default="bms_export_rejected.csv",
                    help="Path ke file CSV baris yang ditolak (default: bms_export_rejected.csv)")
    args = parser.parse_args()

    os.makedirs(args.output, exist_ok=True)

    print(f"[INFO] Membaca file: {args.file}")
    clean_df, full_df = load_and_clean(args.file, args.rejected)

    plt.rcParams.update({
        "font.size": 11,
        "axes.grid": True,
        "grid.alpha": 0.3,
        "figure.facecolor": "white",
    })

    def path(name):
        return os.path.join(args.output, name)

    chart_soc             (clean_df, path("chart_soc.png"))
    chart_voltage_and_current_separate (clean_df, 
                                        path("chart_pack_voltage.png"),
                                        path("chart_current.png"))
    chart_temperature     (clean_df, path("chart_temperature.png"))
    chart_cell_imbalance  (clean_df, path("chart_cell_imbalance.png"))
    chart_signal_quality       (clean_df, path("chart_signal_quality.png"))
    chart_rssi_vs_time_gateway (full_df,  path("chart_rssi_vs_time_gateway.png"))
    chart_snr_rssi_scatter     (full_df,  path("chart_snr_rssi_scatter.png"))

    print("\n[SELESAI] Semua grafik tersimpan di folder:", args.output)


if __name__ == "__main__":
    main()