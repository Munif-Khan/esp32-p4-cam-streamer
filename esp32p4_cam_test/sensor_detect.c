#include "esp_cam_sensor.h"
#include "esp_cam_sensor_types.h"
#include "esp_cam_sensor_detect.h"
#include "imx219.h"

static esp_cam_sensor_detect_fn_t custom_cam_sensors[] = {
    {
        .detect = (esp_cam_sensor_device_t *(*)(void *))imx219_detect,
        .port = ESP_CAM_SENSOR_MIPI_CSI,
        .sccb_addr = 0x10,
    }
};

void esp_cam_sensor_detect_get_array(esp_cam_sensor_detect_fn_t **array_start_ptr, esp_cam_sensor_detect_fn_t **array_end_ptr)
{
    *array_start_ptr = &custom_cam_sensors[0];
    *array_end_ptr = &custom_cam_sensors[1];
}
