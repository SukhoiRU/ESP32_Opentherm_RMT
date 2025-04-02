#include <vector>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "json.hpp"
using json = nlohmann::json;

#include "secure_config.h"
#include "ot_boiler.h"
#include "telegram.h"
#include "mqtt.h"
#include "tcp_server.h"
#include "thermo.h"
#include "room_thermostat.h"

// static const char*	TAG = "boiler_task";

//Шина Opentherm
bool	OT_is_enabled	= true;
constexpr	gpio_num_t	pin_ot_in			= GPIO_NUM_16;
constexpr	gpio_num_t	pin_ot_out			= GPIO_NUM_4;
constexpr	int			slaveID				= 4;
constexpr	int64_t		mqtt_period			= 3600;			//Количество секунд между полной отправкой состояния в MQTT
constexpr	int64_t		thermostat_period	= 60;	//Частота работы термостата

const std::string	boiler_topic	= SecureConfig::boiler_topic;
const std::string	boiler_OT_topic	= SecureConfig::boiler_OT_topic;
const std::string	boiler_command_topic(SecureConfig::boiler_command_topic);

//Константный доступ для запросов статуса из других задач
const OT_Boiler*	pBoiler			= nullptr;
const json*			pControlStatus	= nullptr;

void	boiler_task(void* unused)
{
	enum class ControlMode_t: uint8_t {ch_temp, PID_thermostat};
	ControlMode_t	controlMode	= ControlMode_t::ch_temp;	//По умолчанию - теплоноситель

	OT_Boiler	boiler(pin_ot_in, pin_ot_out, boiler_topic, boiler_OT_topic, slaveID);
	pBoiler	= &boiler;

	//Формирование статуса управления
	json	jsonStatus;
	pControlStatus	= &jsonStatus;

	//Перевод котла в режим Slave
	boiler.read_status();
	boiler.read_slaveConfig();
	boiler.set_slave();

	//Заданная температура теплоносителя в режиме без термостата
	float	ch_temp_zad	= 50;

	//Чтение прошлых настроек
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler_task", NVS_READONLY, &nvs_settings) == ESP_OK)
	{
		uint8_t	val;
		if(nvs_get_u8(nvs_settings, "controlMode", &val) == ESP_OK){
			switch(val)
			{
				case 1:	{
					controlMode	= ControlMode_t::PID_thermostat;
					jsonStatus["controlMode"]	= "PID_thermostat";
				}break;

				default: {
					controlMode	= ControlMode_t::ch_temp;
					jsonStatus["controlMode"]	= "Теплоноситель";
					if(nvs_get_u8(nvs_settings, "ch_temp_zad", &val) == ESP_OK){
						ch_temp_zad	= val;
						jsonStatus["ch_temp_zad"]	= ch_temp_zad;
					}
				}
			}
		}

		nvs_close(nvs_settings);
	}

	//Запрос статуса, чтобы не ждать 10 секунд
	boiler.read_status();

	//Запрос топиков управления котлом
	mqtt_topic_list.push_back(boiler_command_topic + "CH");
	mqtt_topic_list.push_back(boiler_command_topic + "DHW");
	mqtt_topic_list.push_back(boiler_command_topic + "SummerMode");
	mqtt_topic_list.push_back(boiler_command_topic + "ch_temp_zad");
	mqtt_topic_list.push_back(boiler_command_topic + "ch_temp_max");
	mqtt_topic_list.push_back(boiler_command_topic + "ch_mod_max");
	mqtt_topic_list.push_back(boiler_command_topic + "Reset_error");

	//Комнатные термостаты
	std::vector<RoomThermostat*>	rooms;

	//Время от прошлого запроса параметров котла
	int64_t	periodical_time			= esp_timer_get_time();
	int64_t	mqtt_periodical_time	= esp_timer_get_time();
	int64_t	thermostat_time			= esp_timer_get_time();

	/////////////////////////////////////////////////////////////////////
	//  Главный цикл
	for(;;)
	{
		//Отключение обмена на время прошивки
		if(!OT_is_enabled)
		{
			vTaskDelay(pdMS_TO_TICKS(1000));
			continue;
		}

		// //Повтор сообщений с ошибкой обмена
		// size_t	remain_msg	= boiler.repeat_old_messages();
		// if(remain_msg > 50)
		// {
		// 	//Что-то слишком много накопилось ошибочных сообщений
		// 	toTelegram*	send	= new toTelegram;
		// 	send->chat_id		= SecureConfig::telegram_user_id;
		// 	send->reply_id		= 0;
		// 	send->text			= "В очереди накопилось более 50 сообщений";
		// 	xQueueGenericSend(to_telegram_queue, &send, 10, queueSEND_TO_BACK);

		// 	boiler.clear_old_message();
		// }
		boiler.clear_old_message();

		//Ежесекундный опрос состояния
		boiler.read_status();
		boiler.read_modulation();

		//Периодический опрос датчиков котла
		if(esp_timer_get_time() - periodical_time > 10*1000000)
		{
			periodical_time	= esp_timer_get_time();

			boiler.read_flame_current();
			boiler.read_ch_temp();
			boiler.read_dhw_temp();
		}

		//Периодическая отправка всего состояния в MQTT
		if(esp_timer_get_time() - mqtt_periodical_time > mqtt_period*1000000)
		{
			mqtt_periodical_time	= esp_timer_get_time();
			boiler.send_all_mqtt();
		}

		//Термостат
		if(esp_timer_get_time() - thermostat_time > thermostat_period*1000000)
		{
			thermostat_time	= esp_timer_get_time();
			switch(controlMode)
			{
				case ControlMode_t::ch_temp:
				{
					boiler.set_ch_temp_zad(ch_temp_zad, true);
				}break;

				case ControlMode_t::PID_thermostat:
				{
					//Расчет всех термостатов с выбором максимального значения
					float	ch_temp_zad	= 0;
					float	ch_mod_max	= 0;
					for(RoomThermostat* room : rooms){
						room->Life(thermostat_period);
						jsonStatus["params"]["PID"]	= room->getPID_params();
						if(room->out.ch_temp_zad > ch_temp_zad)
							ch_temp_zad	= room->out.ch_temp_zad;
						if(room->out.mod_max > ch_mod_max)
							ch_mod_max	= room->out.mod_max;
					}

					//Управление теплоносителем
					if(ch_temp_zad != 0){
						boiler.set_ch_temp_zad(ch_temp_zad, true);
						// boiler.set_ch_mod_max(ch_mod_max, true);
					}
				}break;

				default:
					break;
			}
		}

		//Разбор сообщений из Telegramm
		fromTelegram_ot*	msg;
		while(xQueueReceive(from_telegram_ot_queue, &msg, 0) == pdPASS)
		{
			//Подготовка ответного сообщения
			toTelegram*	send	= new toTelegram;
			send->chat_id		= msg->chat_id;
			send->reply_id		= msg->reply_id;

			switch(msg->type)
			{
				case telegram_ot_message_t::set_boiler_data:
				{
					std::string	text	= msg->text.substr(strlen("/set_boiler_data"));
					json	j	= json::parse(text, nullptr, false);
					if(j.is_discarded())
					{
						send->text	= "json::parse error\n" + text;
						xQueueGenericSend(to_telegram_queue, &send, 10, queueSEND_TO_BACK);
						break;
					}
					else
					{
						json	resp	= boiler.set_boiler_data(j);
						send->text		= resp.dump(4);

						//Отключение термостата котла
						controlMode		= ControlMode_t::ch_temp;
						jsonStatus	= {
							{"controlMode", "Теплоноситель"},
							{"params", j}
						};

						xQueueGenericSend(to_telegram_queue, &send, 10, queueSEND_TO_BACK);
					}
				}break;

				case telegram_ot_message_t::BLOR:
				{
					if(boiler.BLOR())	send->text	= "BLOR done";
					else				send->text	= "BLOR fail";

					xQueueGenericSend(to_telegram_queue, &send, 10, queueSEND_TO_BACK);
				}break;

				default:
				{
					//Сообщение оказалось лишним
					delete send;
					send	= nullptr;
				}
			}

			if(msg)	delete msg;
		}

		//Разбор сообщений от TCP сервера
		fromTCP_to_ot*	tcp_msg	= nullptr;
		while(xQueueReceive(from_TCP_ot_queue, &tcp_msg, 0) == pdPASS)
		{
			if(mqtt_client) esp_mqtt_client_publish(mqtt_client, (std::string(SecureConfig::boiler_debug_topic) + "request").c_str(), tcp_msg->params.dump().c_str(), 0, 0, 0);
			//Подготовка ответа
			toTCP_from_ot*	answer	= new toTCP_from_ot;
			switch(tcp_msg->type)
			{
				case TCP_message_t::BLOR:{
					bool	res	= boiler.BLOR();
					answer->response	= {{"BLOR", (res ? "done" : "fail")}};
					xQueueGenericSend(to_TCP_ot_queue, &answer, 10, queueSEND_TO_FRONT);
				}break;

				case TCP_message_t::set_boiler_data:{
					answer->response	= boiler.set_boiler_data(tcp_msg->params);

					//Запоминание заданной температуры
					if(tcp_msg->params.contains("ch_temp_zad") && tcp_msg->params.at("ch_temp_zad").is_number_integer()){
						int	ch_temp_zad	= tcp_msg->params.at("ch_temp_zad").get<int>();
						nvs_handle_t	nvs_settings;
						if(nvs_open("boiler_task", NVS_READONLY, &nvs_settings) == ESP_OK){
							nvs_set_u8(nvs_settings, "ch_temp_zad", static_cast<uint8_t>(ch_temp_zad));
							nvs_commit(nvs_settings);
							nvs_close(nvs_settings);
						}
					}

					//Отключение термостата котла
					controlMode			= ControlMode_t::ch_temp;
					jsonStatus	= {
						{"controlMode", "Теплоноситель"},
						{"params", tcp_msg->params}
					};

					xQueueGenericSend(to_TCP_ot_queue, &answer, 10, queueSEND_TO_FRONT);
				}break;

				case TCP_message_t::test_ot_command:{
					answer->response	= boiler.test_ot_command(tcp_msg->params);
					xQueueGenericSend(to_TCP_ot_queue, &answer, 10, queueSEND_TO_FRONT);
				}break;

				case TCP_message_t::PID_thermostat:{
					//Проверка наличия всех полей
					if(!tcp_msg->params.contains("room_name") ||
						!tcp_msg->params.contains("radiator_name") ||
						!tcp_msg->params.contains("room_temp_zad") ||
						!tcp_msg->params.contains("dhw_temp_zad") ||
						!tcp_msg->params.contains("PID") ||
						!tcp_msg->params.contains("room_mod_max"))				answer->response	= {{"fail", "not all params present"}};
					//Проверка типов всех полей
					else if(!tcp_msg->params.at("room_name").is_string() ||
							!tcp_msg->params.at("radiator_name").is_string() ||
							!tcp_msg->params.at("room_temp_zad").is_number() ||
							!tcp_msg->params.at("dhw_temp_zad").is_number() ||
							!tcp_msg->params.at("PID").is_object() ||
							!tcp_msg->params.at("room_mod_max").is_number())	answer->response	= {{"fail", "params types incorrect"}};
					//Все параметры в норме
					else{
						std::string	room_name	= tcp_msg->params.at("room_name").get<std::string>();
						std::string	rad_name	= tcp_msg->params.at("radiator_name").get<std::string>();
						float	room_temp_zad	= tcp_msg->params.at("room_temp_zad").get<float>();
						float	dhw_temp_zad	= tcp_msg->params.at("dhw_temp_zad").get<float>();
						float	room_mod_max	= tcp_msg->params.at("room_mod_max").get<float>();
						json	PID_params		= tcp_msg->params.at("PID");

						//Установка заданной температуры горячей воды
						boiler.set_dhw_temp_zad(dhw_temp_zad);

						//Поиск индекса термометра, соответствующего имени
						bool	bFound		= false;
						uint8_t	room_index	= 0;
						uint8_t	rad_index	= 0;
						for(size_t i = 0; i < thermometers.size(); i++){
							const thermo_info& info	= thermometers.at(i);
							if(info.name == room_name){
								room_index	= i;
								bFound	= true;
								break;
							}
						}

						for(size_t i = 0; i < thermometers.size(); i++){
							const thermo_info& info	= thermometers.at(i);
							if(info.name == rad_name){
								rad_index	= i;
								bFound	= true;
								break;
							}
						}

						if(bFound){
							//Поиск термостата или создание нового
							RoomThermostat*	room	= nullptr;
							for(RoomThermostat* r : rooms){
								if(r->name == room_name){
									room	= r;
									break;
								}
							}
							if(!room){
								room	= new RoomThermostat(room_index, rad_index, room_name, boiler_topic + "PID/" + room_name + "/");
								rooms.push_back(room);
							}

							//Установка параметров
							room->setParams(room_temp_zad, room_mod_max, PID_params);

							controlMode	= ControlMode_t::PID_thermostat;
							jsonStatus	= {
								{"controlMode", "PID_thermostat"},
								{"params", {
									{"room_name", room_name},
									{"radiator_name", rad_name},
									{"room_temp_zad", room_temp_zad},
									{"dhw_temp_zad", dhw_temp_zad},
									{"room_mod_max", room_mod_max},
									{"PID", room->getPID_params()}	//Дело в том, что прийти может только одно значение из списка параметров, поэтому обновлять нужно всё
								}}
							};

							nvs_handle_t	nvs_settings;
							if(nvs_open("boiler_task", NVS_READONLY, &nvs_settings) == ESP_OK){
								nvs_set_u8(nvs_settings, "controlMode", static_cast<uint8_t>(controlMode));
								nvs_set_u16(nvs_settings, "room_temp_zad", uint16_t(room_temp_zad*256));

								nvs_commit(nvs_settings);
								nvs_close(nvs_settings);
							}

							answer->response	= {
								{"status", "ok"},
								{"room_index", room_index},
								{"rad_index", rad_index}
							};
						}
						else{
							answer->response	= {
								{"fail", "names not found"},
								{"name", room_name},
								{"radiator", rad_name}
							};
						}
					}

					xQueueGenericSend(to_TCP_ot_queue, &answer, 10, queueSEND_TO_FRONT);
				}break;

				default:
					delete answer;
			}
			if(mqtt_client) esp_mqtt_client_publish(mqtt_client, (std::string(SecureConfig::boiler_debug_topic) + "response").c_str(), answer->response.dump().c_str(), 0, 0, 0);

			if(tcp_msg)	delete tcp_msg;
		}

		//Задержка выполняется в process_OT, поэтому здеь не нужна
		// vTaskDelay(pdMS_TO_TICKS(1000));
	}
}
