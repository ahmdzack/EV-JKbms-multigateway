#pragma once
#include <stdint.h>

/*
 * PayloadBMS — Struct biner 143-byte yang dikirim via LoRa
 * WAJIB identik di transmitter, receiver_halte, dan receiver_workshop.
 */
struct __attribute__((packed)) PayloadBMS {
    uint8_t  nodeId;
    uint32_t packetSeq;         // TAMBAHAN — sequence number dari transmitter, dipakai untuk dedup lintas-gateway
    uint16_t cellVoltages[24];
    uint16_t avgCellVolt;
    uint16_t cellVoltDelta;
    uint16_t wireRes[24];
    int16_t  tempMOS;
    int16_t  tempT1;
    int16_t  tempT2;
    int32_t  current;
    uint32_t packVoltage;
    uint8_t  soc;
    uint32_t remainCapacity;
    uint32_t nominalCapacity;
    uint32_t cycleCount;
    uint8_t  batPercent;
    uint16_t batVoltage;
    int32_t  lat;
    int32_t  lon;
};

static_assert(sizeof(PayloadBMS) == 143,
    "PayloadBMS harus 143 byte — periksa padding atau field yang berubah!");