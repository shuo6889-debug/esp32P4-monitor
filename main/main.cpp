#include <cstdio>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <inttypes.h>

#include "driver/i2c_master.h"
#include "driver/isp.h"
#include "driver/jpeg_encode.h"
#include "esp_cam_ctlr.h"
#include "esp_cam_ctlr_csi.h"
#include "esp_check.h"
#include "esp_attr.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_hosted.h"
#include "esp_ldo_regulator.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "esp_cache.h"
#include "esp_heap_caps.h"
#include "nvs_flash.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "example_sensor_init.h"
#include "example_config.h"

namespace {

constexpr int kWidth = 800;
constexpr int kHeight = 640;
constexpr size_t kRawFrameBytes = kWidth * kHeight * 2;
constexpr size_t kJpegBufferBytes = kRawFrameBytes + 64 * 1024;
constexpr char kApSsid[] = "WAVESHARE-P4";
constexpr char kApPassword[] = "p4camera8";
constexpr char kBoundary[] = "frame";

const char *TAG = "jianshi";
esp_cam_ctlr_handle_t s_camera = nullptr;
constexpr size_t kCaptureBufferCount = 2;
void *s_capture_buffers[kCaptureBufferCount] = {};
size_t s_next_capture_buffer = 0;
QueueHandle_t s_completed_frames = nullptr;
jpeg_encoder_handle_t s_jpeg_encoder = nullptr;
uint8_t *s_jpeg_buffer = nullptr;
size_t s_jpeg_capacity = 0;
size_t s_jpeg_size = 0;
uint32_t s_frame_number = 0;
SemaphoreHandle_t s_frame_mutex = nullptr;

bool IRAM_ATTR camera_get_buffer(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *user_data)
{
    (void)user_data;
    trans->buffer = s_capture_buffers[s_next_capture_buffer];
    trans->buflen = kRawFrameBytes;
    s_next_capture_buffer = (s_next_capture_buffer + 1) % kCaptureBufferCount;
    return false;
}

bool IRAM_ATTR camera_trans_finished(esp_cam_ctlr_handle_t, esp_cam_ctlr_trans_t *trans, void *)
{
    if (s_completed_frames == nullptr || trans == nullptr || trans->buffer == nullptr) {
        return false;
    }
    void *completed_frame = trans->buffer;
    BaseType_t high_task_woken = pdFALSE;
    (void)xQueueSendFromISR(s_completed_frames, &completed_frame, &high_task_woken);
    return high_task_woken == pdTRUE;
}

esp_err_t initialize_camera()
{
    esp_ldo_channel_config_t ldo_config = {};
    ldo_config.chan_id = 3;
    ldo_config.voltage_mv = 2500;
    esp_ldo_channel_handle_t mipi_ldo = nullptr;
    ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&ldo_config, &mipi_ldo), TAG, "MIPI LDO init failed");

    example_sensor_config_t sensor_config = {};
    sensor_config.i2c_port_num = I2C_NUM_0;
    sensor_config.i2c_sda_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SDA_IO;
    sensor_config.i2c_scl_io_num = EXAMPLE_MIPI_CSI_CAM_SCCB_SCL_IO;
    sensor_config.reset_pin = -1;
    sensor_config.pwdn_pin = -1;
    sensor_config.xclk_pin = -1;
    sensor_config.port = ESP_CAM_SENSOR_MIPI_CSI;
    sensor_config.format_name = EXAMPLE_CAM_FORMAT;

    example_sensor_handle_t sensor = {};
    example_sensor_init(&sensor_config, &sensor);
    if (sensor.sccb_handle == nullptr) {
        ESP_LOGE(TAG, "No supported MIPI camera was detected. Check the camera module and SCCB wiring.");
        return ESP_ERR_NOT_FOUND;
    }

    esp_cam_ctlr_csi_config_t csi_config = {};
    csi_config.ctlr_id = 0;
    csi_config.h_res = kWidth;
    csi_config.v_res = kHeight;
    csi_config.lane_bit_rate_mbps = EXAMPLE_MIPI_CSI_LANE_BITRATE_MBPS;
    csi_config.input_data_color_type = CAM_CTLR_COLOR_RAW8;
    // ESP32-P4 revision v1.x does not support color conversion in the CSI
    // bridge. Keep CSI in RAW8 bypass mode; the ISP below converts to RGB565.
    csi_config.output_data_color_type = CAM_CTLR_COLOR_RAW8;
    csi_config.data_lane_num = 2;
    csi_config.queue_items = 1;
    ESP_RETURN_ON_ERROR(esp_cam_new_csi_ctlr(&csi_config, &s_camera), TAG, "CSI controller init failed");

    for (size_t i = 0; i < kCaptureBufferCount; ++i) {
        s_capture_buffers[i] = esp_cam_ctlr_alloc_buffer(s_camera, kRawFrameBytes,
                                                          MALLOC_CAP_DMA | MALLOC_CAP_SPIRAM);
        ESP_RETURN_ON_FALSE(s_capture_buffers[i] != nullptr, ESP_ERR_NO_MEM, TAG,
                           "Camera frame allocation failed");
    }
    s_completed_frames = xQueueCreate(kCaptureBufferCount, sizeof(void *));
    ESP_RETURN_ON_FALSE(s_completed_frames != nullptr, ESP_ERR_NO_MEM, TAG,
                       "Camera frame queue allocation failed");

    esp_cam_ctlr_evt_cbs_t callbacks = {};
    callbacks.on_get_new_trans = camera_get_buffer;
    callbacks.on_trans_finished = camera_trans_finished;
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_register_event_callbacks(s_camera, &callbacks, nullptr), TAG, "Camera callbacks failed");
    ESP_RETURN_ON_ERROR(esp_cam_ctlr_enable(s_camera), TAG, "Camera enable failed");

    esp_isp_processor_cfg_t isp_config = {};
    isp_config.clk_hz = 80000000;
    isp_config.input_data_source = ISP_INPUT_DATA_SOURCE_CSI;
    isp_config.input_data_color_type = ISP_COLOR_RAW8;
    isp_config.output_data_color_type = ISP_COLOR_RGB565;
    isp_config.has_line_start_packet = false;
    isp_config.has_line_end_packet = false;
    isp_config.h_res = kWidth;
    isp_config.v_res = kHeight;
    isp_proc_handle_t isp = nullptr;
    ESP_RETURN_ON_ERROR(esp_isp_new_processor(&isp_config, &isp), TAG, "ISP init failed");
    ESP_RETURN_ON_ERROR(esp_isp_enable(isp), TAG, "ISP enable failed");

    jpeg_encode_engine_cfg_t jpeg_engine_config = {};
    jpeg_engine_config.timeout_ms = 200;
    ESP_RETURN_ON_ERROR(jpeg_new_encoder_engine(&jpeg_engine_config, &s_jpeg_encoder), TAG, "JPEG encoder init failed");

    jpeg_encode_memory_alloc_cfg_t output_config = {};
    output_config.buffer_direction = JPEG_ENC_ALLOC_OUTPUT_BUFFER;
    s_jpeg_buffer = static_cast<uint8_t *>(jpeg_alloc_encoder_mem(kJpegBufferBytes, &output_config, &s_jpeg_capacity));
    ESP_RETURN_ON_FALSE(s_jpeg_buffer != nullptr, ESP_ERR_NO_MEM, TAG, "JPEG buffer allocation failed");

    ESP_RETURN_ON_ERROR(esp_cam_ctlr_start(s_camera), TAG, "Camera start failed");
    return ESP_OK;
}

