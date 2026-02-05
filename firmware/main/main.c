/*
 * irTemp - IR Temperature Sensor over Zigbee
 *
 * ESP32-H2 Zigbee End Device that:
 * - Reads MLX90614 IR temperature sensor via I2C
 * - Exposes object and ambient temperature to Home Assistant
 * - Configurable update rate via Zigbee
 * - Identify function flashes LED
 */

#include <stdio.h>
#include <string.h>
#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/timers.h"
#include "driver/gpio.h"
#include "driver/i2c.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

// ============================================================================
// Configuration
// ============================================================================

#define TAG "IRTEMP"

// GPIO
#define LED_GPIO            GPIO_NUM_4
#define I2C_SDA_GPIO        GPIO_NUM_2
#define I2C_SCL_GPIO        GPIO_NUM_1

// I2C
#define I2C_PORT            I2C_NUM_0
#define I2C_FREQ_HZ         100000
#define MLX90614_ADDR       0x5A

// MLX90614 RAM addresses
#define MLX90614_TA         0x06    // Ambient temperature
#define MLX90614_TOBJ1      0x07    // Object temperature

// Zigbee
#define IRTEMP_ENDPOINT     1
#define MANUFACTURER_NAME   "\x09""ESPRESSIF"
#define MODEL_ID            "\x0B""IRTEMP_SENS"

// NVS storage
#define NVS_NAMESPACE       "irtemp"
#define NVS_KEY_UPDATE_RATE "update_rate"

// Update rate (stored internally as milliseconds)
#define DEFAULT_UPDATE_RATE_MS      30000
#define MIN_UPDATE_RATE_MS          250
#define MAX_UPDATE_RATE_MS          600000

// Temperature cluster uses 0.01C units, 0x8000 = invalid
#define TEMP_INVALID        ((int16_t)0x8000)

// ============================================================================
// Zigbee Configuration Macros
// ============================================================================

#define ESP_ZB_ZED_CONFIG() \
    { \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED, \
        .install_code_policy = false, \
        .nwk_cfg.zed_cfg = { \
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN, \
            .keep_alive = 3000, \
        }, \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG() \
    { \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG() \
    { \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }

// ============================================================================
// State
// ============================================================================

// Temperature readings (in 0.01C units for ZCL)
static volatile int16_t g_object_temp = TEMP_INVALID;
static volatile int16_t g_ambient_temp = TEMP_INVALID;

// Configurable update rate (milliseconds)
static volatile uint32_t g_update_rate_ms = DEFAULT_UPDATE_RATE_MS;

// Task handle for waking the temp task on rate change
static TaskHandle_t g_temp_task_handle = NULL;

// Identify state
static volatile bool g_identifying = false;
static volatile uint16_t g_identify_time = 0;
static TimerHandle_t g_identify_timer = NULL;

// ============================================================================
// Forward Declarations
// ============================================================================

static void save_config_to_nvs(void);

// ============================================================================
// NVS Configuration Storage
// ============================================================================

static void load_config_from_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return;
    }

    uint32_t rate_ms;
    if (nvs_get_u32(handle, NVS_KEY_UPDATE_RATE, &rate_ms) == ESP_OK) {
        if (rate_ms >= MIN_UPDATE_RATE_MS && rate_ms <= MAX_UPDATE_RATE_MS) {
            g_update_rate_ms = rate_ms;
            ESP_LOGI(TAG, "Loaded update rate: %lu ms", (unsigned long)g_update_rate_ms);
        }
    }

    nvs_close(handle);
}

static void save_config_to_nvs(void)
{
    nvs_handle_t handle;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle) != ESP_OK) {
        ESP_LOGW(TAG, "Failed to open NVS");
        return;
    }

    nvs_set_u32(handle, NVS_KEY_UPDATE_RATE, g_update_rate_ms);
    nvs_commit(handle);
    nvs_close(handle);
    ESP_LOGI(TAG, "Config saved to NVS");
}

// ============================================================================
// LED Handling
// ============================================================================

static void led_init(void)
{
    gpio_config_t io_conf = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    gpio_set_level(LED_GPIO, 0);
}

static void led_set(bool on)
{
    gpio_set_level(LED_GPIO, on ? 1 : 0);
}

// ============================================================================
// Identify Function
// ============================================================================

static void identify_timer_cb(TimerHandle_t t)
{
    static int flash_count = 0;
    static bool led_on = false;

    if (!g_identifying) {
        flash_count = 0;
        led_on = false;
        led_set(false);
        return;
    }

    // Flash pattern: 2 quick flashes
    // Timer fires every 333ms for snappy blink
    led_on = !led_on;
    led_set(led_on);

    if (!led_on) {
        flash_count++;
        if (flash_count >= 2) {
            g_identifying = false;
            flash_count = 0;
            ESP_LOGI(TAG, "Identify complete");
        }
    }
}

