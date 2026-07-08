import pandas as pd
from influxdb_client import InfluxDBClient
from datetime import timezone, timedelta
from validation_rules import validate_payload

# ─── Konfigurasi ──────────────────────────────────────────────────────────────
INFLUX_URL    = "https://us-east-1-1.aws.cloud2.influxdata.com"  # ganti sesuai region Anda
INFLUX_TOKEN  = "sV-RpZ9kw0f2T1JxDqU5wvn2dK_B5RC8RYYvf_8rrrLJv_2u_o-ih9TTRSIXjxBz_AD9C1jP5euUaeb0Tsy7Dw=="
INFLUX_ORG    = ""
INFLUX_BUCKET = "battery_ev_db"

RANGE           = "-24h"   # ganti sesuai kebutuhan: -1h, -24h, -7d, dst.
OUTPUT_FILE     = "bms_export.csv"
REJECTED_FILE   = "bms_export_rejected.csv"  # baris yang dibuang, untuk audit/debug

WIB = timezone(timedelta(hours=7))  # UTC+7

client    = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
query_api = client.query_api()

def _to_float(val):
    if val is None or val == "":
        return None
    try:
        return float(val)
    except (TypeError, ValueError):
        return None

def is_row_valid(row: dict) -> tuple[bool, str]:
    """Adaptasi row CSV (Cell01_mV, CellWR1, dst) ke bentuk yang
    validate_payload() harapkan (cells_mv list, wire_res_mohm list),
    lalu panggil satu sumber kebenaran yang sama dengan bridge."""
    adapted = {
        "soc":           _to_float(row.get("soc")),
        "pack_v":        _to_float(row.get("pack_v")),
        "current_a":     _to_float(row.get("current_a")),
        "temp_mos":      _to_float(row.get("temp_mos")),
        "temp_t1":       _to_float(row.get("temp_t1")),
        "temp_t2":       _to_float(row.get("temp_t2")),
        "cell_min_mv":   _to_float(row.get("cell_min_mv")),
        "cell_max_mv":   _to_float(row.get("cell_max_mv")),
        "cell_delta_mv": _to_float(row.get("cell_delta_mv")),
        "cycle_count":   _to_float(row.get("cycle_count")),
        "avg_cell_mv":   _to_float(row.get("avg_cell_mv")),
        "lat":           _to_float(row.get("lat")),
        "lon":           _to_float(row.get("lon")),
        "cells_mv": [
            _to_float(row.get(f"Cell{i:02d}_mV"))
            for i in range(1, 25)
            if row.get(f"Cell{i:02d}_mV") not in (None, "")
        ],
        "wire_res_mohm": [
            _to_float(row.get(f"CellWR{i}"))
            for i in range(1, 25)
            if row.get(f"CellWR{i}") not in (None, "")
        ],
    }
    return validate_payload(adapted)

def _in_range(val, lo, hi):
    return val is not None and lo <= val <= hi

# ─── 1. Query bms_pack (sekarang sudah termasuk semua cell & wire res) ────────
print("[INFO] Query bms_pack ...")
query_pack = f'''
from(bucket: "{INFLUX_BUCKET}")
  |> range(start: {RANGE})
  |> filter(fn: (r) => r._measurement == "bms_pack")
'''

pack_dict = {}
tables = query_api.query(query_pack)
for table in tables:
    for record in table.records:
        t   = record.get_time()
        nid = str(record.values.get("node_id", ""))
        key = (t, nid)
        if key not in pack_dict:
            pack_dict[key] = {"_time": t, "node_id": nid}
        pack_dict[key][record.get_field()] = record.get_value()
        pack_dict[key]["gateway_id"] = record.values.get("gateway_id", "")

print(f"[INFO] bms_pack: {len(pack_dict)} timestamp unik")

# ─── 2. Susun baris CSV ───────────────────────────────────────────────────────
print("[INFO] Menyusun baris ...")
rows = []

