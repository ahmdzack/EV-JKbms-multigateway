"""
Export bms_pack dari InfluxDB Cloud ke CSV
Format: separator ";", satu baris per paket

[FIX] Sejak MQTT_to_Influxdb.py digabung jadi 1 measurement, cell voltage
dan wire resistance sudah ada sebagai field langsung di "bms_pack"
(cell01_mv...cell24_mv, wire01_mohm...wire24_mohm). Query terpisah ke
measurement "bms_cell" DIHAPUS karena measurement itu sudah tidak pernah
ditulis lagi -> sebelumnya menyebabkan kolom Cell0X_mV/CellWRX selalu kosong.

[FIX-2] Ditambahkan is_row_valid() sebagai lapisan validasi KEDUA sebelum
data ditulis ke CSV. Ini PENTING dibaca: ini bukan pengganti perbaikan bug
di MQTT_to_Influxdb.py, di mana on_message() didefinisikan dua kali dan
definisi kedua (tanpa is_valid_payload()) menimpa yang pertama -> validasi
di bridge saat ini TIDAK PERNAH jalan, sehingga data korup sudah lebih dulu
masuk ke InfluxDB sebelum sampai ke script export ini. Filter di sini cuma
mencegah data korup itu ikut ke CSV/analisis, tidak membersihkan InfluxDB
itu sendiri. Perbaiki bug on_message duplikat di bridge sesegera mungkin.

Kolom output:
  timestamp;gateway_id;node_id;seq;soc;pack_v;current_a;remain_ah;nominal_ah;
  cycle_count;temp_mos;temp_t1;temp_t2;avg_cell_mv;cell_delta_mv;cell_min_mv;
  cell_max_mv;lat;lon;rssi;snr;
  Cell01_mV...Cell24_mV;CellWR1...CellWR24
"""

import pandas as pd
from influxdb_client import InfluxDBClient
from datetime import timezone, timedelta

# ─── Konfigurasi ──────────────────────────────────────────────────────────────
INFLUX_URL    = "https://us-east-1-1.aws.cloud2.influxdata.com"  # ganti sesuai region Anda
INFLUX_TOKEN  = "sV-RpZ9kw0f2T1JxDqU5wvn2dK_B5RC8RYYvf_8rrrLJv_2u_o-ih9TTRSIXjxBz_AD9C1jP5euUaeb0Tsy7Dw=="
INFLUX_ORG    = ""
INFLUX_BUCKET = "battery_ev_db"

RANGE           = "-14d"   # ganti sesuai kebutuhan: -1h, -24h, -7d, dst.
OUTPUT_FILE     = "bms_export.csv"
REJECTED_FILE   = "bms_export_rejected.csv"  # baris yang dibuang, untuk audit/debug

WIB = timezone(timedelta(hours=7))  # UTC+7

client    = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
query_api = client.query_api()

# ─── 0. Validasi baris ────────────────────────────────────────────────────────
# Level-pack: sama persis dengan is_valid_payload() di MQTT_to_Influxdb.py,
# supaya batas fisik yang dipakai konsisten di seluruh pipeline.
# Level-sel: TAMBAHAN — avg_cell_mv dari BMS terbukti masih bisa outlier
# walau semua field level-pack lolos (lihat catatan analisis dashboard
# sebelumnya). Satu sel korup saja cukup untuk membuang seluruh baris,
# supaya tidak ada baris "separuh valid" yang menyesatkan time-series.

PACK_V_RANGE      = (50, 100)     # sesuaikan dengan pack 24S LiFePO4 Anda (~76-88V nominal)
CURRENT_A_RANGE   = (-300, 300)   # sesuaikan dengan rating max charger/motor
TEMP_RANGE        = (-40, 100)
CELL_MV_RANGE     = (2000, 4200)  # rentang fisik sel LiFePO4
CELL_DELTA_RANGE  = (0, 500)      # imbalance >500mV mustahil untuk pack sehat
WIRE_MOHM_RANGE   = (0, 2000)     # longgar; hanya untuk menangkap nilai sampah (mis. 65535)


def _to_float(val):
    if val is None or val == "":
        return None
    try:
        return float(val)
    except (TypeError, ValueError):
        return None


def _in_range(val, lo, hi):
    return val is not None and lo <= val <= hi


def is_row_valid(row: dict) -> tuple[bool, str]:
    """Return (valid, alasan_jika_ditolak)."""
    soc        = _to_float(row.get("soc"))
    pack_v     = _to_float(row.get("pack_v"))
    current_a  = _to_float(row.get("current_a"))
    temp_mos   = _to_float(row.get("temp_mos"))
    temp_t1    = _to_float(row.get("temp_t1"))
    temp_t2    = _to_float(row.get("temp_t2"))
    cell_min   = _to_float(row.get("cell_min_mv"))
    cell_max   = _to_float(row.get("cell_max_mv"))
    cell_delta = _to_float(row.get("cell_delta_mv"))

    if not _in_range(soc, 0, 100):
        return False, f"soc={soc} di luar 0-100"
    if not _in_range(pack_v, *PACK_V_RANGE):
        return False, f"pack_v={pack_v} di luar {PACK_V_RANGE}"
    if not _in_range(current_a, *CURRENT_A_RANGE):
        return False, f"current_a={current_a} di luar {CURRENT_A_RANGE}"
    if not _in_range(temp_mos, *TEMP_RANGE):
        return False, f"temp_mos={temp_mos} di luar {TEMP_RANGE}"
    if not _in_range(temp_t1, *TEMP_RANGE):
        return False, f"temp_t1={temp_t1} di luar {TEMP_RANGE}"
    if not _in_range(temp_t2, *TEMP_RANGE):
        return False, f"temp_t2={temp_t2} di luar {TEMP_RANGE}"
    if not _in_range(cell_min, *CELL_MV_RANGE):
        return False, f"cell_min_mv={cell_min} di luar {CELL_MV_RANGE}"
    if not _in_range(cell_max, *CELL_MV_RANGE):
        return False, f"cell_max_mv={cell_max} di luar {CELL_MV_RANGE}"
    if not _in_range(cell_delta, *CELL_DELTA_RANGE):
        return False, f"cell_delta_mv={cell_delta} di luar {CELL_DELTA_RANGE}"

    # Per-sel (24 sel)
    for i in range(1, 25):
        v = _to_float(row.get(f"Cell{i:02d}_mV"))
        if v is None:
            continue  # kolom kosong/hilang -> jangan buang baris hanya karena ini
        if not _in_range(v, *CELL_MV_RANGE):
            return False, f"Cell{i:02d}_mV={v} di luar {CELL_MV_RANGE}"

    # Per-wire resistance (24 sel) — cuma tangkap nilai sampah/overflow
    for i in range(1, 25):
        w = _to_float(row.get(f"CellWR{i}"))
        if w is None:
            continue
        if not _in_range(w, *WIRE_MOHM_RANGE):
            return False, f"CellWR{i}={w} di luar {WIRE_MOHM_RANGE}"

    return True, ""


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