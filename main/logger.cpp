#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include <sstream>
#include <fstream>
#include <iomanip>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_spiffs.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "json.hpp"
using json = nlohmann::json;

#include "telegram.h"
#include "gpio_control.h"
#include "thermo.h"
#include "boiler_task.h"
#include "logger.h"

static const char*	TAG = "logger";

std::string		filename;		//Имя будущего файла
bool			log_was_sended;	//Признак отправленного файла

void	print_header();

void	logger(void* unused)
{
	//Пауза на время подключения всех датчиков температуры
	vTaskDelay(pdMS_TO_TICKS(60000));
	log_was_sended	= true;

	//Прошлая минута
	time_t	now;
	tm	timeInfo;
	time(&now);
	localtime_r(&now, &timeInfo);
	int		old_minute	= timeInfo.tm_min;
	int		old_day		= timeInfo.tm_mday;

	//Печать заголовка, если файл пустой
	struct stat st;
	stat("/spiffs/log.csv", &st);
	if(st.st_size == 0)
		print_header();

	//Имя файла по первой дате
	filename	= "log_";
	char	date_buf[36];
	strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &timeInfo);
	filename	+= date_buf;
	filename	+= ".csv";
	ESP_LOGI(TAG, "filename: %s", filename.c_str());

	/////////////////////////////////////////////////////////////////////
	//  Главный цикл
	for(;;)
	{
		//Замеры времени
		time(&now);
		localtime_r(&now, &timeInfo);

		//Раз в сутки сброс лога
		if(timeInfo.tm_mday != old_day)
		{
			old_day				= timeInfo.tm_mday;
			toTelegram*	send	= new toTelegram;
			send->chat_id		= 0;
			send->reply_id		= 0;
			send->text			= "send_log";
			xQueueGenericSend(to_telegram_queue, &send, 10, queueSEND_TO_BACK);
			log_was_sended		= false;
		}

		//Добавление записи раз в минуту
		if(timeInfo.tm_min != old_minute && log_was_sended)
		{
			old_minute	= timeInfo.tm_min;

			char buf[16];
			strftime(buf, 10, "%H:%M:%S", &timeInfo);

			std::ostringstream	ss;
			ss << buf << "; ";
			ss << now << "; ";
			thermo_log_data(ss);
			if(pBoiler)
				pBoiler->log_data(ss);
			ss << std::endl;
			ESP_LOGI(TAG, "data: %s", ss.str().c_str());

			FILE*	log_file	= fopen("/spiffs/log.csv", "a");
			if(log_file)
			{
				fprintf(log_file, ss.str().c_str());
				fclose(log_file);
			}
		}

		vTaskDelay(pdMS_TO_TICKS(30000));
	}
}

void	print_header()
{
	FILE*	log_file	= fopen("/spiffs/log.csv", "a");
	if(!log_file)	return;

	std::ostringstream	ss;
	ss << "Время; timestamp; ";
	thermo_log_head(ss);
	if(pBoiler)
		pBoiler->log_head(ss);
	ss << std::endl;
	ESP_LOGI(TAG, "header: %s", ss.str().c_str());

	fprintf(log_file, ss.str().c_str());
	fclose(log_file);
}

void	send_log(Telegram_client* bot, int64_t chat_id, bool create_new)
{
	//Передача данных
	log_was_sended	= false;
	if(bot->sendSPIFFS(chat_id, "application/octet-stream", filename, "/spiffs/log.csv"))
	{
		if(create_new)
		{
			//Удаление прошлого лога
			struct stat st;
			if(stat("/spiffs/log.csv", &st) == 0)
				unlink("/spiffs/log.csv");

			//Создание файла заново
			print_header();

			//Имя файла по первой дате
			time_t	now	= time(nullptr);
			tm	timeInfo;
			localtime_r(&now, &timeInfo);

			filename	= "log_";
			char	date_buf[36];
			strftime(date_buf, sizeof(date_buf), "%Y-%m-%d", &timeInfo);
			filename	+= date_buf;
			filename	+= ".csv";
		}

		log_was_sended	= true;
	}
}

