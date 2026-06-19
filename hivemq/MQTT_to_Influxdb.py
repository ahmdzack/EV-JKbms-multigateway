"""
Bridge: HiveMQ (MQTT, TLS) -> InfluxDB Cloud Serverless

Mendengarkan topic "bms/vehicle/+/data" (semua node), parse JSON yang sudah
dipublish oleh receiver_workshop / receiver_halte, lalu tulis ke InfluxDB.

Catatan desain:
- Field pack-level (SOC, arus, tegangan, dst) -> measurement "bms_pack"
- Tiap sel (24 sel) -> measurement "bms_cell" dengan tag cell_index, supaya
  query per-sel di Grafana lebih natural (mis. "tampilkan tegangan sel 7
  sepanjang waktu") dibanding kalau disimpan sebagai 1 field array.
- Trade-off: ini menulis 25 point per paket (1 pack + 24 cell). Pantau usage
  di dashboard InfluxDB Cloud kalau nanti node bertambah jadi 3.
"""

import json
import ssl
import time
import logging

import paho.mqtt.client as mqtt
# Tambahkan di bagian atas script, di bawah import
processed_packets = {} 

def on_message(client, userdata, msg):
    try:
        data = json.loads(msg.payload.decode("utf-8"))
        node_id = data.get("node_id")
        seq = data.get("seq") # Menggunakan sequence number dari ESP32
        
        # Deduplikasi: (node_id + seq)
        key = f"{node_id}_{seq}"
        now = time.time()
        
        # Jika sudah diproses dalam 10 detik terakhir, abaikan
        if key in processed_packets and (now - processed_packets[key] < 10):
            return 
        
        processed_packets[key] = now
        
        # Panggil fungsi handle_payload
        handle_payload(node_id, data.get("gateway_id", "unknown"), data)
        
    except Exception as e:
        log.error(f"Error: {e}")
        
from influxdb_client import InfluxDBClient, Point
from influxdb_client.client.write_api import SYNCHRONOUS

logging.basicConfig(level=logging.INFO, format="%(asctime)s [%(levelname)s] %(message)s")
log = logging.getLogger("bridge")

# ─── KONFIGURASI HiveMQ (samakan dengan yang sudah jalan di receiver) ───────
MQTT_HOST = "d350359d619a4da79d3dd62cc4659b70.s1.eu.hivemq.cloud"
MQTT_PORT = 8883
MQTT_USER = "JKbms"
MQTT_PASS = "UH04FTJKbms"
MQTT_TOPIC = "bms/vehicle/+/data"   # wildcard: tangkap semua node

# ─── KONFIGURASI InfluxDB Cloud Serverless ──────────────────────────────────
# Region URL beda-beda tergantung saat sign up (lihat di dashboard InfluxDB
# Anda: Settings/region). Contoh umum: https://us-east-1-1.aws.cloud2.influxdata.com
INFLUX_URL = "https://us-east-1-1.aws.cloud2.influxdata.com"          # ganti sesuai region akun Anda
INFLUX_TOKEN = "sV-RpZ9kw0f2T1JxDqU5wvn2dK_B5RC8RYYvf_8rrrLJv_2u_o-ih9TTRSIXjxBz_AD9C1jP5euUaeb0Tsy7Dw=="         # token write yang dibuat di Langkah 1
INFLUX_ORG = ""                              # WAJIB string kosong untuk Cloud Serverless
INFLUX_BUCKET = "battery_ev_db"                # nama bucket yang dibuat di Langkah 1

influx_client = InfluxDBClient(url=INFLUX_URL, token=INFLUX_TOKEN, org=INFLUX_ORG)
write_api = influx_client.write_api(write_options=SYNCHRONOUS)


def handle_payload(node_id: int, gateway_id: str, data: dict):
    points = []

    pack_point = (
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
    points.append(pack_point)

    cells = data.get("cells_mv", [])
    wires = data.get("wire_res_mohm", [])
    for i in range(min(len(cells), 24)):
        cell_point = (
            Point("bms_cell")
            .tag("gateway_id", gateway_id)
            .tag("node_id", str(node_id))
            .tag("cell_index", str(i + 1))
            .field("voltage_mv", int(cells[i]))
        )
        if i < len(wires):
            cell_point = cell_point.field("wire_res_mohm", int(wires[i]))
        points.append(cell_point)

    write_api.write(bucket=INFLUX_BUCKET, record=points)
    log.info(f"Tulis {len(points)} point ke InfluxDB (node {node_id}, seq {data.get('seq')})")


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
        gateway_id = data.get("gateway_id", "unknown")
        if node_id is None:
            log.warning(f"Payload tanpa node_id, topic={msg.topic}")
            return
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