for (t, nid), pack in pack_dict.items():
    row = {}

    # Timestamp WIB dengan milidetik: 2026-06-17 13:31:39.123
    if t:
        t_wib = t.astimezone(WIB)
        ms    = t_wib.microsecond // 1000
        row["timestamp"] = t_wib.strftime("%Y-%m-%d %H:%M:%S") + f".{ms:03d}"
    else:
        row["timestamp"] = ""

    row["gateway_id"]    = pack.get("gateway_id", "")
    row["node_id"]       = nid
    row["seq"]           = pack.get("seq", "")
    row["soc"]           = pack.get("soc", "")
    row["pack_v"]        = pack.get("pack_v", "")
    row["current_a"]     = pack.get("current_a", "")
    row["remain_ah"]     = pack.get("remain_ah", "")
    row["nominal_ah"]    = pack.get("nominal_ah", "")
    row["cycle_count"]   = pack.get("cycle_count", "")
    row["temp_mos"]      = pack.get("temp_mos", "")
    row["temp_t1"]       = pack.get("temp_t1", "")
    row["temp_t2"]       = pack.get("temp_t2", "")
    row["avg_cell_mv"]   = pack.get("avg_cell_mv", "")
    row["cell_delta_mv"] = pack.get("cell_delta_mv", "")
    row["cell_min_mv"]   = pack.get("cell_min_mv", "")
    row["cell_max_mv"]   = pack.get("cell_max_mv", "")
    row["lat"]           = pack.get("lat", "")
    row["lon"]           = pack.get("lon", "")
    row["rssi"]          = pack.get("rssi", "")
    row["snr"]           = pack.get("snr", "")

    # [FIX] Cell voltage & wire resistance diambil langsung dari "pack"
    # (sama point/timestamp), bukan dari lookup measurement terpisah lagi.
    for i in range(1, 25):
        row[f"Cell{i:02d}_mV"] = pack.get(f"cell{i:02d}_mv", "")
        row[f"CellWR{i}"]      = pack.get(f"wire{i:02d}_mohm", "")

    rows.append(row)

# ─── 2b. Filter baris korup ───────────────────────────────────────────────────
print("[INFO] Validasi baris ...")
valid_rows, rejected_rows = [], []
for row in rows:
    ok, reason = is_row_valid(row)
    if ok:
        valid_rows.append(row)
    else:
        row["_reject_reason"] = reason
        rejected_rows.append(row)

n_total, n_rejected = len(rows), len(rejected_rows)
pct = (n_rejected / n_total * 100) if n_total else 0
print(f"[INFO] Validasi selesai: {n_total - n_rejected}/{n_total} baris valid "
      f"({n_rejected} dibuang, {pct:.1f}%)")

rows = valid_rows

# ─── 3. Tulis CSV ─────────────────────────────────────────────────────────────
if not rows:
    print("[WARN] Tidak ada data valid. Pastikan ESP32 sudah mengirim data, "
          "RANGE mencakup waktu pengambilan data, dan filter tidak terlalu ketat.")
else:
    cell_cols = [f"Cell{i:02d}_mV" for i in range(1, 25)]
    wr_cols   = [f"CellWR{i}"      for i in range(1, 25)]
    columns   = [
        "timestamp", "gateway_id", "node_id", "seq",
        "soc", "pack_v", "current_a", "remain_ah", "nominal_ah", "cycle_count",
        "temp_mos", "temp_t1", "temp_t2",
        "avg_cell_mv", "cell_delta_mv", "cell_min_mv", "cell_max_mv",
        "lat", "lon", "rssi", "snr"
    ] + cell_cols + wr_cols

    df = pd.DataFrame(rows, columns=columns)
    df.sort_values("timestamp", inplace=True)
    df.to_csv(OUTPUT_FILE, index=False, sep=";", decimal=",", encoding="utf-8")
    print(f"[OK] {len(df)} baris valid -> {OUTPUT_FILE}")

# Simpan baris yang dibuang untuk audit — supaya bisa dicek apakah filter
# terlalu ketat/longgar, dan untuk memverifikasi dampak bug on_message di bridge.
if rejected_rows:
    df_rej = pd.DataFrame(rejected_rows)
    df_rej.to_csv(REJECTED_FILE, index=False, sep=";", decimal=",", encoding="utf-8")
    print(f"[INFO] {len(df_rej)} baris ditolak -> {REJECTED_FILE} (untuk audit)")

client.close()
print("Selesai.")