static void start_identify(uint16_t time_sec)
{
    ESP_LOGI(TAG, "Identify triggered for %d seconds", time_sec);
    g_identify_time = time_sec;
    g_identifying = true;

    if (g_identify_timer == NULL) {
        g_identify_timer = xTimerCreate("identify", pdMS_TO_TICKS(333),
                                         pdTRUE, NULL, identify_timer_cb);
    }
    xTimerStart(g_identify_timer, 0);
}

// ============================================================================
// I2C and MLX90614
// ============================================================================

static esp_err_t i2c_init(void)
{
    i2c_config_t conf = {
        .mode = I2C_MODE_MASTER,
        .sda_io_num = I2C_SDA_GPIO,
        .scl_io_num = I2C_SCL_GPIO,
        .sda_pullup_en = GPIO_PULLUP_ENABLE,
        .scl_pullup_en = GPIO_PULLUP_ENABLE,
        .master.clk_speed = I2C_FREQ_HZ,
    };

    esp_err_t ret = i2c_param_config(I2C_PORT, &conf);
    if (ret != ESP_OK) return ret;

    ret = i2c_driver_install(I2C_PORT, I2C_MODE_MASTER, 0, 0, 0);
    if (ret != ESP_OK) return ret;

    // Scan the I2C bus to find connected devices
    ESP_LOGI(TAG, "Scanning I2C bus...");
    int found = 0;
    for (uint8_t addr = 1; addr < 127; addr++) {
        i2c_cmd_handle_t cmd = i2c_cmd_link_create();
        i2c_master_start(cmd);
        i2c_master_write_byte(cmd, (addr << 1) | I2C_MASTER_WRITE, true);
        i2c_master_stop(cmd);
        esp_err_t probe = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(50));
        i2c_cmd_link_delete(cmd);
        if (probe == ESP_OK) {
            ESP_LOGI(TAG, "  Found device at 0x%02X%s", addr,
                     addr == MLX90614_ADDR ? " (MLX90614)" : "");
            found++;
        }
    }
    if (found == 0) {
        ESP_LOGW(TAG, "  No I2C devices found! Check wiring: SDA=GPIO%d SCL=GPIO%d",
                 I2C_SDA_GPIO, I2C_SCL_GPIO);
        ESP_LOGW(TAG, "  Try swapping SDA/SCL if sensor is connected but not detected");
    }

    return ESP_OK;
}

static esp_err_t mlx90614_read_temp(uint8_t reg, float *temp_out)
{
    uint8_t data[3];

    // MLX90614 uses SMBus: write register, then read 3 bytes (LSB, MSB, PEC)
    i2c_cmd_handle_t cmd = i2c_cmd_link_create();
    i2c_master_start(cmd);
    i2c_master_write_byte(cmd, (MLX90614_ADDR << 1) | I2C_MASTER_WRITE, true);
    i2c_master_write_byte(cmd, reg, true);
    i2c_master_start(cmd);  // Repeated start
    i2c_master_write_byte(cmd, (MLX90614_ADDR << 1) | I2C_MASTER_READ, true);
    i2c_master_read(cmd, data, 3, I2C_MASTER_LAST_NACK);
    i2c_master_stop(cmd);

    esp_err_t ret = i2c_master_cmd_begin(I2C_PORT, cmd, pdMS_TO_TICKS(100));
    i2c_cmd_link_delete(cmd);

    if (ret != ESP_OK) {
        return ret;
    }

    // Convert to temperature: raw * 0.02 - 273.15
    uint16_t raw = (data[1] << 8) | data[0];
    *temp_out = (raw * 0.02f) - 273.15f;

    return ESP_OK;
}

// Convert float celsius to ZCL temperature (0.01C units)
static int16_t celsius_to_zcl(float celsius)
{
    // ZCL temperature is in units of 0.01C
    // Range: -273.15C to 327.67C
    if (celsius < -273.15f || celsius > 327.67f) {
        return TEMP_INVALID;
    }
    return (int16_t)(celsius * 100.0f);
}

// ============================================================================
// Zigbee Signal Handler
// ============================================================================

static void bdb_start_cb(uint8_t mode)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *sig)
{
    esp_zb_app_signal_type_t type = *sig->p_app_signal;
    esp_err_t status = sig->esp_err_status;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting network steering...");
                esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Rejoined network");
            }
        } else {
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Joined! PAN:0x%04hx Ch:%d",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
        } else {
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        break;
    }
}

// ============================================================================
// Zigbee Action Handler
// ============================================================================

