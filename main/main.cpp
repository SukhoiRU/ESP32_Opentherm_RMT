#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_sntp.h"
#include "esp_spiffs.h"
#include "driver/gpio.h"
#include "json.hpp"
using json = nlohmann::json;

static const char*	TAG = "main";

/////////////////////////////////////////////////////////////////////////////////////
//Задачи FreeRTOS
#include "secure_config.h"
#include "telegram.h"
#include "gpio_control.h"
#include "thermo.h"
#include "logger.h"
#include "boiler_task.h"
#include "mqtt.h"
#include "tcp_server.h"

void	wifi_init_sta(const char* ssid, const char* pass);
QueueHandle_t	from_telegram_gpio_queue	= nullptr;
QueueHandle_t	from_telegram_ot_queue		= nullptr;
QueueHandle_t	to_telegram_queue			= nullptr;
QueueHandle_t	from_mqtt_queue				= nullptr;

QueueHandle_t	from_TCP_ot_queue			= nullptr;
QueueHandle_t	to_TCP_ot_queue				= nullptr;

extern "C" void	app_main()
{
	esp_log_level_set("*", ESP_LOG_INFO);
	esp_log_level_set("main", ESP_LOG_DEBUG);
	esp_log_level_set("wifi", ESP_LOG_WARN);

	esp_log_level_set("thermo", ESP_LOG_WARN);
	esp_log_level_set("telegram", ESP_LOG_INFO);
	esp_log_level_set("telegram_send", ESP_LOG_INFO);

	esp_log_level_set("telegram_client", ESP_LOG_INFO);
	esp_log_level_set("HTTP_CLIENT", ESP_LOG_WARN);
	esp_log_level_set("logger", ESP_LOG_INFO);
	esp_log_level_set("ot_boiler", ESP_LOG_INFO);
	esp_log_level_set("boiler_task", ESP_LOG_INFO);
	esp_log_level_set("tcp_server", ESP_LOG_INFO);
	esp_log_level_set("gpio", ESP_LOG_NONE);
	esp_log_level_set("i2c.master", ESP_LOG_NONE);
	esp_log_level_set("rmt_opentherm", ESP_LOG_INFO);

	//Инициализация NVS
	esp_err_t ret = nvs_flash_init();
	if(ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND)	{ESP_ERROR_CHECK(nvs_flash_erase()); ret = nvs_flash_init();}
	ESP_ERROR_CHECK(ret);

	//Выставка начального состояния термоголовок
	init_thermoHeads();

	//Инициализация SPIFFS
	esp_vfs_spiffs_conf_t conf = {
		.base_path = "/spiffs",
		.partition_label = NULL,
		.max_files = 5,
		.format_if_mount_failed = true
	};

	ret = esp_vfs_spiffs_register(&conf);

	if (ret != ESP_OK)
	{
		if(ret == ESP_FAIL) 				ESP_LOGE(TAG, "Failed to mount or format filesystem");
		else if(ret == ESP_ERR_NOT_FOUND)	ESP_LOGE(TAG, "Failed to find SPIFFS partition");
		else 								ESP_LOGE(TAG, "Failed to initialize SPIFFS (%s)", esp_err_to_name(ret));
		return;
	}

	size_t total = 0, used = 0;
	ret = esp_spiffs_info(conf.partition_label, &total, &used);
	if(ret != ESP_OK)	ESP_LOGE(TAG, "Failed to get SPIFFS partition information (%s)", esp_err_to_name(ret));
	else 				ESP_LOGI(TAG, "Partition size: total: %d, used: %d", total, used);

	//Включение WiFi
	wifi_init_sta(SecureConfig::wifi_ssid, SecureConfig::wifi_password);

	//Синхронизация времени
	sntp_set_sync_mode(SNTP_SYNC_MODE_IMMED);
	esp_sntp_setoperatingmode(esp_sntp_operatingmode_t::SNTP_OPMODE_POLL);
	esp_sntp_setservername(0, "pool.ntp.org");
	esp_sntp_init();
	printf("sntp syncing\n");
	while(sntp_get_sync_status() != SNTP_SYNC_STATUS_COMPLETED)
		vTaskDelay(pdMS_TO_TICKS(500));
	printf("sntp synced!\n");

	time_t	now;
	tm	timeInfo;
	time(&now);
	setenv("TZ", "GMT-3", 1);
	tzset();
	localtime_r(&now, &timeInfo);

	char	date_buf[36];
	char	time_buf[10];
	sprintf(date_buf, "%04d-%02d-%02d", timeInfo.tm_year + 1900, timeInfo.tm_mon + 1, timeInfo.tm_mday);
	sprintf(time_buf, "%02d-%02d-%02d", timeInfo.tm_hour, timeInfo.tm_min, timeInfo.tm_sec);

	//Запуск клиента MQTT
	mqtt_init(SecureConfig::mqtt_broker);

	//Очереди обмена сообщениями
	from_telegram_gpio_queue	= xQueueGenericCreate(20, sizeof(fromTelegram*), queueQUEUE_TYPE_BASE);
	from_telegram_ot_queue		= xQueueGenericCreate(20, sizeof(fromTelegram_ot*), queueQUEUE_TYPE_BASE);
	to_telegram_queue			= xQueueGenericCreate(20, sizeof(toTelegram*), queueQUEUE_TYPE_BASE);
	from_mqtt_queue				= xQueueGenericCreate(20, sizeof(fromMQTT*), queueQUEUE_TYPE_BASE);
	from_TCP_ot_queue			= xQueueGenericCreate(20, sizeof(fromTCP_to_ot*), queueQUEUE_TYPE_BASE);
	to_TCP_ot_queue				= xQueueGenericCreate(20, sizeof(toTCP_from_ot*), queueQUEUE_TYPE_BASE);

	//Запуск задач
	xTaskCreatePinnedToCore(telegram,		"telegram",			8192, nullptr, 1, nullptr, 0);	//0.1 Гц
	xTaskCreatePinnedToCore(tcp_server,		"tcp_server",		8192, (void*)3333, 1, nullptr, 0);	//
	xTaskCreatePinnedToCore(gpio_control,	"gpio_control",		32768, nullptr, 1, nullptr, 1);	//100 Гц
	xTaskCreatePinnedToCore(thermo,			"thermo",			4096, nullptr, 1, nullptr, 1);	//0.1 Гц
	xTaskCreatePinnedToCore(logger,			"logger",			8192, nullptr, 1, nullptr, 1);	//Раз в 30 секунд
	xTaskCreatePinnedToCore(boiler_task,	"boiler_task",		16384, nullptr, 3, nullptr, 1);	//1 Гц
}
