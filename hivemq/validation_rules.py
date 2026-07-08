# validation_rules.py — satu sumber kebenaran, dipakai bridge & export

PACK_V_RANGE      = (50, 100)
CURRENT_A_RANGE   = (-300, 300)
TEMP_RANGE        = (-40, 100)
CELL_MV_RANGE     = (2000, 4200)
CELL_DELTA_RANGE  = (0, 500)
WIRE_MOHM_RANGE   = (0, 2000)
CYCLE_COUNT_RANGE = (0, 5000)      # sesuaikan realistis dari datasheet JK-BMS
LAT_RANGE         = (-90, 90)
LON_RANGE         = (-180, 180)

def _in_range(val, lo, hi):
    return val is not None and lo <= val <= hi

def validate_payload(data: dict) -> tuple[bool, str]:
    """Return (valid, reason). Dipakai LANGSUNG oleh bridge dan export —
    tidak ada logika duplikat."""
    soc = data.get("soc")
    if not _in_range(soc, 0, 100):
        return False, f"soc={soc} di luar 0-100"

    pack_v = data.get("pack_v")
    if not _in_range(pack_v, *PACK_V_RANGE):
        return False, f"pack_v={pack_v} di luar {PACK_V_RANGE}"

    current_a = data.get("current_a")
    if not _in_range(current_a, *CURRENT_A_RANGE):
        return False, f"current_a={current_a} di luar {CURRENT_A_RANGE}"

    for key in ("temp_mos", "temp_t1", "temp_t2"):
        v = data.get(key)
        if not _in_range(v, *TEMP_RANGE):
            return False, f"{key}={v} di luar {TEMP_RANGE}"

    cell_min = data.get("cell_min_mv")
    cell_max = data.get("cell_max_mv")
    cell_delta = data.get("cell_delta_mv")
    if not _in_range(cell_min, *CELL_MV_RANGE):
        return False, f"cell_min_mv={cell_min} di luar {CELL_MV_RANGE}"
    if not _in_range(cell_max, *CELL_MV_RANGE):
        return False, f"cell_max_mv={cell_max} di luar {CELL_MV_RANGE}"
    if not _in_range(cell_delta, *CELL_DELTA_RANGE):
        return False, f"cell_delta_mv={cell_delta} di luar {CELL_DELTA_RANGE}"

    # FIELD YANG SEBELUMNYA BOLONG — ditambahkan di sini, bukan diasumsikan aman
    cycle_count = data.get("cycle_count")
    if not _in_range(cycle_count, *CYCLE_COUNT_RANGE):
        return False, f"cycle_count={cycle_count} di luar {CYCLE_COUNT_RANGE}"

    lat, lon = data.get("lat"), data.get("lon")
    if lat not in (None, 0) and not _in_range(lat, *LAT_RANGE):
        return False, f"lat={lat} di luar {LAT_RANGE}"
    if lon not in (None, 0) and not _in_range(lon, *LON_RANGE):
        return False, f"lon={lon} di luar {LON_RANGE}"

    avg_cell_mv = data.get("avg_cell_mv")
    if avg_cell_mv is not None and not _in_range(avg_cell_mv, *CELL_MV_RANGE):
        return False, f"avg_cell_mv={avg_cell_mv} di luar {CELL_MV_RANGE}"

    # PER-SEL — loop yang sebelumnya cuma ada di export_csv.py, sekarang wajib di bridge juga
    cells = data.get("cells_mv", [])
    for i, v in enumerate(cells, start=1):
        if not _in_range(v, *CELL_MV_RANGE):
            return False, f"cell{i:02d}_mv={v} di luar {CELL_MV_RANGE}"

    wires = data.get("wire_res_mohm", [])
    for i, w in enumerate(wires, start=1):
        if not _in_range(w, *WIRE_MOHM_RANGE):
            return False, f"wire{i:02d}_mohm={w} di luar {WIRE_MOHM_RANGE}"

    return True, ""