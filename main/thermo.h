#ifndef THERMO_H
#define THERMO_H

#include <string>
#include <vector>

#include "ds18b20.h"
#include "driver/i2c_types.h"
#include "driver/i2c_master.h"

struct	thermo_info
{
	std::string			name;
	uint64_t			rom_code;
	ds18b20_device_handle_t	device	= nullptr;
	esp_err_t			error_code	= ESP_OK;
	int					errors_count = 0;
	float				value = 0;
	float				sended_value	= 0;	//Последнее отправленное в MQTT значение
};

extern bool RMT_thermo_is_enabled;
extern std::vector<thermo_info>	thermometers;

struct PCF8574_data
{
	i2c_master_dev_handle_t		device	= nullptr;
	uint8_t						state	= 0;
};

extern std::vector<PCF8574_data>	pcf8574;

void	thermo(void* unused);
void	init_thermoHeads();
void	thermo_status(std::ostringstream& ss);
json	thermo_json_status();
void	thermo_log_head(std::ostringstream& ss);
void	thermo_log_data(std::ostringstream& ss);

#endif	//THERMO_H
