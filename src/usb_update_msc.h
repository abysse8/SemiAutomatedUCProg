#pragma once

#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    UC_USB_MODE_OFF = 0,
    UC_USB_MODE_M0 = 1,
    UC_USB_MODE_M1 = 2,
} uc_usb_mode_t;

const char *uc_usb_mode_name(uc_usb_mode_t mode);
esp_err_t uc_usb_update_start(uc_usb_mode_t mode);
bool uc_usb_update_active(void);
