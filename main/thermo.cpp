#include <string>
#include <vector>
#include <sstream>
#include <iomanip>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_timer.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "driver/i2c_types.h"
#include "driver/i2c_master.h"
#include "json.hpp"
using json = nlohmann::json;

#include "secure_config.h"
#include "onewire_bus.h"
#include "ds18b20.h"
#include "thermo.h"
#include "mqtt.h"

static const char*	TAG = "thermo";

//Термометр
bool RMT_thermo_is_enabled	= true;
constexpr	gpio_num_t	pin_ds18b20	= GPIO_NUM_15;	//Разъем Т1
//constexpr	gpio_num_t	pin_ds18b20	= GPIO_NUM_26;	//Разъем Т2
std::vector<thermo_info>	thermometers;
const std::string			thermo_topic(SecureConfig::thermo_topic);
constexpr	int64_t			mqtt_period	= 3600;	//Количество секунд между полной отправкой всех датчиков в MQTT

//Расширитель портов
constexpr gpio_num_t	pinSCL	= GPIO_NUM_18;
constexpr gpio_num_t	pinSDA	= GPIO_NUM_19;

//Список устройств PCF8574 на шине i2c
std::vector<PCF8574_data>	pcf8574;

void	thermo(void* unused)
{
	onewire_bus_handle_t	bus;
	onewire_bus_config_t	bus_config = {
		.bus_gpio_num = pin_ds18b20,
	};
	onewire_bus_rmt_config_t rmt_config = {
		.max_rx_bytes = 10, // 1byte ROM command + 8byte ROM number + 1byte device command
	};
	ESP_ERROR_CHECK(onewire_new_bus_rmt(&bus_config, &rmt_config, &bus));

	//Поиск подключенных датчиков
	ESP_LOGW(TAG, "Find devices:");
	onewire_device_iter_handle_t	iter = nullptr;
	onewire_device_t	next_onewire_device;
	esp_err_t			search_result = ESP_OK;

	// create 1-wire device iterator, which is used for device search
	ESP_ERROR_CHECK(onewire_new_device_iter(bus, &iter));
	ESP_LOGI(TAG, "Device iterator created, start searching...");
	do{
		search_result	= onewire_device_iter_get_next(iter, &next_onewire_device);
		if (search_result == ESP_OK)
		{
			// found a new device, let's check if we can upgrade it to a DS18B20
			ds18b20_device_handle_t	ds18b20s;
			ds18b20_config_t	ds_cfg = {};
			if(ds18b20_new_device(&next_onewire_device, &ds_cfg, &ds18b20s) == ESP_OK)
			{
				ESP_LOGI(TAG, "Found a DS18B20, address: %016llX", next_onewire_device.address);

				//Печать кода
				char	buf[256];
				sprintf(buf, "%llx", next_onewire_device.address);

				thermo_info	info;
				info.rom_code	= next_onewire_device.address;
				info.name		= buf;
				info.device		= ds18b20s;
				thermometers.push_back(info);
			}
			else
			{
				ESP_LOGI(TAG, "Found an unknown device, address: %016llX", next_onewire_device.address);
			}
		}
	}
	while(search_result != ESP_ERR_NOT_FOUND);
	ESP_ERROR_CHECK(onewire_del_device_iter(iter));
	ESP_LOGW(TAG, "Found %d device%s", thermometers.size(), thermometers.size() == 1 ? "" : "s");

	//Установка разрешения
	for(thermo_info& info : thermometers)
	{
		ESP_ERROR_CHECK(ds18b20_set_resolution(info.device, DS18B20_RESOLUTION_12B));
	}

	//Сортировка списка датчиков и установка нормальных имен
	for(size_t name_index = 0; name_index < SecureConfig::known_sensors.size(); name_index++)
	{
		auto& name	= SecureConfig::known_sensors.at(name_index);

		//Поиск нужного серийного номера
		for(size_t i = 0; i < thermometers.size(); i++)
		{
			thermo_info	info	= thermometers.at(i);
			if(name.first == info.rom_code)
			{
				//Перемещение найденного датчика в нужное место в списке
				info.name	= name.second;
				if(i != name_index)
				{
					thermo_info	tmp	= thermometers.at(name_index);
					thermometers.at(name_index)	= info;
					thermometers.at(i)			= tmp;
				}
				else
					thermometers.at(name_index)	= info;

				break;
			}
		}
	}

	//Значение порта
	uint8_t	relay_state	= 0;
	int64_t	periodical_mqtt_time	= esp_timer_get_time();

	/////////////////////////////////////////////////////////////////////
	//  Главный цикл
	for(;;)
	{
		if(!RMT_thermo_is_enabled)
		{
			vTaskDelay(pdMS_TO_TICKS(10000));
			continue;
		}

		//Опрос температуры
		if(!thermometers.empty())
		{
			//Ручное исполнение ds18b20_convert_all и ds18b20_wait_for_conversion
			const uint8_t	skip_rom		= 0xCC;
			const uint8_t	convert_temp	= 0x44;
			onewire_bus_reset(bus);
			onewire_bus_write_bytes(bus, &skip_rom, sizeof(skip_rom));
			onewire_bus_write_bytes(bus, &convert_temp, sizeof(convert_temp));
			vTaskDelay(pdMS_TO_TICKS(800));

			for(thermo_info& info : thermometers)
			{
				float	value;
				info.error_code	= ds18b20_get_temperature(info.device, &value);
				if(value == 85. || value == -127)	info.error_code	= 85;
				if(info.error_code)
				{
					info.errors_count++;
					if(mqtt_client) esp_mqtt_client_publish(mqtt_client, SecureConfig::thermo_errors_topic, esp_err_to_name(info.error_code), 0, 0, 0);
					continue;
				}

				ESP_LOGI(TAG, "rom_code = 0x%llx, name = %s,\tt = %lf", info.rom_code, info.name.c_str(), info.value);

				if(value != info.value)
				{
					//Обновление значения
					info.value		= value;

					//Отправка в MQTT с фильтрацией дребезга
					float	delta	= value - info.sended_value;
					if(mqtt_client && (abs(delta) > 0.1f))
					{
						info.sended_value	= value;
						char	value[16];
						sprintf(value, "%.2f", info.value);
						esp_mqtt_client_publish(mqtt_client, (thermo_topic + info.name).c_str(), value, 0, 0, 0);
					}
				}
			}

			//Периодическая отправка всех значений в MQTT, чтобы не было разрывов графиков
			if(esp_timer_get_time() - periodical_mqtt_time > mqtt_period*1000000)
			{
				periodical_mqtt_time	= esp_timer_get_time();
				if(mqtt_client)
				{
					char	value[16];
					for(thermo_info& info : thermometers)
					{
						sprintf(value, "%.2f", info.value);
						esp_mqtt_client_publish(mqtt_client, (thermo_topic + info.name).c_str(), value, 0, 0, 0);
					}
				}
			}
		}

		//Управление термоголовками
		relay_state++;
		if(relay_state > 0xf)	relay_state	= 0;
		if(pcf8574.size() > 0)
			pcf8574.front().state	= relay_state;

		//Передача состояния
		for(PCF8574_data& pcf : pcf8574)
			i2c_master_transmit(pcf.device, &pcf.state, sizeof(uint8_t), -1);

		vTaskDelay(pdMS_TO_TICKS(10000));
	}
}

