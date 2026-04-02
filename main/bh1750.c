#include "bh1750.h"
#include "driver/i2c_master.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/projdefs.h"
#include "freertos/task.h"
#include "hal/i2c_types.h"
#include <stdint.h>

static const char *TAG = "BH1750";

// Thời gian master chờ đợi để cảm biến lấy dữ liệu (tối đa)
static const uint32_t wait_times[] = {
	[BH1750_Continuously_H_Resolution_Mode] = 180,
	[BH1750_Continuously_H_Resolution_Mode2] = 180,
	[BH1750_Continuously_L_Resolution_Mode] = 24,
	[BH1750_One_Time_H_Resolution_Mode] = 180,
	[BH1750_One_Time_H_Resolution_Mode2] = 180,
	[BH1750_One_Time_L_Resolution_Mode] = 24
};

// Hàm ghi lệnh xuống BH1750
static esp_err_t bh1750_command_write(bh1750_handle_t *handle, uint8_t command){
	if (!handle || !handle->dev_handle){
		return ESP_ERR_INVALID_ARG;
	};
	
	esp_err_t ret = i2c_master_transmit (handle->dev_handle, &command, 1, 1000);
	
	if (ret != ESP_OK){
		ESP_LOGE(TAG, "Lỗi gửi lệnh 0x%02x: %s", command, esp_err_to_name(ret));
	}
	return ret;
}


// Hàm đọc dữ liệu từ BH1750 và xử lý dữ liệu
static esp_err_t bh1750_read_data(bh1750_handle_t *handle, float *lux){
	// 2 byte dữ liệu gửi về
	uint8_t data[2] = {0};
	
	esp_err_t ret = i2c_master_receive(handle->dev_handle, data, 2, 1000);
	if(ret != ESP_OK){
		ESP_LOGE(TAG, "Đọc dữ liệu về lỗi: %s", esp_err_to_name(ret));
		return ret;
	}
	
	// xử lý dữ liệu đọc về 
	uint16_t raw = (data[0] << 8) | data[1];
	switch (handle->mode) {
		case BH1750_Continuously_H_Resolution_Mode2:
		case BH1750_One_Time_H_Resolution_Mode2:
			*lux = (float)(raw * 0.5) / 1.2;
			break;
		case BH1750_Continuously_H_Resolution_Mode:
		case BH1750_One_Time_H_Resolution_Mode:
			*lux = (float)(raw) / 1.2;
			break;
		case BH1750_Continuously_L_Resolution_Mode:
		case BH1750_One_Time_L_Resolution_Mode:
			*lux = (float)(raw) / 1.2 * 4;
			break;
		default:
			*lux = (float)(raw) / 1.2;
			break;
	}
	ESP_LOGD(TAG, "Duữ liệu: %d -> Lux: %.2f", raw, *lux);
	return ESP_OK;
}

// Hàm khởi tạo BH1750
esp_err_t bh1750_init(i2c_master_bus_handle_t bus_handle, uint16_t address, bh1750_handle_t **out_handle){
	// kiểm tra
	if (!bus_handle || !out_handle){
		return ESP_ERR_INVALID_ARG;
	}
	
	// cấp phát bộ nhớ
	bh1750_handle_t *handle = calloc(1, sizeof(bh1750_handle_t));
	if (!handle){
		return ESP_ERR_NO_MEM;
	}
	
	// cấu hình cho device
	i2c_device_config_t dev_cfg = {
		.dev_addr_length = I2C_ADDR_BIT_LEN_7,
		.device_address = address,
		.scl_speed_hz = 100000,
		.scl_wait_us = 0
	};
	esp_err_t ret = i2c_master_bus_add_device(bus_handle, &dev_cfg, &handle->dev_handle);
	if (ret != ESP_OK){
		ESP_LOGE(TAG, "Lỗi thêm thiết bị: %s", esp_err_to_name(ret));
		free(handle);
		return ret;
	}
	
	// Khởi tạo mặc định để chế độ đo liên tục mức H mode 1
	handle->mode = BH1750_Continuously_H_Resolution_Mode;
	handle->wait_time_us = wait_times[BH1750_Continuously_H_Resolution_Mode];
	
	// Gửi lệnh khởi tạo
	ret = bh1750_command_write(handle, BH1750_Continuously_H_Resolution_Mode);
	if (ret!= ESP_OK){
		i2c_master_bus_rm_device(handle->dev_handle);
		free(handle);
		return ret;
	}
	// Đợi lần đo đầu tiên hoàn tất
	vTaskDelay(pdMS_TO_TICKS(handle->wait_time_us));
	
	ESP_LOGI(TAG, "BH1750 đã được khởi tạo thành công ở địa chỉ 0x%02x", address);
	*out_handle = handle;
	return ESP_OK;
}

