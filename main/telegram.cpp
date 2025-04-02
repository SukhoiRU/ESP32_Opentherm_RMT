#include <string>
#include <sstream>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_ota_ops.h"
#include "esp_flash_partitions.h"
#include "esp_partition.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "json.hpp"
using json = nlohmann::json;

#include "secure_config.h"
#include "telegram.h"
#include "telegram_client.h"
#include "gpio_control.h"
#include "thermo.h"
#include "logger.h"
#include "boiler_task.h"
#include "mqtt.h"
#include "tcp_server.h"

static const char*	TAG	= "telegram";
static char	http_reply[16384];
void	send_to_gpio(const Telegram_client::Message& message, const telegram_message_t& command);
void	send_to_ot(const Telegram_client::Message& message, const telegram_ot_message_t& command);

void	telegram(void* unused)
{
	Telegram_client	bot(SecureConfig::bot_API_token, http_reply);
	bot.setUsers(SecureConfig::telegram_acsess_list);

	std::string	welcome_string("Esp32 is online");
	std::string	version_info("v5.5.12");
	welcome_string += "\n" + version_info;

	//Проверка статуса прошивки
	esp_ota_img_states_t	ota_state;
	if(esp_ota_get_state_partition(esp_ota_get_running_partition(), &ota_state) == ESP_OK)
	{
		if(ota_state == ESP_OTA_IMG_PENDING_VERIFY)
		{
			welcome_string	+= "\n\n";
			welcome_string	+= "Запущена новая прошивка OTA!\n";
			welcome_string	+= "Необходимо проверить работоспособность,\n";
			welcome_string	+= "и подтвердить командой\n/ota_verified\n";
			welcome_string	+= "\n";
			welcome_string	+= "Для возврата к предыдущей прошивке\n";
			welcome_string	+= "отправьте команду\n/ota_rollback";
		}
	}

	//Приветственное сообщение в чат
	bot.sendMessage(welcome_string, SecureConfig::telegram_user_id);

	/////////////////////////////////////////////////////////////////////
	//  Главный цикл
	for(;;)
	{
		//Проверка соединения
		if(bot.http_fail())
		{
			bot.reconnect();
			vTaskDelay(pdMS_TO_TICKS(10000));
			continue;
		}

		//Отправка сообщений из очереди
		toTelegram*	message;
		while(xQueueReceive(to_telegram_queue, &message, 10) == pdPASS)
		{
			ESP_LOGI(TAG, "Отправка сообщений из очереди = %s", message->text.c_str());
			if(message->text == "send_log")
			{
				send_log(&bot, SecureConfig::common_chat_id, true);
				delete message;
				continue;
			}

			bool	sendOK	= bot.sendMessage(message->text, message->chat_id, message->reply_id, message->parse_mode);
			if(!sendOK)
			{
				//При неудачной отправке сообщение возвращается в очередь
				xQueueGenericSend(to_telegram_queue, &message, 10, queueSEND_TO_FRONT);
			}
			else
				delete message;
		}

		//Приём и обработка сообщений
		for(const auto& message : bot.getMessages(90))
		{
			if(!message.authorized)
			{
				bot.sendMessage("Извините, Вас нет в списке допущенных пользователей", message.chat_id, message.message_id);
			}
			else if(message.text.rfind("/start", 0) == 0 ||
					message.text.rfind("/help", 0) == 0)
			{
				std::string	help;
				help	+= "Здравствуйте!\n";
				help	+= "Это бот управления котлом и вентилятором.\n";
				help	+= "Все доступные команды приведены в меню\n";
				help	+= "при нажатии символа /";
				bot.sendMessage(help, message.chat_id, message.message_id);
			}
			else if(message.is_firmware)
			{
				//Проверка, что прошивка пришла именно от меня
				if(message.chat_id != SecureConfig::telegram_user_id)	continue;
				ESP_LOGI("telegram", "file_id = %s", message.text.c_str());

				//Надо обновить сообщения, чтоб не вылезло после перезагрузки
				Telegram_client::Message	firmware_message	= message;	//Надо взять копию, чтоб не слетело после getMessages
				bot.getMessages(0);

				//Отключение остальных задач
				RMT_thermo_is_enabled	= false;
				OT_is_enabled			= false;
				vTaskDelay(pdMS_TO_TICKS(3000));

				//Прошивка
				ESP_LOGI("telegram", "Esp32 прошивается...");
				bot.sendMessage("Esp32 прошивается...", message.chat_id);
				esp_err_t	err	= bot.update_firmware(firmware_message);
				if(err == ESP_OK)
				{
					ESP_LOGI("telegram", "Успешно, перезагрузка...");
					bot.sendMessage("Успешно, перезагрузка...", message.chat_id);
					esp_restart();
				}
				else
				{
					ESP_LOGE("telegram", "Ошибка обновления прошивки: %s", esp_err_to_name(err));
					bot.sendMessage((std::string("Ошибка обновления прошивки: ") + esp_err_to_name(err)).c_str(), message.chat_id);

					//Восстановление задач
					RMT_thermo_is_enabled	= true;
					OT_is_enabled			= true;
				}
			}
			else if(message.text.rfind("/ota_verified", 0) == 0)
			{
				esp_ota_mark_app_valid_cancel_rollback();
				bot.sendMessage("OTA verified", message.chat_id, message.message_id);
			}
			else if(message.text.rfind("/ota_rollback", 0) == 0)
			{
				bot.sendMessage("Откат на предыдущую прошивку", message.chat_id, message.message_id);
				bot.getMessages(0);
				esp_ota_mark_app_invalid_rollback_and_reboot();
				break;
			}
			else if(message.text.rfind("/reboot", 0) == 0)
			{
				bot.getMessages(0);
				esp_restart();
			}
			else if(message.text.rfind("/status", 0) == 0)
			{
				std::ostringstream ss;
				if(pBoiler)
				{
					ss << "Котёл:" << std::endl;
					pBoiler->print_status(ss);
					ss << std::endl;
				}
				thermo_status(ss);
				gpio_status(ss);
				bot.sendMessage(ss.str(), message.chat_id, message.message_id, "Markdown");
			}
			else if(message.text.rfind("/json_status", 0) == 0)
			{
				json j;
				if(pBoiler)	j["Котёл"]		= pBoiler->json_status();
				j["Датчики температуры"]	= thermo_json_status();
				j["Связь"]					= {
					{"OpenTherm", pBoiler && pBoiler->openTherm_is_correct()},
					{"MQTT", (mqtt_client != nullptr)},
					{"TCP server", tcp_server_is_running()}
				};

				//Запрос текущего IP
				esp_netif_t*		sta_netif	= esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
				esp_netif_ip_info_t	ip_info;
				if(esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK)
				{
					ESP_LOGI(TAG, "IP Address: " IPSTR, IP2STR(&ip_info.ip));
					char	ip_addr[16];
					sprintf(ip_addr, IPSTR, IP2STR(&ip_info.ip));
					j["Связь"]["IP"]	= ip_addr;
				}

				bot.sendMessage(j.dump(), message.chat_id, message.message_id, "");
			}
			else if(message.text.rfind("/version_info", 0) == 0)		bot.sendMessage(version_info, message.chat_id, message.message_id);
			else if(message.text.rfind("/reset_worktime", 0) == 0)		send_to_gpio(message, telegram_message_t::reset_worktime);
			else if(message.text.rfind("/set_worktime", 0) == 0)		send_to_gpio(message, telegram_message_t::set_worktime);
			else if((message.text.rfind("/thermostat", 0) == 0))		send_to_gpio(message, telegram_message_t::program);
			else if((message.text.rfind("/get_log", 0) == 0))			send_log(&bot, message.chat_id, false);
			else if((message.text.rfind("/get_new_log", 0) == 0))		send_log(&bot, message.chat_id, true);
			else if((message.text.rfind("/set_boiler_data", 0) == 0))	send_to_ot(message, telegram_ot_message_t::set_boiler_data);
			else if((message.text.rfind("/BLOR", 0) == 0))				send_to_ot(message, telegram_ot_message_t::BLOR);
			else if((message.text.rfind("/set_heads_3", 0) == 0))
			{
				//Времянка для теста термоголовок
				if(message.text.length() > strlen("/set_heads_3") + 1)
				{
					std::string	text	= message.text.substr(strlen("/set_heads_3"));
					int			value	= std::stoi(text);
					if(pcf8574.size() > 2)
						pcf8574.back().state	= value;
				}
			}
		}

		vTaskDelay(pdMS_TO_TICKS(100));
	}
}

void	send_to_gpio(const Telegram_client::Message& message, const telegram_message_t& command)
{
	fromTelegram*	msg	= new fromTelegram;
	msg->chat_id	= message.chat_id;
	msg->reply_id	= message.message_id;
	msg->type		= command;
	msg->text		= message.text;

	xQueueGenericSend(from_telegram_gpio_queue, &msg, 10, queueSEND_TO_BACK);
}

void	send_to_ot(const Telegram_client::Message& message, const telegram_ot_message_t& command)
{
	fromTelegram_ot*	msg	= new fromTelegram_ot;
	msg->chat_id	= message.chat_id;
	msg->reply_id	= message.message_id;
	msg->type		= command;
	msg->text		= message.text;

	xQueueGenericSend(from_telegram_ot_queue, &msg, 10, queueSEND_TO_BACK);
}
