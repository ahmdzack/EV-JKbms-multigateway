/*
 * ═══════════════════════════════════════════════════════════════════════════
 * T-Beam v1.1 — Node Transmitter BMS
 *
 * Penelitian: Rancang Bangun Sistem Monitoring & Predictive Maintenance
 *             Battery LiFePO4 pada Kendaraan Listrik — Komunikasi LoRa
 *
 * Hardware  : TTGO T-Beam v1.1 (ESP32 + AXP2101 + GPS Neo-6M + SX1276)
 * Target BMS: JK-BD6A24S10P
 *
 * Perbaikan dari versi sebelumnya:
 *   [FIX] LoRa SF/BW diselaraskan dengan receiver (SF12, BW125, CR4/5)
 *         — komentar di kode asli bertentangan dengan nilai aktual,
 *           nilai aktual SF12/BW125 dipertahankan karena lebih robust
 *           untuk jangkauan. Ganti sesuai kebutuhan, pastikan IDENTIK
 *           dengan kedua receiver.
 *   [FIX] Pointer g_pClient disimpan — cegah writeValue ke pointer stale
 *         setelah disconnect (potensi crash/UB di versi asli)
 *   [FIX] g_pRemoteChar di-null setelah disconnect
 *   [FIX] AXP2101 menggunakan XPowersLib yang lebih aktif dipelihara
 *
 * LoRa Settings (WAJIB sama di semua node):
 *   SF=12, BW=125kHz, CR=4/5, TxPower=20dBm, Freq=915MHz
 * ═══════════════════════════════════════════════════════════════════════════
 */

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>

// PMU — gunakan XPowersLib (lebih aktif dari AXP202X_Library)
// Jika library ini tidak ditemukan, ganti dengan:
//   #include <axp20x.h>
#define  XPOWERS_CHIP_AXP2101
#include <XPowersLib.h>

#include <NimBLEDevice.h>
#include <LoRa.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <TinyGPS++.h>
#include <HardwareSerial.h>

#include "payload_bms.h"

// ─── PIN T-Beam v1.1 ─────────────────────────────────────────────────────────
#define OLED_SDA      21
#define OLED_SCL      22
#define SCREEN_WIDTH  128
#define SCREEN_HEIGHT  64

#define GPS_RX        34
#define GPS_TX        12

#define LORA_SCK       5
#define LORA_MISO     19
#define LORA_MOSI     27
#define LORA_CS       18
#define LORA_RST      23
#define LORA_DIO0     26

// ─── KONFIGURASI — WAJIB DISESUAIKAN PER UNIT ────────────────────────────────
// Ganti NODE_ID per kendaraan: 1, 2, atau 3
#define NODE_ID             1

// Frekuensi LoRa — sesuaikan dengan regulasi dan kedua receiver
// Indonesia: 915E6 (ISM), beberapa area pakai 433E6
#define LORA_FREQ           915E6

// MAC address JK-BMS target (huruf kecil, format xx:xx:xx:xx:xx:xx)
#define TARGET_MAC_ADDRESS  "c8:47:80:11:09:1c"

// ─── UUID BLE JK-BMS ─────────────────────────────────────────────────────────
#define SERVICE_UUID        "ffe0"
#define CHARACTERISTIC_UUID "ffe1"

// ─── INTERVAL ────────────────────────────────────────────────────────────────
#define BLE_RECONNECT_MS    5000   // retry BLE jika terputus
#define BLE_REQUEST_MS      3000   // polling GET_DATA ke BMS
#define LORA_SEND_MS        5000   // interval kirim LoRa minimum

#define GPS_DEBUG_MS         1000   // interval cetak status GPS ke Serial

// ─── OBJEK GLOBAL ────────────────────────────────────────────────────────────
XPowersAXP2101      pmu;                   // AXP2101 via XPowersLib
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
TinyGPSPlus      gps;
HardwareSerial   gpsSerial(1);

