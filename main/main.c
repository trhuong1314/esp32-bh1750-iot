#include <stdio.h>
#include "esp_mac.h"
#include "time.h"
#include "esp_err.h"
#include "esp_event_base.h"
#include "esp_log_timestamp.h"
#include "esp_netif.h"
#include "esp_netif_types.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "driver/i2c_master.h"
#include "bh1750.h"
#include "hal/gpio_types.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "portmacro.h"
#include "soc/clk_tree_defs.h"
#include "esp_event.h"
#include "nvs_flash.h"
#include "esp_system.h"
#include "lwip/err.h"
#include "lwip/sys.h"
#include "esp_wifi.h"
#include "cJSON.h"
#include "esp_http_client.h"


static const char *TAG = "MAIN";

// Cấu hình sever
#define SERVER_URL_NEW_DEVCIE "https://iot-light-monitoring.onrender.com/api/devices"
#define SERVER_URL_NEW_SENSOR_DATA "https://iot-light-monitoring.onrender.com/api/light-levels"

// Cấu hình gửi dữ liệu
#define chu_ki_gui 30000		// gửi mỗi 30 giây

// Cấu hình Wi-fi
#define WIFI_SSID "Nha 156"
#define WIFI_PASS "156156156"
#define WIFI_MAX_RETRY 5

// Các bit event group
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT BIT1

// Các thông số vật lý cho I2C trên ESP
#define SCL_IO_NUM 22
#define SDA_IO_NUM 21
#define I2C_PORT -1			// auto selecting
#define ADDR_DEV 0x23		// địa chỉ thiết bị
#define BOOT GPIO_NUM_0 

// Cấu hình đọc
#define chu_ki_doc 1000		// đọc mỗi 1 giây
// Chọn chế độ đo (comment/bỏ comment để chọn)
#define USE_MODE             BH1750_Continuously_H_Resolution_Mode    // Continuous 1lx

// #define USE_MODE             BH1750_Continuously_H_Resolution_Mode2   // Continuous 0.5lx
// #define USE_MODE             BH1750_Continuously_L_Resolution_Mode    // Continuous 4lx
// #define USE_MODE             BH1750_One_Time_H_Resolution_Mode     // One-time 1lx
// #define USE_MODE             BH1750_One_Time_H_Resolution_Mode2    // One-time 0.5lx
// #define USE_MODE             BH1750_One_Time_L_Resolution_Mode     // One-time 4lx

// các biến toàn cục 
static i2c_master_bus_handle_t bus_handle = NULL;				
static bh1750_handle_t *bh1750_handle = NULL;
static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;

// Cấu trúc chung cho HTTP POST
typedef struct {
	const char *urrl;
	const char *payload;
	int time_outs;
} http_post_config_t;

// Hàm khởi tạo nút boot
void init_boot_button(){
	gpio_config_t button_boot = {
		.pin_bit_mask = (1UL << BOOT),
		.mode = GPIO_MODE_INPUT,
		.pull_up_en = GPIO_PULLUP_ENABLE,			// Bật điện trở kéo lên để luon ở trạng thái khởi động vào chương trình đã nạp
	};
	gpio_config(&button_boot);
	printf("Đã khởi tạo thành công nút Boot");
}

// Hàm đọc trạng thái của nút boot
// nếu là 1 thì không nhấn còn 0 thì nhấn
int read_button_boot (){
	return gpio_get_level(BOOT);
}

