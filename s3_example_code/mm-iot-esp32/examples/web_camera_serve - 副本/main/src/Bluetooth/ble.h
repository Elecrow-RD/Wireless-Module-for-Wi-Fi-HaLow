#ifndef _BLE_H_
#define _BLE_H_

#include "esp_err.h"

void bt_init();
void bt_deinit();
esp_err_t start_Adv(void);
esp_err_t stop_Adv(void);
esp_err_t ble_disconnect(void);

#endif  //__BSP_BLUETOOTH_H_