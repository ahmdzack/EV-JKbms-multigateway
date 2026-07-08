"""
Bridge: HiveMQ (MQTT, TLS) -> InfluxDB Cloud Serverless

Mendengarkan topic "bms/vehicle/+/data" (semua node), parse JSON yang sudah
dipublish oleh receiver_workshop / receiver_halte, lalu tulis ke InfluxDB.

Catatan desain:
- Field pack-level (SOC, arus, tegangan, dst) -> measurement "bms_pack"
- Tiap sel (24 sel) -> measurement "bms_cell" dengan tag cell_index, supaya
  query per-sel di dashboard lebih natural (mis. "tampilkan tegangan sel 7
  sepanjang waktu") dibanding kalau disimpan sebagai 1 field array.
- Trade-off: ini menulis 25 point per paket (1 pack + 24 cell). Pantau usage
  di dashboard InfluxDB Cloud kalau nanti node bertambah jadi 3.
"""
"""
Bridge: HiveMQ (MQTT, TLS) -> InfluxDB Cloud Serverless

Mendengarkan topic "bms/vehicle/+/data" (semua node), parse JSON yang sudah
dipublish oleh receiver_workshop / receiver_halte, lalu tulis ke InfluxDB.


"""

import json
import ssl
import time
import logging
import paho.mqtt.client as mqtt
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS
from validation_rules import validate_payload

# ─── LOGGING CONFIGURATION ──────────────────────────────────────────────────
logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("bridge")

# ─── STATE STORAGE (DEDUPLIKASI) ────────────────────────────────────────────
# Menyimpan sequence number terakhir dari tiap node_id -> {node_id: last_seq}
processed_packets = {} 

# ─── KONFIGURASI HiveMQ ─────────────────────────────────────────────────────
MQTT_HOST = "d350359d619a4da79d3dd62cc4659b70.s1.eu.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USER = "JKbms"
MQTT_PASS = "UH04FTJKbms"
MQTT_TOPIC = "bms/vehicle/+/data"

# ─── KONFIGURASI InfluxDB Cloud Serverless ──────────────────────────────────
INFLUX_URL = "https://us-east-1-1.aws.cloud2.influxdata.com"          
INFLUX_TOKEN = "sV-RpZ9kw0f2T1JxDqU5wvn2dK_B5RC8RYYvf_8rrrLJv_2u_o-ih9TTRSIXjxBz_AD9C1jP5euUaeb0Tsy7Dw=="         
INFLUX_ORG = ""                              
INFLUX_BUCKET = "battery_ev_db"                

influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = influx_client.write_api(write_options=SYNCHRONOUS)

def handle_payload(node_id: int, gateway_id: str, data: dict):
    point = (
        Point("bms_pack")
        .tag("gateway_id", gateway_id)
        .tag("node_id", str(node_id))
        .field("soc", int(data.get("soc", 0)))
        .field("pack_v", float(data.get("pack_v", 0)))
        .field("current_a", float(data.get("current_a", 0)))
        .field("remain_ah", float(data.get("remain_ah", 0)))
        .field("nominal_ah", float(data.get("nominal_ah", 0)))
        .field("cycle_count", int(data.get("cycle_count", 0)))
        .field("temp_mos", float(data.get("temp_mos", 0)))
        .field("temp_t1", float(data.get("temp_t1", 0)))
        .field("temp_t2", float(data.get("temp_t2", 0)))
        .field("avg_cell_mv", int(data.get("avg_cell_mv", 0)))
        .field("cell_delta_mv", int(data.get("cell_delta_mv", 0)))
        .field("cell_min_mv", int(data.get("cell_min_mv", 0)))
        .field("cell_max_mv", int(data.get("cell_max_mv", 0)))
        .field("lat", float(data.get("lat", 0)))
        .field("lon", float(data.get("lon", 0)))
        .field("gps_fix", bool(data.get("gps_fix", False)))
        .field("rssi", int(data.get("rssi", 0)))
        .field("snr", float(data.get("snr", 0)))
    )

    cells = data.get("cells_mv", [])
    wires = data.get("wire_res_mohm", [])
    for i in range(min(len(cells), 24)):
        point = point.field(f"cell{i+1:02d}_mv", int(cells[i]))
    for i in range(min(len(wires), 24)):
        point = point.field(f"wire{i+1:02d}_mohm", int(wires[i]))

    write_api.write(bucket=INFLUX_BUCKET, record=point)


def on_connect(client, userdata, flags, reason_code, properties=None):
    if reason_code == 0:
        log.info("Terhubung ke HiveMQ ✓")
        client.subscribe(MQTT_TOPIC)
        log.info(f"Subscribe: {MQTT_TOPIC}")
    else:
        log.error(f"Gagal connect ke HiveMQ, reason_code={reason_code}")


def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode("utf-8"))
        node_id = data.get("node_id")
        seq = data.get("seq")

        if node_id is None:
            log.warning(f"Payload tanpa node_id, topic={msg.topic}")
            return

        # Validasi DULU — pakai modul bersama
        valid, reason = validate_payload(data)
        if not valid:
            log.warning(f"Payload tidak valid, dibuang: {reason}. "
                        f"node={node_id} seq={seq}")
            return

        # Dedup HANYA setelah lolos validasi
        if seq is not None:
            if processed_packets.get(node_id) == seq:
                log.info(f"Paket duplikat diabaikan. node={node_id} seq={seq}")
                return
            processed_packets[node_id] = seq

        gateway_id = data.get("gateway_id", "unknown")
        handle_payload(node_id, gateway_id, data)
    except json.JSONDecodeError:
        log.error(f"Payload bukan JSON valid di topic {msg.topic}")
    except Exception as e:
        log.error(f"Gagal proses pesan: {e}")
        
def main():
    client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2, protocol=mqtt.MQTTv311)
    client.username_pw_set(MQTT_USER, MQTT_PASS)
    client.tls_set(cert_reqs=ssl.CERT_REQUIRED, tls_version=ssl.PROTOCOL_TLS_CLIENT)
    client.on_connect = on_connect
    client.on_message = on_message

    while True:
        try:
            client.connect(MQTT_HOST, MQTT_PORT, keepalive=60)
            client.loop_forever()
        except Exception as e:
            log.error(f"Koneksi MQTT terputus: {e} — retry dalam 5 detik...")
            time.sleep(5)


if __name__ == "__main__":
    main()