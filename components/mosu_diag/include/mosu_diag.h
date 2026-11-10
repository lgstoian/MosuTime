/*
 * SPDX-FileCopyrightText: 2026 Stoian Alexandru
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include "esp_err.h"
#include "mosu_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * A small line console on the USB/UART port.  It is for the person who
 * maintains the watch, never for the wearer: the watch face has no menus.
 */
esp_err_t mosu_diag_start(mosu_config_t *cfg);

#ifdef __cplusplus
}
#endif