void camera_task(void *)
{
    ESP_LOGI(TAG, "Camera capture task started");
    jpeg_encode_cfg_t encode_config = {};
    encode_config.width = kWidth;
    encode_config.height = kHeight;
    encode_config.src_type = JPEG_ENCODE_IN_FORMAT_RGB565;
    encode_config.sub_sample = JPEG_DOWN_SAMPLING_YUV422;
    encode_config.image_quality = 70;

    while (true) {
        void *frame = nullptr;
        if (xQueueReceive(s_completed_frames, &frame, portMAX_DELAY) != pdTRUE || frame == nullptr) {
            ESP_LOGW(TAG, "Camera frame queue receive failed");
            continue;
        }
        esp_cache_msync(frame, kRawFrameBytes, ESP_CACHE_MSYNC_FLAG_DIR_M2C);

        uint32_t encoded_size = 0;
        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(200)) == pdTRUE) {
            const esp_err_t encode_result = jpeg_encoder_process(s_jpeg_encoder, &encode_config,
                static_cast<const uint8_t *>(frame), kRawFrameBytes,
                s_jpeg_buffer, s_jpeg_capacity, &encoded_size);
            if (encode_result == ESP_OK) {
                s_jpeg_size = encoded_size;
                ++s_frame_number;
                if (s_frame_number == 1 || (s_frame_number % 100) == 0) {
                    ESP_LOGI(TAG, "Camera stream active: frame=%" PRIu32 ", jpeg=%" PRIu32 " bytes",
                             s_frame_number, encoded_size);
                }
            }
            xSemaphoreGive(s_frame_mutex);
            if (encode_result != ESP_OK) {
                ESP_LOGW(TAG, "JPEG encode failed: %s", esp_err_to_name(encode_result));
            }
        }
    }
}

