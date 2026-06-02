#pragma once
#include "struct.h"

// Encode and transmit processed data from Dados ESP to Display ESP.
void comms_send(const MainData &m, const CompData &c);

// Receive processed data on Display ESP.
bool comms_receive(MainData &outMain, CompData &outComp);