static esp_err_t zb_attr_handler(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    if (!msg || msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_FAIL;

    ESP_LOGI(TAG, "Attr write: ep=%d cluster=0x%04x attr=0x%04x",
             msg->info.dst_endpoint, msg->info.cluster, msg->attribute.id);

    // Identify time attribute
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_IDENTIFY &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_IDENTIFY_IDENTIFY_TIME_ID) {
        uint16_t time = *(uint16_t*)msg->attribute.data.value;
        if (time > 0) {
            start_identify(time);
        } else {
            g_identifying = false;
            led_set(false);
        }
    }

    // Update rate (Analog Input present_value, in seconds as float)
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID) {
        float seconds = *(float*)msg->attribute.data.value;
        uint32_t ms = (uint32_t)(seconds * 1000.0f);
        if (ms < MIN_UPDATE_RATE_MS) ms = MIN_UPDATE_RATE_MS;
        if (ms > MAX_UPDATE_RATE_MS) ms = MAX_UPDATE_RATE_MS;
        g_update_rate_ms = ms;
        ESP_LOGI(TAG, "Update rate: %lu ms (%.2f sec)", (unsigned long)ms, seconds);
        save_config_to_nvs();
        // Wake the temp task so new rate takes effect immediately
        if (g_temp_task_handle) {
            xTaskNotifyGive(g_temp_task_handle);
        }
    }

    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t id, const void *msg)
{
    if (id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID) {
        return zb_attr_handler((esp_zb_zcl_set_attr_value_message_t*)msg);
    }
    return ESP_OK;
}

// ============================================================================
// Zigbee Clusters
// ============================================================================