PayloadBMS dataToSend = {};

// ─── STATE BLE ───────────────────────────────────────────────────────────────
NimBLEUUID  g_serviceUUID(SERVICE_UUID);
NimBLEUUID  g_charUUID(CHARACTERISTIC_UUID);

// [FIX] Simpan pointer client agar bisa dihapus saat disconnect
NimBLEClient*            g_pClient     = nullptr;
NimBLERemoteCharacteristic* g_pRemoteChar = nullptr;

bool g_connected    = false;
bool g_hasWokenUp   = false;
bool g_bmsDataReady = false;

unsigned long g_lastRequest  = 0;
unsigned long g_lastLoRaSend = 0;
unsigned long g_lastBleRetry = 0;
unsigned long g_lastGpsDebug = 0;

// Perintah ke JK-BMS — byte ke-19 diisi checksum oleh calculateChecksum()
uint8_t g_CMD_WAKEUP[20]   = { 0xAA, 0x55, 0x90, 0xEB, 0x96,
                                0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 };
uint8_t g_CMD_GET_DATA[20] = { 0xAA, 0x55, 0x90, 0xEB, 0x97,
                                0,0,0,0,0, 0,0,0,0,0, 0,0,0,0,0 };

// Buffer akumulasi notifikasi BLE
uint8_t g_bmsBuffer[2048];
int     g_bmsBufferLen = 0;


// ═══════════════════════════════════════════════════════════════════════════════
// HELPER — Little-Endian binary readers
// ═══════════════════════════════════════════════════════════════════════════════
static inline uint16_t readU16LE(const uint8_t* d, size_t o) {
    return (uint16_t(d[o + 1]) << 8) | uint16_t(d[o]);
}
static inline int16_t readS16LE(const uint8_t* d, size_t o) {
    return (int16_t)readU16LE(d, o);
}
static inline uint32_t readU32LE(const uint8_t* d, size_t o) {
    return (uint32_t(d[o+3]) << 24) | (uint32_t(d[o+2]) << 16) |
           (uint32_t(d[o+1]) <<  8) |  uint32_t(d[o]);
}
static inline int32_t readS32LE(const uint8_t* d, size_t o) {
    return (int32_t)readU32LE(d, o);
}

// ─── Checksum: jumlah 19 byte pertama, ambil byte bawah ──────────────────────
static void calculateChecksum(uint8_t* cmd) {
    uint32_t sum = 0;
    for (int i = 0; i < 19; i++) sum += cmd[i];
    cmd[19] = (uint8_t)(sum & 0xFF);
}


