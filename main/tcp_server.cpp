#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_netif.h"
#include "lwip/err.h"
#include "lwip/sockets.h"
#include "lwip/sys.h"
#include <lwip/netdb.h>
#include "driver/gpio.h"
#include "rom/gpio.h"

#include "json.hpp"
using json = nlohmann::json;
#include "gpio_control.h"
#include "thermo.h"
#include "boiler_task.h"
#include "mqtt.h"
#include "tcp_server.h"

static const char *TAG = "tcp_server";
char	rx_buffer[1024];
void	parse_tcp_message(const int sock);
bool	send_to_socket(const int sock, const json& j);
constexpr gpio_num_t	pin_led				= GPIO_NUM_2;
bool	tcp_server_is_listening	= false;

void	tcp_server(void *pvParameters)
{
	gpio_pad_select_gpio(pin_led);
	gpio_set_direction(pin_led, GPIO_MODE_OUTPUT);
	gpio_pullup_dis(pin_led);
	gpio_pulldown_dis(pin_led);
	gpio_set_level(pin_led, 0);

	char	addr_str[128];
	int		keepAlive		= 1;
	int		keepIdle		= 5;
	int		keepInterval	= 5;
	int		keepCount		= 3;
	int		PORT			= int(pvParameters);

	struct sockaddr_storage	dest_addr;
	struct sockaddr_in*		dest_addr_ip4	= (struct sockaddr_in *)&dest_addr;
	dest_addr_ip4->sin_addr.s_addr			= htonl(INADDR_ANY);
	dest_addr_ip4->sin_family 				= AF_INET;
	dest_addr_ip4->sin_port					= htons(PORT);

	int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
	if (listen_sock < 0) {
		ESP_LOGE(TAG, "Unable to create socket: errno %d", errno);
		vTaskDelete(NULL);
		return;
	}
	int opt = 1;
	setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

	ESP_LOGI(TAG, "Socket created");

	int err	= bind(listen_sock, (struct sockaddr *)&dest_addr, sizeof(dest_addr));
	if (err != 0) {
		ESP_LOGE(TAG, "Socket unable to bind: errno %d", errno);
		ESP_LOGE(TAG, "IPPROTO: AF_INET");
		close(listen_sock);
		vTaskDelete(NULL);
		return;
	}
	ESP_LOGI(TAG, "Socket bound, port %d", PORT);

	err = listen(listen_sock, 1);
	if (err != 0) {
		ESP_LOGE(TAG, "Error occurred during listen: errno %d", errno);
		close(listen_sock);
		vTaskDelete(NULL);
		return;
	}

	while (1) {
		ESP_LOGI(TAG, "Socket listening");
		tcp_server_is_listening	= true;

		struct sockaddr_storage	source_addr; // Large enough for both IPv4 or IPv6
		socklen_t	addr_len	= sizeof(source_addr);
		int			sock		= accept(listen_sock, (struct sockaddr *)&source_addr, &addr_len);
		if (sock < 0) {
			ESP_LOGE(TAG, "Unable to accept connection: errno %d", errno);
			tcp_server_is_listening	= false;
			continue;
		}
		gpio_set_level(pin_led, 1);

		//Set tcp keepalive option
		setsockopt(sock, SOL_SOCKET, SO_KEEPALIVE, &keepAlive, sizeof(int));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPIDLE, &keepIdle, sizeof(int));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPINTVL, &keepInterval, sizeof(int));
		setsockopt(sock, IPPROTO_TCP, TCP_KEEPCNT, &keepCount, sizeof(int));

		//Convert ip address to string
		if (source_addr.ss_family == PF_INET) {
			inet_ntoa_r(((struct sockaddr_in *)&source_addr)->sin_addr, addr_str, sizeof(addr_str) - 1);
		}
		ESP_LOGI(TAG, "Socket accepted ip address: %s", addr_str);

		parse_tcp_message(sock);

		shutdown(sock, 0);
		close(sock);
		gpio_set_level(pin_led, 0);
	}
}

