#pragma once
#include "acquisition.h"
#include "struct.h"

void process_data(const RawData &raw, MainData &outMain, CompData &outComp);
