#pragma once
#include "esp_err.h"
#include "payphone.h"
esp_err_t telephony_coin_inject(int line, int cents, payphone_src_t src);