esp_err_t index_handler(httpd_req_t *request)
{
    static const char page[] =
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>ESP32-P4 Camera</title><style>body{margin:0;background:#111;color:#eee;font:16px sans-serif;text-align:center}"
        "img{display:block;width:100%;height:auto;margin:auto}</style></head>"
        "<body><img src='/stream' alt='Camera stream'></body></html>";
    httpd_resp_set_type(request, "text/html; charset=utf-8");
    return httpd_resp_send(request, page, HTTPD_RESP_USE_STRLEN);
}

esp_err_t stream_handler(httpd_req_t *request)
{
    httpd_resp_set_type(request, "multipart/x-mixed-replace; boundary=frame");
    httpd_resp_set_hdr(request, "Access-Control-Allow-Origin", "*");
    uint8_t *frame_copy = static_cast<uint8_t *>(heap_caps_malloc(kJpegBufferBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if (frame_copy == nullptr) {
        return ESP_ERR_NO_MEM;
    }

    uint32_t last_frame = UINT32_MAX;
    esp_err_t result = ESP_OK;
    while (true) {
        size_t jpeg_size = 0;
        uint32_t frame_number = last_frame;
        if (xSemaphoreTake(s_frame_mutex, pdMS_TO_TICKS(1000)) == pdTRUE) {
            frame_number = s_frame_number;
            jpeg_size = s_jpeg_size;
            if (jpeg_size > 0 && jpeg_size <= kJpegBufferBytes && frame_number != last_frame) {
                memcpy(frame_copy, s_jpeg_buffer, jpeg_size);
            }
            xSemaphoreGive(s_frame_mutex);
        }

        if (jpeg_size > 0 && jpeg_size <= kJpegBufferBytes && frame_number != last_frame) {
            char part_header[96];
            const int header_length = snprintf(part_header, sizeof(part_header),
                "--%s\r\nContent-Type: image/jpeg\r\nContent-Length: %u\r\n\r\n", kBoundary,
                static_cast<unsigned>(jpeg_size));
            result = httpd_resp_send_chunk(request, part_header, header_length);
            if (result == ESP_OK) {
                result = httpd_resp_send_chunk(request, reinterpret_cast<const char *>(frame_copy), jpeg_size);
            }
            if (result == ESP_OK) {
                result = httpd_resp_send_chunk(request, "\r\n", 2);
            }
            if (result != ESP_OK) {
                break;
            }
            last_frame = frame_number;
        } else {
            vTaskDelay(pdMS_TO_TICKS(30));
        }
    }

    free(frame_copy);
    httpd_resp_send_chunk(request, nullptr, 0);
    return result;
}

esp_err_t start_http_server()
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.server_port = 80;
    config.max_uri_handlers = 4;
    config.stack_size = 8192;
    httpd_handle_t server = nullptr;
    esp_err_t result = httpd_start(&server, &config);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP server start failed: %s", esp_err_to_name(result));
        return result;
    }

    httpd_uri_t index_uri = {};
    index_uri.uri = "/";
    index_uri.method = HTTP_GET;
    index_uri.handler = index_handler;
    result = httpd_register_uri_handler(server, &index_uri);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP index handler registration failed: %s", esp_err_to_name(result));
        httpd_stop(server);
        return result;
    }

    httpd_uri_t stream_uri = {};
    stream_uri.uri = "/stream";
    stream_uri.method = HTTP_GET;
    stream_uri.handler = stream_handler;
    result = httpd_register_uri_handler(server, &stream_uri);
    if (result != ESP_OK) {
        ESP_LOGE(TAG, "HTTP stream handler registration failed: %s", esp_err_to_name(result));
        httpd_stop(server);
        return result;
    }

    ESP_LOGI(TAG, "HTTP server listening on port %d", config.server_port);
    return ESP_OK;
}