// Hàm giải phóng BH1750
esp_err_t bh1750_deinit(bh1750_handle_t *handle){
	if (!handle || !handle->dev_handle){
		return ESP_ERR_INVALID_ARG;
	}
	bh1750_command_write(handle, BH1750_POWER_DOWN);
	esp_err_t ret = i2c_master_bus_rm_device(handle->dev_handle);
	if (ret != ESP_OK){
		ESP_LOGE(TAG, "Gỡ bỏ thiết bị khỏi I2C bus thất bại: %s", esp_err_to_name(ret));
	}
	
	free(handle);
	ESP_LOGI(TAG, "Giải phóng thành công");
	return ret;
}

// Hàm cài đặt chế độ đo
esp_err_t bh1750_set_mode(bh1750_handle_t *handle, bh1750_command_t mode){
	if(!handle) return ESP_ERR_INVALID_ARG;
	if (mode < BH1750_Continuously_H_Resolution_Mode || mode > BH1750_One_Time_L_Resolution_Mode){
		return ESP_ERR_INVALID_ARG;
	}
	esp_err_t ret;
	
	if (handle->mode >= BH1750_One_Time_H_Resolution_Mode){
		// Nếu đang ở chế độ đo 1 lần cần đảm bảo đã ở power_down trước khi chuyển mode về chế độ mới
		bh1750_command_write(handle, BH1750_POWER_DOWN);
		vTaskDelay(pdMS_TO_TICKS(1));
	}
	// gửi lệnh set mode mới
	ret = bh1750_command_write(handle, mode);
	if (ret != ESP_OK) return ret;
	
	// cập nhật handle
	handle->mode = mode;
	handle->wait_time_us = wait_times[mode];
	
	// Nếu là chế độ đo liên tục thì đợi lần đo đầu tiên
	if (mode <= BH1750_Continuously_L_Resolution_Mode){
		vTaskDelay(pdMS_TO_TICKS(handle->wait_time_us));
		ESP_LOGI(TAG, "đã chuuyển sang chế độ đo liên tục (chờ: %dms)", (int)handle->wait_time_us);
	} else {
		ESP_LOGI(TAG, "đã chuuyển sang chế độ đo 1 lần (chờ: %dms)", (int)handle->wait_time_us);
	}
	return ESP_OK;
}

// Hàm đọc giá trị lux
esp_err_t read_lux(bh1750_handle_t *handle, float *lux){
	if (!handle || !handle->dev_handle){
		return ESP_ERR_INVALID_ARG;
	}
	esp_err_t ret;
	
	// Xử lý ở chế độ đo liên tục
	if (handle->mode <= BH1750_Continuously_L_Resolution_Mode){
		// do đã khởi tạo mặc định là chế độ liên tục nên chỉ cần đọc dữ liệu
		return bh1750_read_data(handle, lux);
	}
	
	// Xử lý ở chế độ đo 1 lần
	// Power_On
	ret = bh1750_command_write(handle, BH1750_POWER_ON);
	if (ret != ESP_OK) return ret;
	vTaskDelay(pdMS_TO_TICKS(1));
	
	// Gửi lệnh đo
	ret = bh1750_command_write(handle, handle->mode);
	if (ret != ESP_OK) return ret;
	
	// Chờ đo xong
	vTaskDelay(pdMS_TO_TICKS(handle->wait_time_us));
	
	// đọc kết quả
	ret = bh1750_read_data(handle, lux);
	
	// Tự động về Power Down
	if (ret == ESP_OK){
		bh1750_command_write(handle, BH1750_POWER_DOWN);
	}
	return ret;
	
}
