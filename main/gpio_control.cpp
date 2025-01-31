#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_spiffs.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "rom/gpio.h"
#include "esp_timer.h"
#include "json.hpp"
using json = nlohmann::json;

#include "telegram.h"
#include "gpio_control.h"
#include "thermo.h"
#include "boiler_task.h"
#include "mqtt.h"
#include "tcp_server.h"

static const char*	TAG = "gpio_control";

//Пины для обогревателей
constexpr gpio_num_t	pin_heater_1		= GPIO_NUM_33;
constexpr gpio_num_t	pin_heater_2		= GPIO_NUM_25;

//Состояние пинов
bool	pin_heater_1_isOn	= false;
bool	pin_heater_2_isOn	= false;

//Заданная температура
float	floor_1_temp_zad	= 7.5;
float	floor_2_temp_zad	= 7.5;
float	temp_zad_dt			= 0.5;
uint8_t	prog_index			= 0;

void	thermostat(const uint8_t& prog_index);

class printTime
{
private:
	uint32_t	dt;
public:
	explicit printTime(const uint32_t& dt) : dt(dt){}
	friend std::ostream& operator<< (std::ostream& ss, const printTime& t)
	{
		uint32_t	dt		= t.dt;
		uint32_t	days	= dt/24/3600;	dt	-= days*24*3600;
		uint32_t	hours	= dt/3600;		dt	-= hours*3600;
		uint32_t	mins	= dt/60;		dt	-= mins*60;
		uint32_t	secs	= dt;

		if(days)
			ss << days << "д ";
		ss << std::setfill('0');
		ss << std::setw(2) << hours << ":";
		ss << std::setw(2) << mins << ":";
		ss << std::setw(2) << secs;
		ss << std::setfill(' ');

		return ss;
	}
};

void	gpio_control(void* unused)
{
	//Настройка GPIO
	gpio_pad_select_gpio(pin_heater_1);
	gpio_set_direction(pin_heater_1, GPIO_MODE_OUTPUT);
	gpio_pullup_dis(pin_heater_1);
	gpio_pulldown_dis(pin_heater_1);
	gpio_set_level(pin_heater_1, 0);

	gpio_pad_select_gpio(pin_heater_2);
	gpio_set_direction(pin_heater_2, GPIO_MODE_OUTPUT);
	gpio_pullup_dis(pin_heater_2);
	gpio_pulldown_dis(pin_heater_2);
	gpio_set_level(pin_heater_2, 0);

	//Чтение настроек программы термостата
	nvs_handle_t	nvs_settings;
	if(nvs_open("thermostat", NVS_READONLY, &nvs_settings) == ESP_OK)
	{
		int16_t	val;
		uint8_t	index;
		if(nvs_get_i16(nvs_settings, "floor_1_temp_zad", &val) == ESP_OK)		floor_1_temp_zad		= 0.01*val;
		if(nvs_get_i16(nvs_settings, "floor_2_temp_zad", &val) == ESP_OK)		floor_2_temp_zad		= 0.01*val;
		if(nvs_get_i16(nvs_settings, "temp_zad_dt", &val) == ESP_OK)			temp_zad_dt				= 0.01*val;
		if(nvs_get_u8(nvs_settings, "prog_index", &index) == ESP_OK)			prog_index				= index;

		nvs_close(nvs_settings);
	}

	/////////////////////////////////////////////////////////////////////
	//  Главный цикл
	for(;;)
	{
		//Алгоритм термостата
		if(prog_index)
			thermostat(prog_index);

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void	thermostat(const uint8_t& prog_index)
{
	//Управление конвекторами по температуре
	if(thermometers.size() < 6)	return;

	// const float	floor_1		= thermometers.at(1).value;
	// const float	floor_2		= thermometers.at(2).value;

	// switch(prog_index)
	// {
	// 	case 1:
	// 	{
	// 		if(floor_1 < floor_1_temp_zad)						floor_1_on();
	// 		else if(floor_1 > floor_1_temp_zad + temp_zad_dt)	floor_1_off();

	// 		if(floor_2 < floor_2_temp_zad)						floor_2_on();
	// 		else if(floor_2 > floor_2_temp_zad + temp_zad_dt)	floor_2_off();
	// 	}break;

	// 	default:
	// 		break;
	// }
}

void	gpio_status(std::ostringstream& ss)
{
	ss << "```" << std::endl;
	ss << "heap = " << esp_get_free_heap_size() << std::endl;

	if(esp_spiffs_mounted(nullptr))
	{
		size_t total = 0, used = 0;
		esp_spiffs_info(nullptr, &total, &used);
		ss << "spiffs total = " << total << std::endl;
		ss << "spiffs used = " << used << std::endl;
	}

	ss << "uptime:  " << printTime(esp_timer_get_time()*0.000001) << std::endl;
	ss << "```";

	ss << "MQTT: " << (mqtt_client ? "connected" : "disconnected") << std::endl;
	ss << "TCP server: " << (tcp_server_is_running() ? "running" : "down") << std::endl;
	ss << "OpenTherm: " << (pBoiler && pBoiler->openTherm_is_correct() ? "ok" : "TIMEOUT") << std::endl;

	//Запрос текущего IP
	esp_netif_t*		sta_netif	= esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
	esp_netif_ip_info_t	ip_info;
	if(esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK)
	{
		ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&ip_info.ip));
		char	ip_addr[16];
		sprintf(ip_addr, IPSTR, IP2STR(&ip_info.ip));
		ss << "IP: " << ip_addr << std::endl;
	}
}