// ═══════════════════════════════════════════════════════════════════════════════
// PARSER DATA JK-BMS
// Offset dikunci berdasarkan protokol JK-BD6A24S10P yang telah diverifikasi.
// Jika tipe BMS berbeda, offset perlu diperiksa ulang dengan sniffer BLE.
// ═══════════════════════════════════════════════════════════════════════════════
static void parseBMSData(const uint8_t* data, int len) {
    // Minimal 300 byte — data lengkap protokol JK
    if (len < 300) {
        Serial.printf("[BMS] ✗ Buffer terlalu pendek: %d byte\n", len);
        return;
    }
    // Byte ke-4 harus 0x02 (response tipe data)
    if (data[4] != 0x02) {
        Serial.printf("[BMS] ✗ Bukan response data (byte[4]=0x%02X)\n", data[4]);
        return;
    }

    // Tegangan 24 sel (+6, stride 2 byte per sel)
    for (int i = 0; i < 24; i++) {
        dataToSend.cellVoltages[i] = readU16LE(data, 6 + (i * 2));
    }

    // Rata-rata & imbalance dari BMS langsung (+74, +76)
    dataToSend.avgCellVolt   = readU16LE(data, 74);
    dataToSend.cellVoltDelta = readU16LE(data, 76);

    // Wire resistance 24 sel (+80, stride 2 byte)
    for (int i = 0; i < 24; i++) {
        dataToSend.wireRes[i] = readU16LE(data, 80 + (i * 2));
    }

    // Suhu — 0.1°C per unit
    dataToSend.tempMOS = readS16LE(data, 144);
    dataToSend.tempT1  = readS16LE(data, 162);
    dataToSend.tempT2  = readS16LE(data, 164);

    // Arus — mA, signed (+158)
    dataToSend.current = readS32LE(data, 158);

    // SOC % (+173)
    dataToSend.soc = data[173];

    // Kapasitas — mAh (+174 sisa, +178 nominal)
    dataToSend.remainCapacity  = readU32LE(data, 174);
    dataToSend.nominalCapacity = readU32LE(data, 178);

    // Siklus charge (+182)
    dataToSend.cycleCount = readU32LE(data, 182);

    // Tegangan pack — centi-Volt (+234)
    dataToSend.packVoltage = readU32LE(data, 234);

    g_bmsDataReady = true;

    Serial.printf("[BMS] SOC:%d%% | %.2fA | %.2fV | T1:%.1f°C | Siklus:%u\n",
        dataToSend.soc,
        dataToSend.current      / 1000.0f,
        dataToSend.packVoltage  / 100.0f,
        dataToSend.tempT1       / 10.0f,
        dataToSend.cycleCount);
    Serial.printf("      AVG:%dmV | DELTA:%dmV | WireRes[0]:%dmOhm\n",
        dataToSend.avgCellVolt,
        dataToSend.cellVoltDelta,
        dataToSend.wireRes[0]);
}


// ═══════════════════════════════════════════════════════════════════════════════
// BLE NOTIFY CALLBACK
// Akumulasi notifikasi BLE (paket kecil-kecil) ke buffer, lalu cari header
// JK-BMS: 55 AA EB 90
// ═══════════════════════════════════════════════════════════════════════════════
static void notifyCallback(NimBLERemoteCharacteristic* pChar,
                           uint8_t* pData, size_t length, bool isNotify) {
    if (g_bmsBufferLen + (int)length > (int)sizeof(g_bmsBuffer)) {
        // Overflow — buang buffer, mulai dari awal
        Serial.println("[BLE] Buffer overflow, reset.");
        g_bmsBufferLen = 0;
    }
    memcpy(g_bmsBuffer + g_bmsBufferLen, pData, length);
    g_bmsBufferLen += length;

    // Cari header JK-BMS: 55 AA EB 90
    int headerIdx = -1;
    for (int i = 0; i <= g_bmsBufferLen - 4; i++) {
        if (g_bmsBuffer[i]   == 0x55 && g_bmsBuffer[i+1] == 0xAA &&
            g_bmsBuffer[i+2] == 0xEB && g_bmsBuffer[i+3] == 0x90) {
            headerIdx = i;
            break;
        }
    }

    if (headerIdx != -1 && (g_bmsBufferLen - headerIdx) >= 300) {
        parseBMSData(g_bmsBuffer + headerIdx, g_bmsBufferLen - headerIdx);
        int processed = headerIdx + 300;
        if (g_bmsBufferLen > processed) {
            memmove(g_bmsBuffer, g_bmsBuffer + processed,
                    g_bmsBufferLen - processed);
            g_bmsBufferLen -= processed;
        } else {
            g_bmsBufferLen = 0;
        }
    }

    // Safety flush jika buffer terakumulasi terlalu besar tanpa header valid
    if (g_bmsBufferLen > 1500) {
        Serial.println("[BLE] Buffer besar tanpa header valid, flush.");
        g_bmsBufferLen = 0;
    }
}