// wifi event-handle
static void wifi_event_handle(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data){
	if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START){
		ESP_LOGI(TAG, "Wi-fi bắt đầu, đang kết nối tới AP ");
		esp_wifi_connect();
	} else if(event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_CONNECTED){
		ESP_LOGI(TAG, "Đã kết nối Wi-fi thành công!");
		s_retry_num = 0;		// reset số lần thử khi đã kết nối thành công
	} else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED){
		if (s_retry_num < WIFI_MAX_RETRY){
			ESP_LOGI(TAG, "Mất kết nối, đang thử lại lần thứ [%d/%d]", s_retry_num + 1, WIFI_MAX_RETRY);
			vTaskDelay(pdMS_TO_TICKS(2000));	// delay 2 giây trước khi thử lại
			esp_wifi_connect();
			s_retry_num++;
		} else {
			ESP_LOGE(TAG, "Kết nối thất bại");
			xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
		}
	} else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP){
		ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
		ESP_LOGI(TAG, "Đã lấy được IP: "IPSTR, IP2STR(&event->ip_info.ip));
		s_retry_num = 0;
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

// khởi tạo wifi
static void wifi_init_sta(void){
	// tạo event group
	s_wifi_event_group = xEventGroupCreate();
	
	// Khởi tạo networking interface
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();
	
	// khởi tạo wifi
	wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));
	
	// đăng ký vào event handle
	esp_event_handler_instance_t instace_any_id;
	esp_event_handler_instance_t instace_got_ip;
	// đăng ký handle cho tất cả event wifi
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handle,NULL, &instace_any_id ));
	// đăng ký handle riêng cho event có ip
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handle, NULL, &instace_got_ip));
	
	// cấu hình wifi
	wifi_config_t wifi_config = {
		.sta = {
			.ssid = WIFI_SSID,
			.password = WIFI_PASS,
			.threshold.authmode = WIFI_AUTH_WPA2_PSK,
			.scan_method = WIFI_ALL_CHANNEL_SCAN,
			.sort_method = WIFI_CONNECT_AP_BY_SIGNAL,
		},
	};
	
	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());
	
	ESP_LOGI(TAG, "wìi đã được khởi tạo thành công chờ kết nối");
	
	// Chờ kết nối cho đến khi có WIFI_CONNECTED_BIT hoặc WIFI_FAIL_BIT
	EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE, portMAX_DELAY);
	if (bits & WIFI_CONNECTED_BIT) {
		ESP_LOGI(TAG, "Kết nối tới điểm truy cập: %s thành công", WIFI_SSID);
	} else if (bits & WIFI_FAIL_BIT){
		ESP_LOGE(TAG, "Kết nối tới điểm truy cập: %s thất bại", WIFI_SSID);
	}
	
}

// Khởi tạo bus i2c
static esp_err_t i2c_bus_init(void){
	i2c_master_bus_config_t i2c_mst_config = {
		.clk_source = I2C_CLK_SRC_DEFAULT,
		.i2c_port = I2C_PORT,
		.scl_io_num = SCL_IO_NUM,
		.sda_io_num = SDA_IO_NUM,
		.glitch_ignore_cnt = 7,
		.flags.enable_internal_pullup = true,
	};
	return i2c_new_master_bus(&i2c_mst_config, &bus_handle);
}

// Khởi tạo device BH1750
static esp_err_t bh1750_device_init(void){
    
    // Kiểm tra thiết bị có kết nối không
    ESP_LOGI(TAG, "Đang probe địa chỉ 0x%02x...", ADDR_DEV);
    esp_err_t ret = i2c_master_probe(bus_handle, ADDR_DEV, 1000);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Không tìm thấy thiết bị tại địa chỉ 0x%02x: %s", ADDR_DEV, esp_err_to_name(ret));
        
        // Thử probe địa chỉ còn lại
        uint16_t alt_addr = (ADDR_DEV == 0x23) ? 0x5C : 0x23;
        ESP_LOGI(TAG, "Thử probe địa chỉ 0x%02x...", alt_addr);
        ret = i2c_master_probe(bus_handle, alt_addr, 1000);
        if (ret == ESP_OK) {
            ESP_LOGI(TAG, "Tìm thấy thiết bị tại địa chỉ 0x%02x", alt_addr);
            // Cập nhật lại ADDR_DEV nếu cần
        }
        return ret;
    }
    
    ESP_LOGI(TAG, "Tìm thấy thiết bị tại địa chỉ 0x%02x", ADDR_DEV);
    
    // khởi tạo bh1750
    return bh1750_init(bus_handle, ADDR_DEV, &bh1750_handle);
}

static void print_mode_name(bh1750_command_t mode) {
    switch(mode) {
        case BH1750_Continuously_H_Resolution_Mode:
            ESP_LOGI(TAG, "Mode: Continuous H-Resolution (1lx)");
            break;
        case BH1750_Continuously_H_Resolution_Mode2:
            ESP_LOGI(TAG, "Mode: Continuous H-Resolution2 (0.5lx)");
            break;
        case BH1750_Continuously_L_Resolution_Mode:
            ESP_LOGI(TAG, "Mode: Continuous L-Resolution (4lx)");
            break;
        case BH1750_One_Time_H_Resolution_Mode:
            ESP_LOGI(TAG, "Mode: One-time H-Resolution (1lx)");
            break;
        case BH1750_One_Time_H_Resolution_Mode2:
            ESP_LOGI(TAG, "Mode: One-time H-Resolution2 (0.5lx)");
            break;
        case BH1750_One_Time_L_Resolution_Mode:
            ESP_LOGI(TAG, "Mode: One-time L-Resolution (4lx)");
            break;
        default:
            ESP_LOGI(TAG, "Mode: Unknown");
    }
}

