#ifndef BH1750_H
#define BH1750_H


#include "driver/i2c_master.h"
#include "esp_err.h"
#include <stdint.h>

// Địa chỉ I2C của BH1750
#define BH1750_ADDR_HIGH 0x5C
#define BH1750_ADDR_LOW 0x23

// Phần Instruction Set
typedef enum {
	BH1750_POWER_DOWN = 0x00,
	BH1750_POWER_ON = 0x01,
	BH1750_RESET = 0x07,
	BH1750_Continuously_H_Resolution_Mode = 0x10,
	BH1750_Continuously_H_Resolution_Mode2 = 0x11,
	BH1750_Continuously_L_Resolution_Mode = 0x13,
	BH1750_One_Time_H_Resolution_Mode = 0x20,
	BH1750_One_Time_H_Resolution_Mode2 = 0x21,
	BH1750_One_Time_L_Resolution_Mode = 0x23
} bh1750_command_t;

// Cấu trúc thông tin cho BH1750
typedef struct {
	i2c_master_dev_handle_t dev_handle;			// Handle cho device
	bh1750_command_t mode;						// chế độ hiện tại của device
	uint32_t wait_time_us;						// cache thời gian chờ
} bh1750_handle_t;

// Các hàm của thư viện

// Khởi tạo BH1750
esp_err_t bh1750_init(i2c_master_bus_handle_t bus_handle, uint16_t adddress, bh1750_handle_t **out_handle);

// Hàm giải phóng BH1750
esp_err_t bh1750_deinit(bh1750_handle_t *handle);

// Hàm đọc giá trị lux
esp_err_t read_lux(bh1750_handle_t *handle, float *lux);

// Hamd cài đặt chế độ
esp_err_t bh1750_set_mode(bh1750_handle_t *handle, bh1750_command_t mode);


#endif