void	init_thermoHeads()
{
	//Создание шины i2c
	i2c_master_bus_config_t	i2c_master_config;
	i2c_master_config.clk_source					= I2C_CLK_SRC_DEFAULT;	//Источник синхронизации для шины
	i2c_master_config.i2c_port						= I2C_NUM_0;			//Номер шины (I2C_NUM_0 или I2C_NUM_1)
	i2c_master_config.scl_io_num					= pinSCL;				//Номер GPIO для линии синхронизации SCL
	i2c_master_config.sda_io_num					= pinSDA;				//Номер GPIO для линии данных SDА
	i2c_master_config.flags.enable_internal_pullup	= 1;					//Использовать встроенную подтяжку GPIO
	i2c_master_config.glitch_ignore_cnt				= 7;					//Период сбоя данных на шине, стандартное значение 7
	i2c_master_config.intr_priority					= 0;					//Приоритет прерывания: авто
	i2c_master_config.trans_queue_depth				= 0;					//Глубина внутренней очереди. Действительно только при асинхронной передаче

	//Настройка шины
	i2c_master_bus_handle_t	bus_handle;
	ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_master_config, &bus_handle));
	ESP_LOGI(TAG, "I2C bus is configured");

	//Сканирование шины (поиск устройств)
	for(uint8_t i = 1; i < 128; i++)
	{
		if(i2c_master_probe(bus_handle, i, -1) == ESP_OK)
		{
			ESP_LOGI(TAG, "Found device on bus 0 at address 0x%.2X", i);
		}
	}

	//Настройка slave-устройства
	i2c_device_config_t	i2c_device_config;
	i2c_device_config.dev_addr_length			= I2C_ADDR_BIT_LEN_7;	//Используется стандартная 7-битная адресация
	i2c_device_config.device_address			= 0x20;					//Адрес устройства
	i2c_device_config.scl_speed_hz				= 100000;				//Тактовая частота шины 100kHz
	i2c_device_config.scl_wait_us				= 0;					//Время ожидания по умолчанию
	i2c_device_config.flags.disable_ack_check	= 0;					//Не отключать проверку ACK

	//Настройка PCF8574
	PCF8574_data	data;
	i2c_master_dev_handle_t	dev_handle;

	i2c_device_config.device_address	= 0x20;
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &i2c_device_config, &dev_handle));
	data.device	= dev_handle;
	pcf8574.push_back(data);

	//Второе устройство
	i2c_device_config.device_address	= 0x21;
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &i2c_device_config, &dev_handle));
	data.device	= dev_handle;
	pcf8574.push_back(data);

	//Коллектор 1 этажа
	i2c_device_config.device_address	= 0x22;
	ESP_ERROR_CHECK(i2c_master_bus_add_device(bus_handle, &i2c_device_config, &dev_handle));
	data.device	= dev_handle;
	pcf8574.push_back(data);

	ESP_LOGI(TAG, "PCF8574 is configured");

	//Запись нулей
	for(PCF8574_data& pcf : pcf8574)
		i2c_master_transmit(pcf.device, &pcf.state, sizeof(uint8_t), -1);
}

void	thermo_status(std::ostringstream& ss)
{
	ss << "Датчики:" << std::endl;
	for(const auto& info : thermometers)
	{
		ss << "*" << info.name << "*";
		ss << " = " << std::fixed << std::setprecision(2) << info.value << " ℃";
		if(info.errors_count > 0)
			ss << " (" << info.errors_count << " сбоев)";
		ss << std::endl;
	}
}

json	thermo_json_status()
{
	json	thermo;
	for(const auto &info : thermometers)
		thermo[info.name]	= info.value;

	return thermo;
}

void	thermo_log_head(std::ostringstream& ss)
{
	for(const thermo_info& info : thermometers)
		ss << info.name << "; ";
}

void	thermo_log_data(std::ostringstream& ss)
{
	for(const thermo_info& info : thermometers)
		ss << info.value << "; ";
}