esp_err_t start_wifi_ap()
{
    esp_err_t result = esp_netif_init();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Network interface init failed: %s", esp_err_to_name(result));
        return result;
    }
    result = esp_event_loop_create_default();
    if (result != ESP_OK && result != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Default event loop init failed: %s", esp_err_to_name(result));
        return result;
    }
    if (esp_netif_create_default_wifi_ap() == nullptr) {
        ESP_LOGE(TAG, "Failed to create default Wi-Fi AP interface");
        return ESP_ERR_NO_MEM;
    }

    // ESP32-P4 has no local Wi-Fi radio. The A kit uses its onboard ESP32-C6
    // over SDIO, so initialize ESP-Hosted and establish the link first.
    const esp_err_t hosted_init_result = static_cast<esp_err_t>(esp_hosted_init());
    if (hosted_init_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted initialization failed: %s", esp_err_to_name(hosted_init_result));
        return hosted_init_result;
    }
    const esp_err_t hosted_connect_result = static_cast<esp_err_t>(esp_hosted_connect_to_slave());
    if (hosted_connect_result != ESP_OK) {
        ESP_LOGE(TAG, "ESP-Hosted C6 connection failed: %s", esp_err_to_name(hosted_connect_result));
        return hosted_connect_result;
    }
    wifi_init_config_t init_config = WIFI_INIT_CONFIG_DEFAULT();
    const esp_err_t wifi_init_result = esp_wifi_init(&init_config);
    if (wifi_init_result != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi initialization failed: %s", esp_err_to_name(wifi_init_result));
        return wifi_init_result;
    }
    wifi_config_t ap_config = {};
    strncpy(reinterpret_cast<char *>(ap_config.ap.ssid), kApSsid, sizeof(ap_config.ap.ssid));
    strncpy(reinterpret_cast<char *>(ap_config.ap.password), kApPassword, sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = strlen(kApSsid);
    ap_config.ap.channel = 6;
    ap_config.ap.max_connection = 4;
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "Wi-Fi AP started: SSID=%s, channel=%u", kApSsid, ap_config.ap.channel);
    return ESP_OK;
}

} // namespace

extern "C" void app_main(void)
{
    esp_err_t nvs_result = nvs_flash_init();
    if (nvs_result == ESP_ERR_NVS_NO_FREE_PAGES || nvs_result == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ESP_ERROR_CHECK(nvs_flash_init());
    } else {
        ESP_ERROR_CHECK(nvs_result);
    }

    const esp_err_t camera_result = initialize_camera();
    if (camera_result != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: %s", esp_err_to_name(camera_result));
        return;
    }
    s_frame_mutex = xSemaphoreCreateMutex();
    ESP_ERROR_CHECK(s_frame_mutex == nullptr ? ESP_ERR_NO_MEM : ESP_OK);
    const BaseType_t camera_task_result = xTaskCreate(camera_task, "camera_capture", 8192, nullptr, 5, nullptr);
    ESP_ERROR_CHECK(camera_task_result == pdPASS ? ESP_OK : ESP_ERR_NO_MEM);

    const esp_err_t wifi_result = start_wifi_ap();
    if (wifi_result != ESP_OK) {
        ESP_LOGE(TAG, "Application stopped because Wi-Fi startup failed: %s", esp_err_to_name(wifi_result));
        return;
    }
    const esp_err_t http_result = start_http_server();
    if (http_result != ESP_OK) {
        ESP_LOGE(TAG, "Application stopped because HTTP startup failed: %s", esp_err_to_name(http_result));
        return;
    }
    ESP_LOGI(TAG, "Connect phone to Wi-Fi %s (password: %s), then open http://192.168.4.1", kApSsid, kApPassword);
}
