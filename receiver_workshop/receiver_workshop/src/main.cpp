/*
 * ═══════════════════════════════════════════════════════════════════════════
 * TTGO LoRa32 v2.1 — Receiver Workshop
 * SD Card Logger + MQTT Publisher + Captive Portal MikroTik
 *
 * Penelitian: Rancang Bangun Sistem Monitoring & Predictive Maintenance
 *             Battery LiFePO4 pada Kendaraan Listrik — Komunikasi LoRa
 *
 * Fitur:
 *   ✓ Koneksi WiFi kampus + bypass Captive Portal MikroTik
 *   ✓ WiFi cadangan jika WiFi kampus/portal gagal
 *   ✓ Re-login portal otomatis setiap 5 menit
 *   ✓ MQTT TLS ke HiveMQ Cloud (port 8884, 256dpi/MQTT + WiFiClientSecure)
 *   ✓ SD Card dual SPI (VSPI=LoRa, HSPI=SD) — log CSV permanen
 *   ✓ Fallback: MQTT gagal → data ke file pending SD
 *   ✓ Sequence counter untuk de-duplikasi
 *   ✓ OLED 5 halaman: pack, suhu/sel, wire resistance, status, GPS
 *   ✓ 1 node transmitter (MAX_NODES = 1)
 *
 * Library MQTT: 256dpi/MQTT (bukan PubSubClient)
 *   → support WiFiClientSecure sebagai transport TLS
 *   → mqtt.begin(host, port, net) di setup()
 *   → mqtt.connect(id, user, pass) tanpa LWT inline — LWT via setWill()
 *
 * MQTT Topics:
 *   bms/vehicle/<nodeId>/data       → payload JSON lengkap
 *   bms/vehicle/<nodeId>/status     → online/offline (retained)
 *   bms/gateway/<GATEWAY_ID>/status → LWT gateway
 *   bms/gateway/<GATEWAY_ID>/heartbeat
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <SPI.h>
#include <LoRa.h>
#include <Wire.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <FS.h>
#include <SD.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <WiFiClientSecure.h>
#include <ArduinoJson.h>
#include <PubSubClient.h>
// #include <MQTT.h>

#include "payload_bms.h"

// ─── PIN HARDWARE TTGO LoRa32 v2.1 ───────────────────────────────────────────
#define OLED_SDA      21
#define OLED_SCL      22
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64

// VSPI — LoRa SX1276
#define LORA_SCK       5
#define LORA_MISO     19
#define LORA_MOSI     27
#define LORA_CS       18
#define LORA_RST      23
#define LORA_DIO0     26

// HSPI — SD Card (internal TTGO LoRa32)
#define SD_SCK        14
#define SD_MISO        2
#define SD_MOSI       15
#define SD_CS         13

// ─── IDENTITAS GATEWAY ────────────────────────────────────────────────────────
#define GATEWAY_ID    "Workshop"

// ─── KONFIGURASI WiFi KAMPUS ──────────────────────────────────────────────────
const char* WIFI_SSID     = "Camon";
const char* WIFI_PASSWORD = "canca091";

// ─── KONFIGURASI CAPTIVE PORTAL MikroTik ─────────────────────────────────────
const char* PORTAL_USER   = "";
const char* PORTAL_PASS   = "";
const char* PORTAL_IP     = "";

// ─── KONFIGURASI WiFi CADANGAN ────────────────────────────────────────────────
const char* WIFI_SSID_BK  = "HotspotCadangan";
const char* WIFI_PASS_BK  = "passwordhotspot";

// ─── KONFIGURASI MQTT (HiveMQ Cloud — TLS port 8884) ─────────────────────────
// Port 8884 terbukti terbuka dari WiFi kampus (verified via curl)
const char* MQTT_HOST      = "d350359d619a4da79d3dd62cc4659b70.s1.eu.hivemq.cloud";
const int   MQTT_PORT      = 8883;
const char* MQTT_USER      = "JKbms";
const char* MQTT_PASS      = "UH04FTJKbms";
const char* MQTT_CLIENT_ID = "lora_gw_" GATEWAY_ID;

// ─── FREKUENSI LoRa ───────────────────────────────────────────────────────────
#define LORA_FREQ     915E6

// ─── INTERVAL ─────────────────────────────────────────────────────────────────
#define WIFI_TIMEOUT_MS    15000
#define PORTAL_CHECK_MS   300000   // re-login portal setiap 5 menit
#define MQTT_RECONNECT_MS   5000
#define HEARTBEAT_MS       30000
#define NODE_TIMEOUT_MS    30000

// ─── JUMLAH NODE (hanya 1 transmitter) ───────────────────────────────────────
#define MAX_NODES    1

// ─── NAMA FILE SD ─────────────────────────────────────────────────────────────
#define CSV_FILE      "/bms_log.csv"
#define REPLAY_FILE   "/bms_pending.csv"

// ─── STATE NODE ───────────────────────────────────────────────────────────────
PayloadBMS    nodeData[MAX_NODES + 1];
unsigned long lastReceived[MAX_NODES + 1]    = {0};
bool          nodeOnline[MAX_NODES + 1]      = {false};
int           nodeRSSI[MAX_NODES + 1]        = {0};
float         nodeSNR[MAX_NODES + 1]         = {0.0f};
uint32_t      nodePacketCount[MAX_NODES + 1] = {0};
uint8_t       seqCounter[MAX_NODES + 1]      = {0};

// ─── STATE KONEKSI ────────────────────────────────────────────────────────────
bool          portalOK        = false;
bool          usingBackupWifi = false;
unsigned long lastPortalCheck = 0;

// ─── OBJEK GLOBAL ─────────────────────────────────────────────────────────────
SPIClass         spiLora(VSPI);
SPIClass         spiSD(HSPI);
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// MQTT: 256dpi/MQTT library menggunakan WiFiClientSecure sebagai transport TLS
// Buffer 1200 byte disesuaikan dengan ukuran payload JSON BMS (24 sel + wire res)
WiFiClientSecure net;
PubSubClient      mqtt(net);
// MQTTClient       mqtt(1200);

bool sdReady  = false;
bool mqttEver = false;

uint8_t       displayPage       = 0;
unsigned long lastDisplaySwitch = 0;
unsigned long totalPackets      = 0;
unsigned long lastHeartbeat     = 0;
uint32_t      sdPendingCount    = 0;


// ═══════════════════════════════════════════════════════════════════════════════
// CAPTIVE PORTAL
// ═══════════════════════════════════════════════════════════════════════════════

static bool cekInternetAktif() {
    HTTPClient http;
    http.begin("http://connectivitycheck.gstatic.com/generate_204");
    http.setTimeout(5000);
    int code = http.GET();
    http.end();
    return (code == 204);
}

static bool loginMikrotikPortal() {
    HTTPClient http;
    String url = "http://" + String(PORTAL_IP) + "/login";
    http.begin(url);
    http.addHeader("Content-Type", "application/x-www-form-urlencoded");
    http.setTimeout(8000);

    String postData = "username=" + String(PORTAL_USER)
                    + "&password=" + String(PORTAL_PASS)
                    + "&dst=http%3A%2F%2Fwww.msftconnecttest.com%2Fredirect";

    Serial.println("[PORTAL] Mengirim HTTP POST ke MikroTik...");
    int httpCode = http.POST(postData);
    String response = (httpCode > 0) ? http.getString() : "";
    http.end();
    Serial.printf("[PORTAL] HTTP Code: %d\n", httpCode);

    if (httpCode <= 0) {
        Serial.println("[PORTAL] Tidak bisa reach gateway MikroTik.");
        return false;
    }
    if (response.indexOf("login_failed") != -1 ||
        response.indexOf("invalid") != -1) {
        Serial.println("[PORTAL] Autentikasi ditolak.");
        return false;
    }

    delay(1500);
    if (cekInternetAktif()) {
        Serial.println("[PORTAL] Login berhasil! Internet aktif.");
        return true;
    }
    Serial.println("[PORTAL] POST diterima tapi internet belum aktif.");
    return false;
}

static bool connectWiFi(const char* ssid, const char* pass) {
    Serial.printf("[WiFi] Connecting to: %s\n", ssid);
    WiFi.disconnect(true);
    delay(300);
    WiFi.mode(WIFI_STA);
    WiFi.begin(ssid, pass);
    unsigned long t = millis();
    while (WiFi.status() != WL_CONNECTED) {
        if (millis() - t > WIFI_TIMEOUT_MS) {
            Serial.println("[WiFi] TIMEOUT.");
            return false;
        }
        delay(500);
        Serial.print(".");
    }
    Serial.printf("\n[WiFi] OK — IP: %s | GW: %s\n",
                  WiFi.localIP().toString().c_str(),
                  WiFi.gatewayIP().toString().c_str());
    return true;
}

static bool setupKoneksiInternet() {
    usingBackupWifi = false;

    if (connectWiFi(WIFI_SSID, WIFI_PASSWORD)) {
        if (cekInternetAktif()) {
            Serial.println("[WiFi] Internet langsung aktif (sesi portal masih hidup).");
            portalOK = true;
            return true;
        }
        for (int i = 0; i < 3; i++) {
            Serial.printf("[PORTAL] Percobaan %d/3...\n", i + 1);
            if (loginMikrotikPortal()) { portalOK = true; return true; }
            delay(3000);
        }
        Serial.println("[PORTAL] Semua percobaan gagal.");
    }

    Serial.println("[WiFi] Beralih ke WiFi cadangan...");
    if (connectWiFi(WIFI_SSID_BK, WIFI_PASS_BK)) {
        usingBackupWifi = true;
        portalOK = true;
        return true;
    }

    Serial.println("[WiFi] Semua opsi gagal. Mode offline.");
    portalOK = false;
    return false;
}

static void jagaKoneksiInternet() {
    if (millis() - lastPortalCheck < PORTAL_CHECK_MS) return;
    lastPortalCheck = millis();

    if (WiFi.status() != WL_CONNECTED) {
        Serial.println("[WiFi] Terputus, reconnect...");
        portalOK = false;
        setupKoneksiInternet();
        return;
    }
    if (!usingBackupWifi && !cekInternetAktif()) {
        Serial.println("[PORTAL] Sesi expired, login ulang...");
        portalOK = false;
        for (int i = 0; i < 3; i++) {
            if (loginMikrotikPortal()) { portalOK = true; return; }
            delay(3000);
        }
        if (connectWiFi(WIFI_SSID_BK, WIFI_PASS_BK)) {
            usingBackupWifi = true;
            portalOK = true;
        }
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// MQTT (256dpi/MQTT library)
// ═══════════════════════════════════════════════════════════════════════════════

static bool mqttConnect() {
    if (mqtt.connected()) return true;
    if (!portalOK || WiFi.status() != WL_CONNECTED) return false;

    char willTopic[64];
    snprintf(willTopic, sizeof(willTopic), "bms/gateway/%s/status", GATEWAY_ID);
    Serial.printf("[MQTT] Connecting to %s:%d ...\n", MQTT_HOST, MQTT_PORT);

    bool ok = mqtt.connect(MQTT_CLIENT_ID,
                           MQTT_USER, MQTT_PASS,
                           willTopic, 1, true,
                           "{\"online\":false,\"gateway\":\"" GATEWAY_ID "\"}");
    if (ok) {
        Serial.println("[MQTT] Connected ✓");
        char payload[120];
        snprintf(payload, sizeof(payload),
                 "{\"online\":true,\"gateway\":\"%s\",\"sd\":%s,\"wifi\":\"%s\"}",
                 GATEWAY_ID,
                 sdReady ? "true" : "false",
                 usingBackupWifi ? "cadangan" : "kampus");
        mqtt.publish(willTopic, payload, true);
        mqttEver = true;
    } else {
        Serial.printf("[MQTT] Gagal rc=%d\n", mqtt.state());
    }
    return ok;
}
static void sendHeartbeat() {
    if (!mqtt.connected()) return;
    char topic[64], payload[220];
    snprintf(topic, sizeof(topic), "bms/gateway/%s/heartbeat", GATEWAY_ID);
    snprintf(payload, sizeof(payload),
             "{\"gateway\":\"%s\",\"uptime_s\":%lu,\"wifi_rssi\":%d,"
             "\"sd\":%s,\"sd_pending\":%lu,\"wifi\":\"%s\",\"portal_ok\":%s}",
             GATEWAY_ID, millis() / 1000, WiFi.RSSI(),
             sdReady ? "true" : "false",
             (unsigned long)sdPendingCount,
             usingBackupWifi ? "cadangan" : "kampus",
             portalOK ? "true" : "false");
    mqtt.publish(topic, payload);
}

static bool publishNodeData(const PayloadBMS& d, int rssi, float snr,
                            uint8_t seq, uint16_t minV, uint16_t maxV) {
    if (!mqtt.connected()) return false;

    JsonDocument doc;
    doc["gateway_id"]    = GATEWAY_ID;
    doc["node_id"]       = d.nodeId;
    doc["seq"]           = seq;
    doc["ts_ms"]         = millis();
    doc["soc"]           = d.soc;
    doc["pack_v"]        = d.packVoltage     / 100.0f;
    doc["current_a"]     = d.current         / 1000.0f;
    doc["remain_ah"]     = d.remainCapacity  / 1000.0f;
    doc["nominal_ah"]    = d.nominalCapacity / 1000.0f;
    doc["cycle_count"]   = d.cycleCount;
    doc["temp_mos"]      = d.tempMOS / 10.0f;
    doc["temp_t1"]       = d.tempT1  / 10.0f;
    doc["temp_t2"]       = d.tempT2  / 10.0f;
    doc["avg_cell_mv"]   = d.avgCellVolt;
    doc["cell_delta_mv"] = d.cellVoltDelta;
    doc["cell_min_mv"]   = minV;
    doc["cell_max_mv"]   = maxV;
    doc["lat"]           = d.lat / 1e7;
    doc["lon"]           = d.lon / 1e7;
    doc["gps_fix"]       = (d.lat != 0);
    doc["rssi"]          = rssi;
    doc["snr"]           = snr;

    JsonArray cells = doc["cells_mv"].to<JsonArray>();
    for (int i = 0; i < 24; i++) cells.add(d.cellVoltages[i]);

    JsonArray wires = doc["wire_res_mohm"].to<JsonArray>();
    for (int i = 0; i < 24; i++) wires.add(d.wireRes[i]);

    char buf[1200];
    size_t len = serializeJson(doc, buf, sizeof(buf));

    char topic[48];
    snprintf(topic, sizeof(topic), "bms/vehicle/%d/data", d.nodeId);

    // 256dpi/MQTT: publish(topic, payload, length) untuk binary-safe
    bool ok = mqtt.publish(topic, (uint8_t*)buf, len);
    Serial.printf("[MQTT] → %s (%d byte) %s\n", topic, (int)len, ok ? "✓" : "✗");
    return ok;
}


// ═══════════════════════════════════════════════════════════════════════════════
// SD CARD
// ═══════════════════════════════════════════════════════════════════════════════

static void createCSVHeader(const char* path) {
    if (SD.exists(path)) return;
    File f = SD.open(path, FILE_WRITE);
    if (!f) { Serial.printf("[SD] ✗ Gagal buat %s\n", path); return; }

    f.print("timestamp_ms,gateway_id,node_id,seq,");
    f.print("soc,pack_v,current_a,remain_ah,nominal_ah,cycle_count,");
    f.print("temp_mos,temp_t1,temp_t2,");
    f.print("avg_cell_mv,cell_delta_mv,cell_min_mv,cell_max_mv,");
    f.print("lat,lon,rssi,snr");
    for (int i = 1; i <= 24; i++) f.printf(",cell%02d_mv", i);
    for (int i = 1; i <= 24; i++) f.printf(",wire%02d_mohm", i);
    f.println();
    f.close();
    Serial.printf("[SD] ✓ Header dibuat: %s\n", path);
}

static void logToSD(const char* path, const PayloadBMS& d,
                    int rssi, float snr, uint8_t seq,
                    uint16_t minV, uint16_t maxV) {
    if (!sdReady) return;
    File f = SD.open(path, FILE_APPEND);
    if (!f) { Serial.printf("[SD] ✗ Gagal buka %s\n", path); return; }

    f.printf("%lu,%s,%d,%d,", millis(), GATEWAY_ID, d.nodeId, seq);
    f.printf("%d,%.2f,%.3f,%.3f,%.3f,%u,",
             d.soc,
             d.packVoltage     / 100.0f,
             d.current         / 1000.0f,
             d.remainCapacity  / 1000.0f,
             d.nominalCapacity / 1000.0f,
             d.cycleCount);
    f.printf("%.1f,%.1f,%.1f,",
             d.tempMOS / 10.0f, d.tempT1 / 10.0f, d.tempT2 / 10.0f);
    f.printf("%d,%d,%d,%d,",
             d.avgCellVolt, d.cellVoltDelta, minV, maxV);
    f.printf("%.7f,%.7f,%d,%.1f", d.lat / 1e7, d.lon / 1e7, rssi, snr);
    for (int i = 0; i < 24; i++) f.printf(",%d", d.cellVoltages[i]);
    for (int i = 0; i < 24; i++) f.printf(",%d", d.wireRes[i]);
    f.println();
    f.flush();
    f.close();
}


// ═══════════════════════════════════════════════════════════════════════════════
// OLED
// ═══════════════════════════════════════════════════════════════════════════════

static String nodeStatus(uint8_t id) {
    if (!nodeOnline[id]) return "OFFLINE";
    unsigned long age = millis() - lastReceived[id];
    if (age < 5000)  return "LIVE";
    if (age < 15000) return "DELAY";
    return "SLOW";
}

static void updateOLED() {
    // Ganti halaman setiap 4 detik (5 halaman: 0–4)
    if (millis() - lastDisplaySwitch > 4000) {
        lastDisplaySwitch = millis();
        displayPage = (displayPage + 1) % 5;
    }

    display.clearDisplay();
    display.setCursor(0, 0);
    display.setTextSize(1);
    display.printf("%s|N1 [%d/5]\n", GATEWAY_ID, displayPage + 1);

    if (!nodeOnline[1]) {
        display.println("NODE 1: OFFLINE");
        display.printf("PKT: %lu\n", totalPackets);
        display.printf("SD:%s MQTT:%s\n",
                       sdReady ? "OK" : "ERR",
                       mqtt.connected() ? "OK" : "OFF");
        display.printf("WiFi:%s\n",
                       WiFi.status() == WL_CONNECTED ? "OK" : "DISC");
    } else {
        const PayloadBMS& d = nodeData[1];
        if (displayPage == 0) {
            display.printf("SOC:%d%% [%s]\n",  d.soc, nodeStatus(1).c_str());
            display.printf("ARUS:%.1fA\n",     d.current / 1000.0f);
            display.printf("PACK:%.2fV\n",     d.packVoltage / 100.0f);
            display.printf("SISA:%.1fAh\n",    d.remainCapacity / 1000.0f);
            display.printf("RSSI:%ddBm\n",     nodeRSSI[1]);
        } else if (displayPage == 1) {
            display.printf("T-MOS:%.1fC\n",   d.tempMOS / 10.0f);
            display.printf("T1   :%.1fC\n",   d.tempT1  / 10.0f);
            display.printf("T2   :%.1fC\n",   d.tempT2  / 10.0f);
            display.printf("AVG  :%dmV\n",    d.avgCellVolt);
            display.printf("DELTA:%dmV\n",    d.cellVoltDelta);
            display.printf("SIKLUS:%u\n",     d.cycleCount);
        } else if (displayPage == 2) {
            display.printf("WIRE RESISTANCE\n");
            display.printf("R1 :%5dmO\n", d.wireRes[0]);
            display.printf("R2 :%5dmO\n", d.wireRes[1]);
            display.printf("R3 :%5dmO\n", d.wireRes[2]);
            display.printf("R4 :%5dmO\n", d.wireRes[3]);
            uint32_t sumR = 0; uint8_t cnt = 0;
            for (int i = 0; i < 24; i++) {
                if (d.wireRes[i] > 0 && d.wireRes[i] < 10000) {
                    sumR += d.wireRes[i]; cnt++;
                }
            }
            if (cnt > 0) display.printf("AVG:%5dmO\n", sumR / cnt);
        } else if (displayPage == 3) {
            display.printf("WiFi:%s\n",
                WiFi.status() == WL_CONNECTED
                    ? (usingBackupWifi ? "CADANGAN" : "KAMPUS") : "DISC");
            display.printf("Portal:%s\n", portalOK ? "OK" : "GAGAL");
            display.printf("MQTT:%s\n",   mqtt.connected() ? "OK" : "OFF");
            display.printf("SD  :%s\n",   sdReady ? "OK" : "ERR");
            display.printf("PEND:%lu\n",  (unsigned long)sdPendingCount);
        } else {
            if (d.lat != 0) {
                display.printf("LAT:%.7f\n", d.lat / 1e7);
                display.printf("LON:%.7f\n", d.lon / 1e7);
            } else {
                display.println("GPS:NO FIX");
            }
            display.printf("PKT:%lu\n",    nodePacketCount[1]);
            display.printf("SNR:%.1fdB\n", nodeSNR[1]);
            display.printf("SEQ:%d\n",     seqCounter[1]);
        }
    }
    display.display();
}


// ═══════════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    Wire.begin(OLED_SDA, OLED_SCL);

    // ── OLED ────────────────────────────────────────────────────────────────
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[OLED] ✗ SSD1306 tidak terdeteksi.");
    } else {
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(WHITE);
        display.setCursor(0, 0);
        display.println("WORKSHOP GATEWAY");
        display.println(GATEWAY_ID);
        display.println("Memulai sistem...");
        display.display();
    }

    Serial.printf("[INFO] sizeof(PayloadBMS) = %d byte\n", sizeof(PayloadBMS));
    Serial.printf("[INFO] Gateway: %s | MAX_NODES: %d\n\n", GATEWAY_ID, MAX_NODES);

    // ── SD Card (HSPI) ──────────────────────────────────────────────────────
    spiSD.begin(SD_SCK, SD_MISO, SD_MOSI, SD_CS);
    if (!SD.begin(SD_CS, spiSD)) {
        Serial.println("[SD] ✗ SD Card tidak terdeteksi!");
        sdReady = false;
    } else {
        uint64_t cardMB = SD.cardSize() / (1024 * 1024);
        Serial.printf("[SD] ✓ OK — %llu MB\n", cardMB);
        sdReady = true;
        createCSVHeader(CSV_FILE);
        createCSVHeader(REPLAY_FILE);
    }

    // ── LoRa (VSPI) ─────────────────────────────────────────────────────────
    spiLora.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    LoRa.setSPI(spiLora);
    LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LoRa] ✗ Init gagal!");
        while (1) delay(1000);
    }
    LoRa.setSpreadingFactor(7);      // WAJIB sama dengan transmitter
    LoRa.setSignalBandwidth(250E3);  // WAJIB sama dengan transmitter
    LoRa.setCodingRate4(5);
    LoRa.setSyncWord(0xF3);          // WAJIB sama dengan transmitter
    Serial.println("[LoRa] ✓ Aktif (SF7, BW250kHz, CR4/5, SyncWord=0xF3)");

    // ── MQTT + WiFiClientSecure ──────────────────────────────────────────────
    // setInsecure: skip verifikasi sertifikat TLS (cukup untuk penelitian)
    // Untuk produksi: ganti dengan net.setCACert(hivemq_root_cert)
    net.setInsecure();
    mqtt.setServer(MQTT_HOST, MQTT_PORT);
    mqtt.setBufferSize(1200); // default 256, perlu untuk payload JSON BMS
    mqtt.setKeepAlive(60);

    // ── WiFi + Captive Portal ────────────────────────────────────────────────
    setupKoneksiInternet();
    if (portalOK) mqttConnect();

    // ── Status awal OLED ─────────────────────────────────────────────────────
    display.clearDisplay();
    display.setCursor(0, 0);
    display.println("WORKSHOP GATEWAY");
    display.printf("SD    : %s\n",  sdReady ? "READY" : "ERROR");
    display.printf("Portal: %s\n",  portalOK ? "OK" : "GAGAL");
    display.printf("MQTT  : %s\n",  mqtt.connected() ? "OK" : "...");
    display.println("Menunggu node...");
    display.display();

    Serial.println("[INFO] Setup selesai. Menunggu paket LoRa...\n");
}


// ═══════════════════════════════════════════════════════════════════════════════
// LOOP UTAMA
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── 1. Jaga sesi internet (re-login portal jika expired) ────────────────
    jagaKoneksiInternet();

    // ── 2. Kelola koneksi MQTT ───────────────────────────────────────────────
    if (!mqtt.connected()) {
        static unsigned long lastRecon = 0;
        if (now - lastRecon > MQTT_RECONNECT_MS) {
            lastRecon = now;
            mqttConnect();
        }
    }
    mqtt.loop();

    // ── 3. Heartbeat ────────────────────────────────────────────────────────
    if (now - lastHeartbeat > HEARTBEAT_MS) {
        lastHeartbeat = now;
        sendHeartbeat();
    }

    // ── 4. Terima paket LoRa ────────────────────────────────────────────────
    int packetSize = LoRa.parsePacket();

    if (packetSize == sizeof(PayloadBMS)) {
        PayloadBMS incoming;
        LoRa.readBytes((uint8_t*)&incoming, packetSize);
        int   rssi = LoRa.packetRssi();
        float snr  = LoRa.packetSnr();

        // Hanya terima node ID = 1
        if (incoming.nodeId == 1) {
            uint8_t id = incoming.nodeId;
            nodeData[id]        = incoming;
            lastReceived[id]    = now;
            nodeOnline[id]      = true;
            nodeRSSI[id]        = rssi;
            nodeSNR[id]         = snr;
            nodePacketCount[id]++;
            totalPackets++;
            seqCounter[id]      = (seqCounter[id] + 1) & 0xFF;

            // Hitung min/max tegangan sel
            uint16_t minV = 9999, maxV = 0;
            for (int i = 0; i < 24; i++) {
                uint16_t v = incoming.cellVoltages[i];
                if (v > 100 && v < 5000) {
                    if (v < minV) minV = v;
                    if (v > maxV) maxV = v;
                }
            }

            Serial.printf("[RX] NODE %d | RSSI:%d | SNR:%.1f | PKT#%lu\n",
                          id, rssi, snr, nodePacketCount[id]);
            Serial.printf("     SOC:%d%% | %.2fA | %.2fV | %.1fAh | Siklus:%u\n",
                          incoming.soc,
                          incoming.current        / 1000.0f,
                          incoming.packVoltage    / 100.0f,
                          incoming.remainCapacity / 1000.0f,
                          incoming.cycleCount);
            Serial.printf("     AVG:%dmV | DELTA:%dmV | WireRes[0]:%dmO\n",
                          incoming.avgCellVolt,
                          incoming.cellVoltDelta,
                          incoming.wireRes[0]);

            // Log ke CSV permanen (selalu)
            logToSD(CSV_FILE, incoming, rssi, snr, seqCounter[id], minV, maxV);

            // Publish ke MQTT — jika gagal, simpan ke pending
            bool sent = publishNodeData(incoming, rssi, snr,
                                        seqCounter[id], minV, maxV);
            if (!sent) {
                logToSD(REPLAY_FILE, incoming, rssi, snr,
                        seqCounter[id], minV, maxV);
                sdPendingCount++;
                Serial.println("[INFO] MQTT offline — data disimpan ke pending.");
            }

        } else {
            Serial.printf("[RX] ✗ nodeId tidak valid: %d (expected 1)\n",
                          incoming.nodeId);
        }

    } else if (packetSize > 0) {
        Serial.printf("[RX] ✗ Ukuran salah: %d byte (expected %d)\n",
                      packetSize, (int)sizeof(PayloadBMS));
        while (LoRa.available()) LoRa.read();
    }

    // ── 5. Tandai node offline ───────────────────────────────────────────────
    if (nodeOnline[1] && (now - lastReceived[1]) > NODE_TIMEOUT_MS) {
        Serial.println("[WARN] NODE 1 → OFFLINE");
        nodeOnline[1] = false;
        if (mqtt.connected()) {
            char topic[48], payload[80];
            snprintf(topic, sizeof(topic), "bms/vehicle/1/status");
            snprintf(payload, sizeof(payload),
                     "{\"node_id\":1,\"online\":false,\"gateway\":\"%s\"}",
                     GATEWAY_ID);
            mqtt.publish(topic, payload, true);
        }
    }

    // ── 6. Update OLED ──────────────────────────────────────────────────────
    updateOLED();

    delay(50);
}