// Hàm lấy địa chỉ MAC của ESP32
static void get_device_id(char *out_buffer, size_t buffer_size){
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out_buffer, buffer_size, "%02X:%02X:%02X:%02X:%02X:%02X", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

// Hàm HTTP POST
static esp_err_t http_post(const http_post_config_t *cfg){
	esp_err_t ret;

	// Cấu hình HTTP client
	esp_http_client_config_t http_cfg = {
		.url = cfg->urrl,
		.method = HTTP_METHOD_POST,		
		.timeout_ms = cfg->time_outs,
		.keep_alive_enable = true,
		.cert_pem = NULL,
		.use_global_ca_store = false,
		.buffer_size = 4096,
	};

	// Khởi tạo HTTP client
	esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
	if (client == NULL){
		ESP_LOGE(TAG, "Không thể khởi tạo HTTP");
		return ESP_FAIL;
	}

	// thêm header cho request và đặt dữ liệu body cho request
	esp_http_client_set_header(client, "Content-Type", "application/json");
	esp_http_client_set_post_field(client, cfg->payload, strlen(cfg->payload));

	// gửi request
	ret = esp_http_client_perform(client);
	if (ret == ESP_OK){
		int status_code = esp_http_client_get_status_code(client);
		ESP_LOGI(TAG, "HTTP status: %d", status_code);
		if (status_code == 200 || status_code == 201){
			ESP_LOGI(TAG, "Thành công");
		} else {
			ESP_LOGW(TAG, "sever trả về status: %d", status_code);
			ret = ESP_FAIL;
		}
	} else {
		ESP_LOGE(TAG, "HTTP resquest thất bại: %s", esp_err_to_name(ret));
	}

	// dọn dẹp
	esp_http_client_cleanup(client);
	return  ret;
}

// Hàm đăng ký thiết bị với sever
static esp_err_t register_device (void){

    // lấy device_id
    char device_id[18];
    get_device_id(device_id, sizeof(device_id));

	// Tạo 1 JSON object
	cJSON *root = cJSON_CreateObject();
	if (root == NULL) {
		ESP_LOGE(TAG, "Không thể tạo JSON object");
		return ESP_FAIL;
	}

	// Thêm các trường dữ liệu
	cJSON_AddStringToObject(root, "id", device_id);
	cJSON_AddStringToObject(root, "name", "ESP32_BH1750");
	cJSON_AddStringToObject(root, "location", "Phong_Lab");
	cJSON_AddNumberToObject(root, "measurement_interval", chu_ki_gui / 1000);

	// Chuyển đổi thành chuỗi
	char *json_str = cJSON_Print(root);
	// Giải phóng vùng nhớ của Object
	cJSON_Delete(root);

	if (json_str == NULL){
		ESP_LOGE(TAG, "Không thể chuyến thành chuỗi");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Đăng ký thiết bị: %s", json_str);

	// cấu hình http post
	http_post_config_t cfg = {
		.urrl = SERVER_URL_NEW_DEVCIE,
		.payload = json_str,
		.time_outs = 30000,
	};

	// gửi dữ liệu
	esp_err_t ret = http_post(&cfg);
	free(json_str);
	return ret;
}

// Hàm gửi dữ liệu lên sever 
static esp_err_t send_data_to_server (float lux) {

	// lấy địa chỉ MAC
	char device_id[18];
	get_device_id(device_id, sizeof(device_id));

	// Tạo JSON object
	cJSON *root = cJSON_CreateObject();
	if (root == NULL){
		ESP_LOGE(TAG, "Không thể tạo JSON object");
		return ESP_FAIL;
	}

	// Thêm các trường
	cJSON_AddStringToObject(root, "device_id", device_id);
	cJSON_AddNumberToObject(root, "value", (int)lux);

	// Chuyển đổi thành chuỗi
	char *json_str = cJSON_Print(root);
	cJSON_Delete(root);

	if (json_str == NULL){
		ESP_LOGE(TAG, "Không thể chuyển thành chuỗi JSON");
		return ESP_FAIL;
	}

	ESP_LOGI(TAG, "Gửi dữ liệu: %s", json_str);

	// cấu hình http post
	http_post_config_t cfg = {
		.urrl = SERVER_URL_NEW_SENSOR_DATA,
		.payload = json_str,
		.time_outs = 30000,
	};

	esp_err_t ret = http_post(&cfg);
	free(json_str);
	return ret;
}
// Task đọc lux
static void task_read_lux(void *arg){
	float lux;
	int count = 0;
	int send_count = 0;
	int state_boot_latest = 1; 	// trạng thái lần cuối của boot để là 1 (không nhấn) vì pull-up
	TickType_t last_send_time = 0; 		// biến theo dõi lần gửi cuối
	esp_err_t ret;
	int registered = 0;				// biến đăng ký với 0 là chưa còn 1 là rồi
	
	while(1){
		// Đăng ký thiết bị 1 lần duy nhất khi có WIFI
		if (!registered) {
			EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
			if (bits &WIFI_CONNECTED_BIT){
				ESP_LOGI(TAG, "Đang đăng ký thiết bị...");
				register_device();
				registered = 1;
				vTaskDelay(pdMS_TO_TICKS(1000));
			}
		}
		ret = read_lux(bh1750_handle, &lux);
		if (ret == ESP_OK){
			ESP_LOGI(TAG, "[%d] Light: %.2f lux", ++count, lux);
			// Gửi dữ liệu lên sever theo chu kỳ
			TickType_t now = xTaskGetTickCount();
			if ((now - last_send_time) >= pdMS_TO_TICKS(chu_ki_gui)){
				// Kiểm tra kết nối Wifi
				EventBits_t bits = xEventGroupGetBits(s_wifi_event_group);
				if ((bits & WIFI_CONNECTED_BIT)) {
					ESP_LOGI(TAG, "Đang gửi dữ liệu lần %d", ++send_count);
					send_data_to_server(lux);
				} else {
					ESP_LOGW(TAG, "Không có kết nối Wi-fi, bỏ qua gửi dữ liệu");
				}
				last_send_time = now;
			}
		} else {
			ESP_LOGE(TAG, "[%d] Đọc lỗi: %s", ++count, esp_err_to_name(ret));
		}
		// Đọc nút boot
		int current_state = read_button_boot();
		
		// Nếu nút vừa được nhấn
		if (current_state == 0 && state_boot_latest == 1){
			ESP_LOGI(TAG,"Nút boot đã được nhấn");
		}
		// cập nhật trạng thái
		state_boot_latest = current_state;
		vTaskDelay(pdMS_TO_TICKS(chu_ki_doc));
	}
}

void app_main(void)
{
	// Khởi tạo NVS
	esp_err_t ret = nvs_flash_init();
	if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND){
		ESP_ERROR_CHECK(nvs_flash_erase());
		ret  = nvs_flash_init();
	}
	ESP_ERROR_CHECK(ret);
	
	// Khởi tạo wifi và chờ kết nối
	wifi_init_sta();
	
	// Khởi tạo Bus I2C
	ret = i2c_bus_init();
	if (ret != ESP_OK){
		ESP_LOGE(TAG, "Khởi tạo Bus I2C thất bại : %s", esp_err_to_name(ret));
		return;
	}
	ESP_LOGI(TAG, "Khởi tạo Bus I2C thành công");

		// ===== SCAN I2C =====
	ESP_LOGI(TAG, "Bắt đầu scan I2C...");

	for (int addr = 1; addr < 127; addr++) {
		if (i2c_master_probe(bus_handle, addr, 100) == ESP_OK) {
			ESP_LOGI("SCAN", "Tìm thấy thiết bị tại địa chỉ 0x%02X", addr);
		}
	}

	ESP_LOGI(TAG, "Scan I2C hoàn tất");
	// ====================
	
	// Khởi tạo BH1750
	ret = bh1750_device_init();
	if (ret != ESP_OK){
		ESP_LOGE(TAG, "Khởi tạo sensor BH1750 thất bại: %s", esp_err_to_name(ret));
		return;
	}
	ESP_LOGI(TAG, "Khởi tạo sensor BH1750 thành công");
	
	// Khởi tạo nút Boot
	init_boot_button();
	
	// Đặt chế độ đo theo cấu hình
	ESP_LOGI(TAG, "Đang cài đặt chế độ đo...");
	print_mode_name(USE_MODE);
	    
	ret = bh1750_set_mode(bh1750_handle, USE_MODE);
	if (ret != ESP_OK) {
	    ESP_LOGE(TAG, "Cài đặt chế độ đo thất bại: %s", esp_err_to_name(ret));
	    bh1750_deinit(bh1750_handle);
	    i2c_del_master_bus(bus_handle);
	    return;
	}
	
	// Các task 
	
	// Task 1: đọc lux
	xTaskCreatePinnedToCore(task_read_lux, "read_lux", 4096, NULL, 1, NULL, 1);
	ESP_LOGI(TAG, "Hệ thống đã khởi động đọc mỗi %dms", chu_ki_doc);
}