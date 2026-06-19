#pragma once
#include <stdint.h>

/*
 * PayloadBMS — Struct biner 136-byte yang dikirim via LoRa
 * WAJIB identik di transmitter, receiver_halte, dan receiver_workshop.
 *
 * Gunakan static_assert untuk memastikan ukuran tidak berubah
 * akibat perbedaan compiler/platform.
 */
struct __attribute__((packed)) PayloadBMS {
    uint8_t  nodeId;            // ID kendaraan (1–3)
    uint16_t cellVoltages[24];  // Tegangan 24 sel (mV)
    uint16_t avgCellVolt;       // Rata-rata tegangan sel (mV)
    uint16_t cellVoltDelta;     // Imbalance max-min (mV)
    uint16_t wireRes[24];       // Resistansi kabel 24 sel (mOhm)
    int16_t  tempMOS;           // Suhu MOS (0.1°C)
    int16_t  tempT1;            // Suhu Baterai T1 (0.1°C)
    int16_t  tempT2;            // Suhu Baterai T2 (0.1°C)
    int32_t  current;           // Arus (mA) — positif=charging, negatif=discharging
    uint32_t packVoltage;       // Tegangan total pack (centi-Volt, bagi 100 → Volt)
    uint8_t  soc;               // State of Charge (%)
    uint32_t remainCapacity;    // Kapasitas tersisa (mAh)
    uint32_t nominalCapacity;   // Kapasitas nominal (mAh)
    uint32_t cycleCount;        // Jumlah siklus charge
    uint8_t batPercent;       // Persentase baterai (0-100%) — tambahan untuk monitoring level baterai
    uint16_t batVoltage;      // Tegangan baterai (mV) — tambahan untuk monitoring level baterai
    int32_t    lat;               // Latitude GPS
    int32_t    lon;               // Longitude GPS
};

// Guard — kompilasi gagal jika struct tidak tepat 139 byte
static_assert(sizeof(PayloadBMS) == 139,
    "PayloadBMS harus 139 byte — periksa padding atau field yang berubah!");
