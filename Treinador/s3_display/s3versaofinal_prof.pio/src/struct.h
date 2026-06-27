#pragma once
#include <stdint.h>

#pragma pack(push, 1)

struct MainData {
    float sog; // speed over ground
    float heel;
    float trim;
    float heading;   // <--- AQUI ESTÁ O RUMO REAL DA BÚSSOLA (BNO055)
    uint32_t time;
};

#pragma pack(pop)