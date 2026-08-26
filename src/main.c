#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "nvs_flash.h"

#include "BLETask.h"
#include "blue_led.h"
#include "device_mode.h"

#define TAG "MAIN"

void app_main()
{
	/* Order matters: the LED backs the mode indicator, NVS backs the stored
	 * role, and both must be up before device_mode_init() runs. The BLE task
	 * registers its apply callback once the mesh stack is initialized. */
	blue_led_init();

	esp_err_t err = nvs_flash_init();
	if (err == ESP_ERR_NVS_NO_FREE_PAGES) {
		ESP_ERROR_CHECK(nvs_flash_erase());
		err = nvs_flash_init();
	}
	ESP_ERROR_CHECK(err);

	ESP_ERROR_CHECK(device_mode_init());

	ble_task_init();

	while (1)
	{
		vTaskDelay(10000 / portTICK_PERIOD_MS);
		ESP_LOGI(TAG, "alive, mode=%s", device_mode_name(device_mode_get()));
	}
}