void	parse_tcp_message(const int sock)
{
	json	response;

	//Приём строки
	int	len	= recv(sock, rx_buffer, sizeof(rx_buffer) - 1, 0);
	if(len < 0)
	{
		ESP_LOGE(TAG, "Error occurred during receiving: errno %d", errno);
		response	= {{"result", std::string("Error occurred during receiving: errno ") +  std::to_string(errno)}};
	}
	else if(len == 0)
	{
		ESP_LOGW(TAG, "Connection closed");
		response	= {{"result", "Connection closed"}};
	}
	else if(len > 1000)	response	= {{"result", "Превышен размер запроса"}};
	else
	{
		//Разбор сообщения от клиента
		rx_buffer[len] = 0;
		ESP_LOGI(TAG, "Received %d bytes: %s", len, rx_buffer);

		json	j	= json::parse(rx_buffer, nullptr, false);
		if(j.is_discarded())
		{
			ESP_LOGE(TAG, "Ошибка при разборе json: %s", rx_buffer);
			response	= {
				{"result", "Ошибка при разборе json"}
			};
		}
		else if(j.contains("command"))
		{
			if(!j.at("command").is_string())	response	= {{"result", "command не строка"}};
			else{
				std::string	command	= j.at("command").get<std::string>();

				//Статус
				if(command == "status"){
					json j;
					if(pControlStatus)
						j["control"]			= *pControlStatus;
					if(pBoiler)
						j["Котёл"]				= pBoiler->json_status();
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

					response	= {
						{"result", "ok"},
						{"response", j}
					};
				}

				//Установка параметров котла
				else if(command == "set_boiler_data"){
					if(!j.contains("boiler_data"))	response	= {{"result", "Отсутствует boiler_data"}};
					else{
						if(!j.at("boiler_data").is_object())	response	= {{"result", "boiler_data не объект"}};
						else{
							json	boiler_data	= j.at("boiler_data");

							//Постановка сообщения в очередь
							fromTCP_to_ot*	msg	= new fromTCP_to_ot(TCP_message_t::set_boiler_data, boiler_data);
							xQueueGenericSend(from_TCP_ot_queue, &msg, 10, queueSEND_TO_FRONT);

							//Ожидание ответа
							toTCP_from_ot*	boiler_response	= nullptr;
							if(xQueueReceive(to_TCP_ot_queue, &boiler_response, pdMS_TO_TICKS(5000)) == pdPASS){
								response	= {{"result", "ok"},{"response", boiler_response->response}};
								if(boiler_response)	delete boiler_response;
							}
							else{
								response	= {{"result", "no answer from task_boiler"}};
							}
						}
					}
				}

				//Принудительный сброс ошибки
				else if(command == "BLOR"){
					//Постановка сообщения в очередь
					fromTCP_to_ot*	msg	= new fromTCP_to_ot(TCP_message_t::BLOR, json{{"params", ""}});
					xQueueGenericSend(from_TCP_ot_queue, &msg, 10, queueSEND_TO_FRONT);

					//Ожидание ответа
					toTCP_from_ot*	boiler_response	= nullptr;
					if(xQueueReceive(to_TCP_ot_queue, &boiler_response, pdMS_TO_TICKS(5000)) == pdPASS){
						response	= {{"result", "ok"}, {"response", boiler_response->response}};
						if(boiler_response)	delete boiler_response;
					}
					else{response	= {{"result", "no answer from task_boiler"}};}
				}

				//Тестирование обмена с котлом
				else if(command == "test_ot_command"){
					if(!j.contains("ot_data"))				response	= {{"result", "Отсутствует ot_data"}};
					else if(!j.at("ot_data").is_object())	response	= {{"result", "ot_data не объект"}};
					else{
						json	ot_data	= j.at("ot_data");

						//Постановка сообщения в очередь
						fromTCP_to_ot*	msg	= new fromTCP_to_ot(TCP_message_t::test_ot_command, ot_data);
						xQueueGenericSend(from_TCP_ot_queue, &msg, 10, queueSEND_TO_FRONT);

						//Ожидание ответа
						toTCP_from_ot*	boiler_response	= nullptr;
						if(xQueueReceive(to_TCP_ot_queue, &boiler_response, pdMS_TO_TICKS(5000)) == pdPASS){
							response	= {{"result", "ok"}, {"response", boiler_response->response}};
							if(boiler_response)	delete boiler_response;
						}
						else{response	= {{"result", "no answer from task_boiler"}};}
					}
				}

				//Включение моего термостата
				else if(command == "PID_thermostat"){
					if(!j.contains("params"))				response	= {{"result", "Отсутствует params"}};
					else if(!j.at("params").is_object())	response	= {{"result", "params не объект"}};
					else{
						json	params	= j.at("params");

						//Постановка сообщения в очередь
						fromTCP_to_ot*	msg	= new fromTCP_to_ot(TCP_message_t::PID_thermostat, params);
						xQueueGenericSend(from_TCP_ot_queue, &msg, 10, queueSEND_TO_FRONT);

						//Ожидание ответа
						toTCP_from_ot*	boiler_response	= nullptr;
						if(xQueueReceive(to_TCP_ot_queue, &boiler_response, pdMS_TO_TICKS(5000)) == pdPASS){
							response	= {{"result", "ok"}, {"response", boiler_response->response}};
							if(boiler_response)	delete boiler_response;
						}
						else{response	= {{"result", "no answer from task_boiler"}};}
					}
				}

				//Принудительная перезагрузка
				else if(command == "reboot"){
					esp_restart();
				}

				else
				{
					response	= {
						{"result", "Неизвестная команда: " + command}
					};
				}
			}
		}
	}

	//Очистка очереди ответов
	toTCP_from_ot*	boiler_response	= nullptr;
	while(xQueueReceive(to_TCP_ot_queue, &boiler_response, pdMS_TO_TICKS(0)) == pdPASS){
		if(boiler_response)	delete boiler_response;
	}

	//Отправка ответа
	std::string	msg	= response.dump();
	ESP_LOGI(TAG, "response: %s", msg.c_str());
	const char*	buf	= msg.c_str();
	size_t	buf_len	= msg.length();
	size_t	to_write = buf_len;
	while(to_write > 0)
	{
		int	written	= send(sock, buf + (buf_len - to_write), to_write, 0);
		if(written < 0)
		{
			ESP_LOGE(TAG, "Error occurred during sending: errno %d", errno);
			return;
		}
		to_write -= written;
	}
}

bool	tcp_server_is_running()
{
	return tcp_server_is_listening;
}