// ═══════════════════════════════════════════════════════════════════════════════
// BLE CLIENT CALLBACKS
// [FIX] onDisconnect set g_pRemoteChar = nullptr untuk cegah akses stale pointer
// ═══════════════════════════════════════════════════════════════════════════════
class BMSClientCallbacks : public NimBLEClientCallbacks {
public:
    void onConnect(NimBLEClient* pClient) override {
        g_connected  = true;
        g_hasWokenUp = false;
        Serial.println("[BLE] ✓ Terhubung ke JK-BMS!");
    }

    void onDisconnect(NimBLEClient* pClient, int reason) override {
        g_connected    = false;
        g_hasWokenUp   = false;
        g_bmsBufferLen = 0;
        // [FIX] Null-kan pointer — cegah writeValue ke pointer stale
        g_pRemoteChar  = nullptr;
        Serial.println("[BLE] ✗ Terputus dari JK-BMS.");
    }
};


// ═══════════════════════════════════════════════════════════════════════════════
// KONEKSI BLE KE JK-BMS VIA MAC ADDRESS
// ═══════════════════════════════════════════════════════════════════════════════
static bool connectToBMS() {
    // [FIX] Hapus client lama jika ada sebelum membuat yang baru
    if (g_pClient != nullptr) {
        if (g_pClient->isConnected()) {
            g_pClient->disconnect();
        }
        NimBLEDevice::deleteClient(g_pClient);
        g_pClient = nullptr;
    }

    Serial.printf("[BLE] Menghubungkan ke %s...\n", TARGET_MAC_ADDRESS);
    g_pClient = NimBLEDevice::createClient();
    if (!g_pClient) {
        Serial.println("[BLE] ✗ Gagal membuat BLE client.");
        return false;
    }

    g_pClient->setClientCallbacks(new BMSClientCallbacks(), true);

    if (!g_pClient->connect(NimBLEAddress(TARGET_MAC_ADDRESS, BLE_ADDR_PUBLIC))) {
        Serial.println("[BLE] ✗ Gagal terhubung — cek apakah BMS aktif dan MAC benar.");
        NimBLEDevice::deleteClient(g_pClient);
        g_pClient = nullptr;
        return false;
    }

    NimBLERemoteService* pService = g_pClient->getService(g_serviceUUID);
    if (!pService) {
        Serial.println("[BLE] ✗ Service FFE0 tidak ditemukan.");
        g_pClient->disconnect();
        return false;
    }

    g_pRemoteChar = pService->getCharacteristic(g_charUUID);
    if (!g_pRemoteChar) {
        Serial.println("[BLE] ✗ Characteristic FFE1 tidak ditemukan.");
        g_pClient->disconnect();
        return false;
    }

    if (g_pRemoteChar->canNotify()) {
        g_pRemoteChar->subscribe(true, notifyCallback);
        Serial.println("[BLE] ✓ Subscribe data stream berhasil.");
    } else {
        Serial.println("[BLE] ⚠ Characteristic tidak support notify.");
    }

    return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
// INISIALISASI AXP2101 via XPowersLib
// ═══════════════════════════════════════════════════════════════════════════════
static bool initPMU() {
    // XPowersLib: begin(wire, sda, scl, addr)
    if (!pmu.begin(Wire, AXP2101_SLAVE_ADDRESS, OLED_SDA, OLED_SCL)) {
        Serial.println("[PMU] ✗ AXP2101 tidak terdeteksi!");
        return false;
    }
    // ALDO3 = 3.3V → GPS
    pmu.setALDO3Voltage(3300);
    pmu.enableALDO3();
    // ALDO2 = 3.3V → LoRa
    pmu.setALDO2Voltage(3300);
    pmu.enableALDO2();
    // ALDO4 = 3.3V → OLED
    pmu.setALDO4Voltage(3300);  // OLED / lainnya
    pmu.enableALDO4();
    // Tambahkan setelah enableALDO4():
    // pmu.enableDCDC1();
    // pmu.setDCDC1Voltage(3300);
    // ALDO4 juga bisa untuk sensor eksternal lain jika tidak pakai OLED, tinggal ganti pinout di hardware
    // DCDC3 — ESP32 core, jangan dimatikan
    // pmu.enableDCDC1();
    // Aktifkan deteksi baterai & pengukuran tegangan untuk monitoring level baterai pada ttgo tbeam v1.2
    pmu.enableBattDetection();
    pmu.enableBattVoltageMeasure();

    Serial.println("[PMU] ✓ AXP2101 OK — GPS, LoRa, OLED dinyalakan.");
    return true;
}


// ═══════════════════════════════════════════════════════════════════════════════
// SETUP
// ═══════════════════════════════════════════════════════════════════════════════
void setup() {
    Serial.begin(115200);
    delay(500);

    dataToSend.nodeId = NODE_ID;
    calculateChecksum(g_CMD_WAKEUP);
    calculateChecksum(g_CMD_GET_DATA);

    Serial.printf("\n[INFO] T-Beam Transmitter — NODE ID: %d\n", NODE_ID);
    Serial.printf("[INFO] sizeof(PayloadBMS) = %d byte\n", sizeof(PayloadBMS));
    Serial.printf("[INFO] Target BMS MAC: %s\n\n", TARGET_MAC_ADDRESS);

    // ── PMU dulu — Wire.begin() dihandle di dalam pmu.begin()
    // TIDAK ada Wire.begin() di sini
    if (!initPMU()) {
        Serial.println("[WARN] Lanjut tanpa PMU.");
    }
    delay(500);

    // ── OLED — langsung setelah PMU stabil
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) {
        Serial.println("[OLED] ✗ Gagal.");
    } else {
        Serial.println("[OLED] ✓ OK");
        display.clearDisplay();
        display.setTextSize(1);
        display.setTextColor(SSD1306_WHITE);
        display.setCursor(0, 0);
        display.printf("NODE ID : %d\n", NODE_ID);
        display.printf("Payload : %d byte\n", sizeof(PayloadBMS));
        display.println("Memulai...");
        display.display();
    }

    // ── GPS
    gpsSerial.begin(9600, SERIAL_8N1, GPS_RX, GPS_TX);
    Serial.println("[GPS] ✓ UART1 GPS siap.");

    // ── LoRa
    SPI.begin(LORA_SCK, LORA_MISO, LORA_MOSI, LORA_CS);
    LoRa.setPins(LORA_CS, LORA_RST, LORA_DIO0);
    if (!LoRa.begin(LORA_FREQ)) {
        Serial.println("[LoRa] ✗ Gagal!");
    } else {
        LoRa.setSpreadingFactor(7);
        LoRa.setSignalBandwidth(250E3);
        LoRa.setCodingRate4(5);
        LoRa.setTxPower(20);
        LoRa.setSyncWord(0xF3);
        Serial.println("[LoRa] ✓ Siap SF7 BW250 CR4/5 20dBm");
    }

    // ── BLE
    NimBLEDevice::init("");
    NimBLEDevice::setPower(ESP_PWR_LVL_P9);
    connectToBMS();

    Serial.println("[INFO] Setup selesai.\n");
}
// ═══════════════════════════════════════════════════════════════════════════════
// LOOP UTAMA
// ═══════════════════════════════════════════════════════════════════════════════
void loop() {
    unsigned long now = millis();

    // ── 1. Baca GPS ──────────────────────────────────────────────────────────
    while (gpsSerial.available() > 0) {
        gps.encode(gpsSerial.read());
    }
    dataToSend.lat = gps.location.isValid() ? (int32_t)(gps.location.lat() * 1e7) : 0;
    dataToSend.lon = gps.location.isValid() ? (int32_t)(gps.location.lng() * 1e7) : 0;
// ── 1b. Debug status GPS (independen dari siklus BLE/LoRa) ──────────────
    if (now - g_lastGpsDebug > GPS_DEBUG_MS) {
        g_lastGpsDebug = now;
        Serial.printf(
            "[GPS] sats:%d  hdop:%.1f  fix:%s  charsProcessed:%lu  sentencesWithFix:%lu  failedChecksum:%lu\n",
            gps.satellites.isValid() ? gps.satellites.value() : 0,
            gps.hdop.isValid()       ? gps.hdop.hdop()        : 0.0,
            gps.location.isValid()   ? "YA" : "TIDAK",
            gps.charsProcessed(),
            gps.sentencesWithFix(),
            gps.failedChecksum());
    }
    // ── 2. State machine BLE ─────────────────────────────────────────────────
    if (!g_connected) {
        // Coba reconnect setiap BLE_RECONNECT_MS
        if (now - g_lastBleRetry > BLE_RECONNECT_MS) {
            g_lastBleRetry = now;
            Serial.println("[BLE] Mencoba reconnect...");
            connectToBMS();
        }
    } else {
        if (!g_hasWokenUp) {
            // Kirim wakeup signal satu kali setelah connect
            if (g_pRemoteChar != nullptr) {
                Serial.println("[BLE] Kirim Wakeup signal...");
                g_pRemoteChar->writeValue(g_CMD_WAKEUP, sizeof(g_CMD_WAKEUP), false);
                g_hasWokenUp  = true;
                g_lastRequest = now;
            }
        } else if (now - g_lastRequest > BLE_REQUEST_MS) {
            // Polling GET_DATA setiap BLE_REQUEST_MS
            if (g_pRemoteChar != nullptr) {
                g_pRemoteChar->writeValue(g_CMD_GET_DATA, sizeof(g_CMD_GET_DATA), false);
                g_lastRequest = now;
            }
        }
    }

    // ── 3. Kirim LoRa jika data BMS tersedia ─────────────────────────────────
    if (g_bmsDataReady && (now - g_lastLoRaSend > LORA_SEND_MS)) {
        dataToSend.batPercent = pmu.getBatteryPercent();
        LoRa.beginPacket();
        size_t written = LoRa.write((uint8_t*)&dataToSend, sizeof(PayloadBMS));
        bool txOK = LoRa.endPacket();

    if (txOK) {
    Serial.printf("[TX] NODE:%d | SOC:%d%% | %.2fA | %.2fV | "
                  "Delta:%dmV | Siklus:%u | GPS:%s\n",
            dataToSend.nodeId, dataToSend.soc,
            dataToSend.current      / 1000.0f,
            dataToSend.packVoltage  / 100.0f,
            dataToSend.cellVoltDelta,
            dataToSend.cycleCount,
            dataToSend.lat != 0 ? "FIX" : "NO FIX");
        Serial.printf("     WireRes[0]:%dmO | %d byte terkirim\n",
            dataToSend.wireRes[0], (int)written);
    } else {
    Serial.println("[TX] ✗ LoRa endPacket gagal!");
}

        g_lastLoRaSend = now;
        g_bmsDataReady = false;

        // Update OLED
        display.clearDisplay();
        display.setCursor(0, 0);
        display.printf("NODE  : %d\n",    dataToSend.nodeId);
        display.printf("SOC   : %d%%\n",  dataToSend.soc);
        display.printf("ARUS  : %.1fA\n", dataToSend.current / 1000.0f);
        display.printf("PACK  : %.2fV\n", dataToSend.packVoltage / 100.0f);
        display.printf("T1    : %.1fC\n", dataToSend.tempT1 / 10.0f);
        display.printf("BLE   : %s\n",    g_connected ? "OK" : "DISC");
        display.printf("GPS   : %s\n",    dataToSend.lat != 0 ? "FIX" : "NO FIX");
        display.printf("LoRa  : %s\n",    txOK ? "TX OK" : "TX FAIL");
        display.display();
    }

    delay(50);  // yield — lebih pendek dari 100ms untuk responsivitas GPS
}
