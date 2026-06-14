#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

esp_err_t net_eth_init(void);
esp_err_t net_eth_wait_for_ip(uint32_t timeout_ms);
bool net_eth_is_up(void);
const char *net_eth_get_ip_str(void);