static esp_zb_cluster_list_t *create_main_clusters(void)
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    // Basic cluster
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                   (void*)MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                   (void*)MODEL_ID);
    esp_zb_cluster_list_add_basic_cluster(list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Identify cluster
    esp_zb_identify_cluster_cfg_t id_cfg = { .identify_time = 0 };
    esp_zb_attribute_list_t *identify = esp_zb_identify_cluster_create(&id_cfg);
    esp_zb_cluster_list_add_identify_cluster(list, identify, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Temperature Measurement cluster (Object temperature)
    // ZCL int16 max is 32767 = 327.67C (MLX90614 goes to 380C but ZCL can't)
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = TEMP_INVALID,
        .min_value = -7000,   // -70.00 C (MLX90614 min)
        .max_value = 32767,   // 327.67 C (int16_t max)
    };
    esp_zb_attribute_list_t *temp = esp_zb_temperature_meas_cluster_create(&temp_cfg);
    esp_zb_cluster_list_add_temperature_meas_cluster(list, temp, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Analog Input cluster (Update rate setting, value in seconds)
    esp_zb_analog_input_cluster_cfg_t ai_cfg = {
        .out_of_service = false,
        .present_value = (float)g_update_rate_ms / 1000.0f,
    };
    esp_zb_attribute_list_t *ai = esp_zb_analog_input_cluster_create(&ai_cfg);
    static char ai_desc[] = "\x13""Update Rate Seconds";
    esp_zb_analog_input_cluster_add_attr(ai, ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, ai_desc);
    esp_zb_cluster_list_add_analog_input_cluster(list, ai, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return list;
}

static esp_zb_cluster_list_t *create_ambient_clusters(void)
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    // Basic cluster (minimal for second endpoint)
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    static char ambient_model[] = "\x0C""IRTEMP_AMBNT";
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID,
                                   (void*)MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic, ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID,
                                   (void*)ambient_model);
    esp_zb_cluster_list_add_basic_cluster(list, basic, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // Temperature Measurement cluster (Ambient temperature)
    esp_zb_temperature_meas_cluster_cfg_t temp_cfg = {
        .measured_value = TEMP_INVALID,
        .min_value = -4000,   // -40.00 C (MLX90614 ambient min)
        .max_value = 12500,   // 125.00 C (MLX90614 ambient max)
    };
    esp_zb_attribute_list_t *temp = esp_zb_temperature_meas_cluster_create(&temp_cfg);
    esp_zb_cluster_list_add_temperature_meas_cluster(list, temp, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return list;
}

#define ENDPOINT_OBJECT     1
#define ENDPOINT_AMBIENT    2

static esp_zb_ep_list_t *create_endpoints(void)
{
    esp_zb_ep_list_t *ep_list = esp_zb_ep_list_create();

    // Endpoint 1: Object temperature (main endpoint with settings)
    esp_zb_endpoint_config_t cfg1 = {
        .endpoint = ENDPOINT_OBJECT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, create_main_clusters(), cfg1);

    // Endpoint 2: Ambient temperature
    esp_zb_endpoint_config_t cfg2 = {
        .endpoint = ENDPOINT_AMBIENT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_TEMPERATURE_SENSOR_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep_list, create_ambient_clusters(), cfg2);

    return ep_list;
}

// ============================================================================
// Temperature Update Task (reports both endpoints)
// ============================================================================

static void temp_update_task(void *p)
{
    g_temp_task_handle = xTaskGetCurrentTaskHandle();
    vTaskDelay(pdMS_TO_TICKS(3000));  // Wait for Zigbee to be ready

    ESP_LOGI(TAG, "Temperature reporting started");
    ESP_LOGI(TAG, "  Update rate: %lu ms", (unsigned long)g_update_rate_ms);

    float object_temp, ambient_temp;

    while (1) {
        bool object_ok = false;
        bool ambient_ok = false;

        // Read object temperature
        esp_err_t err = mlx90614_read_temp(MLX90614_TOBJ1, &object_temp);
        if (err == ESP_OK) {
            g_object_temp = celsius_to_zcl(object_temp);
            object_ok = true;
        } else {
            ESP_LOGW(TAG, "Failed to read object temp: %s (0x%x)", esp_err_to_name(err), err);
            g_object_temp = TEMP_INVALID;
        }

        // Read ambient temperature
        err = mlx90614_read_temp(MLX90614_TA, &ambient_temp);
        if (err == ESP_OK) {
            g_ambient_temp = celsius_to_zcl(ambient_temp);
            ambient_ok = true;
        } else {
            ESP_LOGW(TAG, "Failed to read ambient temp: %s (0x%x)", esp_err_to_name(err), err);
            g_ambient_temp = TEMP_INVALID;
        }

        ESP_LOGI(TAG, "Obj: %s%.2f C%s  Amb: %s%.2f C%s",
                 object_ok ? "" : "ERR ", object_ok ? object_temp : 0,
                 object_ok ? "" : "",
                 ambient_ok ? "" : "ERR ", ambient_ok ? ambient_temp : 0,
                 ambient_ok ? "" : "");

        // Update and report object temperature (endpoint 1)
        if (object_ok) {
            int16_t obj_val = g_object_temp;
            esp_zb_lock_acquire(portMAX_DELAY);
            esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(ENDPOINT_OBJECT,
                ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                &obj_val, false);
            if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "EP1 set_attr failed: 0x%x", status);
            }
            esp_zb_zcl_report_attr_cmd_t cmd = {
                .zcl_basic_cmd.src_endpoint = ENDPOINT_OBJECT,
                .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
                .clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                .attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
            };
            esp_zb_zcl_report_attr_cmd_req(&cmd);
            esp_zb_lock_release();
        }

        // Small gap to let the stack process the first report
        vTaskDelay(pdMS_TO_TICKS(5));

        // Update and report ambient temperature (endpoint 2)
        if (ambient_ok) {
            int16_t amb_val = g_ambient_temp;
            esp_zb_lock_acquire(portMAX_DELAY);
            esp_zb_zcl_status_t status = esp_zb_zcl_set_attribute_val(ENDPOINT_AMBIENT,
                ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
                ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                &amb_val, false);
            if (status != ESP_ZB_ZCL_STATUS_SUCCESS) {
                ESP_LOGW(TAG, "EP2 set_attr failed: 0x%x", status);
            }
            esp_zb_zcl_report_attr_cmd_t cmd = {
                .zcl_basic_cmd.src_endpoint = ENDPOINT_AMBIENT,
                .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
                .clusterID = ESP_ZB_ZCL_CLUSTER_ID_TEMP_MEASUREMENT,
                .attributeID = ESP_ZB_ZCL_ATTR_TEMP_MEASUREMENT_VALUE_ID,
                .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
            };
            esp_zb_zcl_report_attr_cmd_req(&cmd);
            esp_zb_lock_release();
        }

        // Wait for the update interval, but wake immediately if rate changes
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(g_update_rate_ms));
    }
}

// ============================================================================
// Zigbee Task
// ============================================================================

static void zigbee_task(void *p)
{
    esp_zb_cfg_t cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&cfg);
    esp_zb_device_register(create_endpoints());
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// ============================================================================
// Main
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "IR Temperature Sensor - MLX90614 over Zigbee");

    // Initialize NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_config_from_nvs();
    led_init();

    // Initialize I2C
    ret = i2c_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "I2C init failed: %s", esp_err_to_name(ret));
        ESP_LOGE(TAG, "Check SDA (GPIO%d) and SCL (GPIO%d) connections", I2C_SDA_GPIO, I2C_SCL_GPIO);
    } else {
        ESP_LOGI(TAG, "I2C initialized on SDA=GPIO%d, SCL=GPIO%d", I2C_SDA_GPIO, I2C_SCL_GPIO);
    }

    // Initialize Zigbee platform
    esp_zb_platform_config_t zb_cfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&zb_cfg));

    // Start tasks
    xTaskCreate(temp_update_task, "temp", 4096, NULL, 5, NULL);
    xTaskCreate(zigbee_task, "zigbee", 4096, NULL, 5, NULL);
}
