#include <string>
#include <fstream>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_tls.h"
#include "esp_https_ota.h"
#include "esp_spiffs.h"
#include "sdkconfig.h"
#include "json.hpp"
using json = nlohmann::json;

#include "secure_config.h"
#include "telegram_client.h"
#include "telegram.h"

extern volatile bool	wifi_connected;
esp_err_t	_http_event_handler(esp_http_client_event_t* evt);
extern const char	server_root_cert_pem_start[] asm("_binary_server_root_cert_pem_start");
extern const char	server_root_cert_pem_end[]   asm("_binary_server_root_cert_pem_end");
char			send_buf[16384];	//Буфер для отправки

Telegram_client::Telegram_client(const std::string& token, char* buf) :
	token(token)
{
	http_reply	= buf;
	create_client();
}

Telegram_client::~Telegram_client()
{

}

void	Telegram_client::create_client()
{
	//Создание клиента для http
	esp_http_client_config_t	config;
	memset(&config, 0, sizeof(config));
	config.url 				= "https://api.telegram.org";
	config.query			= "esp32";
	config.event_handler	= _http_event_handler;
	config.user_data		= http_reply;
	config.transport_type	= HTTP_TRANSPORT_OVER_SSL;
	config.cert_pem 		= server_root_cert_pem_start;
	config.timeout_ms		= 100000;
	config.keep_alive_enable	= true;
	config.keep_alive_idle		= 100000;
	config.keep_alive_interval	= 100000;

	if(client)
	{
		esp_http_client_close(client);
		esp_http_client_cleanup(client);
	}
	client	= esp_http_client_init(&config);
}

void	Telegram_client::setUsers(const std::initializer_list<int64_t>& users)
{
	authorized_users	= users;
}

bool	Telegram_client::sendMessage(const std::string& text, const int64_t& chat_id, const int64_t& reply_id, const std::string& parse_mode)
{
	if(http_fail_status)	return false;

	//Создание запроса
	esp_http_client_set_url(client, (url + "/bot" + token + "/sendMessage").c_str());
	esp_http_client_set_method(client, HTTP_METHOD_POST);
	esp_http_client_set_header(client, "Content-Type", "application/json");

	//Тело POST-запроса
	json	body_json{
		{"chat_id", chat_id},
		{"text", text}
	};

	if(!parse_mode.empty())
		body_json["parse_mode"]	= parse_mode;
	if(reply_id)
		body_json["reply_to_message_id"]	= reply_id;

	std::string	body	= body_json.dump();
	ESP_LOGI(TAG, "send body = %s", body.c_str());
	esp_http_client_set_post_field(client, body.c_str(), body.size());

	//Выполнение запроса
	esp_err_t	err	= esp_http_client_perform(client);
	if(err == ESP_OK)
	{
		http_fail_status	= false;
		return true;
	}
	else
	{
		ESP_LOGE(TAG_send, "sendMessage POST request failed: %s", esp_err_to_name(err));
		http_fail_status	= true;
		return false;
	}
}

const std::vector<Telegram_client::Message>&	Telegram_client::getMessages(int timeout)
{
	if(http_fail_status)
	{
		return messages;
	}

	//Создание запроса
	esp_http_client_set_url(client, (url + "/bot" + token + "/getUpdates").c_str());
	esp_http_client_set_method(client, HTTP_METHOD_POST);
	esp_http_client_set_header(client, "Content-Type", "application/json");

	//Тело POST-запроса
	json	body_json{
		{"timeout", timeout}
	};

	if(update_id)
		body_json["offset"]	= update_id;

	std::string	body	= body_json.dump();

	esp_http_client_set_post_field(client, body.c_str(), body.size());

	//Выполнение запроса
	esp_err_t	err	= esp_http_client_perform(client);
	if(err == ESP_OK)
	{
		http_fail_status	= false;
		int	statusCode		= esp_http_client_get_status_code(client);
		int	content_length	= esp_http_client_get_content_length(client);

		//Финализация строки ответа
		http_reply[content_length] = 0;
		if(statusCode == 200)
		{
			if(parseBotReply())
			{
				ESP_LOGI(TAG, "New update_id = %s", std::to_string(update_id).c_str());
				update_id++;
			}
			else
			{
				ESP_LOGI(TAG, "Waiting for messages... heap = %d", int(esp_get_free_heap_size()));
			}
		}
		else
		{
			ESP_LOGI(TAG, "getMessages Status = %d, content = %s", statusCode, http_reply);
		}
	}
	else
	{
		ESP_LOGE(TAG, "getMessages POST request failed: %s", esp_err_to_name(err));
		http_fail_status	= true;
	}

	return messages;
}

bool	Telegram_client::parseBotReply()
{
	//Очистка набора сообщений
	messages.clear();

	//Разбор сообщения от бота
	json	j	= json::parse(http_reply, nullptr, false);
	if(j.is_discarded())
	{
		ESP_LOGE(TAG, "Ошибка при разборе json: %s", http_reply);
		return false;
	}

	if(!j.contains("ok"))		return false;
	if(!j.at("ok").get<bool>())	return false;
	if(!j.contains("result"))	return false;

	json result	= j.at("result");
	if(result.is_array())
	{
		//Пустой массив означает отсутствие обновлений
		if(result.empty())	return false;

		for(const json&	record : result)
		{
			if(record.contains("update_id"))
			{
				//Текущий update_id
				update_id	= record.at("update_id").get<uint64_t>();
			}

			if(record.contains("message"))
			{
				const json& message	= record.at("message");

				//Получение message_id
				int64_t message_id	= 0;
				if(message.contains("message_id"))
					message_id	= message.at("message_id").get<int64_t>();

				//Получение chat_id
				int64_t	chat_id	= 0;
				if(message.contains("chat"))
				{
					const json&	chat	= message.at("chat");
					if(chat.contains("id"))
						chat_id	= chat.at("id").get<int64_t>();
				}

				//Проверка авторизации
				bool	authorized	= false;
				if(message.contains("from"))
				{
					const json& from	= message.at("from");
					if(from.contains("id"))
					{
						const int64_t user_id	= from.at("id").get<int64_t>();
						authorized	= (std::find(authorized_users.begin(), authorized_users.end(), user_id) != authorized_users.end());
					}
				}

				//Текстовое сообщение
				if(message.contains("text"))
				{
					std::string	text	= message.at("text").get<std::string>();
					printf("%s\n", text.c_str());

					if(text.front() == '/')
					{
						Message	msg;
						msg.chat_id		= chat_id;
						msg.message_id	= message_id;
						msg.authorized	= authorized;
						msg.text		= text;

						messages.push_back(msg);
					}
				}

				//Файл прошивки
				if(message.contains("document"))
				{
					const json&	document	= message.at("document");
					if(document.contains("file_name"))
					{
						std::string	file_name	= document.at("file_name").get<std::string>();
						if(file_name == SecureConfig::firmware_name)
						{
							//Получена прошивка
							if(document.contains("file_id"))
							{
								std::string	file_id	= document.at("file_id").get<std::string>();

								Message	msg;
								msg.chat_id		= chat_id;
								msg.message_id	= message_id;
								msg.authorized	= authorized;
								msg.text		= file_id;
								msg.is_firmware	= true;

								messages.push_back(msg);
							}
						}
					}
				}
			}

			if(record.contains("callback_query"))
			{
				//Сделать проверку chat_id!!!!!
				const json&	query	= record.at("callback_query");
				if(query.contains("data"))
				{
					Message	msg;
					msg.message_id	= 0;
					msg.text		= query.at("data").get<std::string>();
					msg.authorized	= true;

					messages.push_back(msg);
				}
			}
		}

		return true;
	}

	return true;
}

void	Telegram_client::reconnect()
{
	if(wifi_connected)
	{
		create_client();
		http_fail_status	= false;
	}
}

static const char* TAG	= "_http_event_handler";
esp_err_t	_http_event_handler(esp_http_client_event_t* evt)
{
	static char *output_buffer;  // Buffer to store response of http request from event handler
	static int output_len;       // Stores number of bytes read
	switch(evt->event_id) {
		case HTTP_EVENT_ERROR:
			{
				ESP_LOGD(TAG, "HTTP_EVENT_ERROR");
			}break;

		case HTTP_EVENT_REDIRECT:
			{
				ESP_LOGD(TAG, "HTTP_EVENT_REDIRECT");
			}break;

		case HTTP_EVENT_ON_CONNECTED:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_CONNECTED");
			break;
		case HTTP_EVENT_HEADER_SENT:
			ESP_LOGD(TAG, "HTTP_EVENT_HEADER_SENT");
			break;
		case HTTP_EVENT_ON_HEADER:
			ESP_LOGD(TAG, "HTTP_EVENT_ON_HEADER, key=%s, value=%s", evt->header_key, evt->header_value);
			break;
		case HTTP_EVENT_ON_DATA:
			{
				ESP_LOGD(TAG, "HTTP_EVENT_ON_DATA, len=%d", evt->data_len);
				/*
					*  Check for chunked encoding is added as the URL for chunked encoding used in this example returns binary data.
					*  However, event handler can also be used in case chunked encoding is used.
					*/
				if (!esp_http_client_is_chunked_response(evt->client)) {
					// If user_data buffer is configured, copy the response into the buffer
					if (evt->user_data) {
						memcpy((char*)evt->user_data + output_len, evt->data, evt->data_len);
					} else {
						if (output_buffer == NULL) {
							output_buffer = (char *) malloc(esp_http_client_get_content_length(evt->client));
							output_len = 0;
							if (output_buffer == NULL) {
								ESP_LOGE(TAG, "Failed to allocate memory for output buffer");
								return ESP_FAIL;
							}
						}
						memcpy(output_buffer + output_len, evt->data, evt->data_len);
					}
					output_len += evt->data_len;
				}
			}break;

		case HTTP_EVENT_ON_FINISH:
		{
			ESP_LOGD(TAG, "HTTP_EVENT_ON_FINISH");
			if (output_buffer != NULL) {
				// Response is accumulated in output_buffer. Uncomment the below line to print the accumulated response
				// ESP_LOG_BUFFER_HEX(TAG, output_buffer, output_len);
				free(output_buffer);
				output_buffer = NULL;
			}
			output_len = 0;
		}break;

		case HTTP_EVENT_DISCONNECTED:
		{	//ESP_LOGI(TAG, "HTTP_EVENT_DISCONNECTED");
			int mbedtls_err = 0;
			esp_err_t err = esp_tls_get_and_clear_last_error((esp_tls_error_handle_t)(evt->data), &mbedtls_err, nullptr);
			if (err != 0) {
				if (output_buffer != NULL) {
					free(output_buffer);
					output_buffer = NULL;
				}
				output_len = 0;
				ESP_LOGI(TAG, "Last esp error code: 0x%x", err);
				ESP_LOGI(TAG, "Last mbedtls failure: 0x%x", mbedtls_err);
			}
		}break;
	}
	return ESP_OK;
}

bool	Telegram_client::sendFile(const int64_t& chat_id, const std::string& contentType, const std::string& filename, const size_t& filesize, const uint8_t* data)
{
	if(http_fail_status)	return false;

	//Создание запроса
	esp_http_client_set_url(client, (url + "/bot" + token + "/sendDocument?chat_id=" + std::to_string(chat_id)).c_str());
	esp_http_client_set_method(client, HTTP_METHOD_POST);

	const std::string	boundary("------------------------b8f610217e83e29b");

	std::string	start_request;
	start_request += "--";
	start_request += boundary;
	start_request += "\r\ncontent-disposition: form-data; name=\"document\"; filename=\"";
	start_request += filename;
	start_request += "\"\r\n" "Content-Type: ";
	start_request += contentType;
	start_request += "\r\n\r\n";

	std::string	end_request;
	end_request += "\r\n" "--";
	end_request += boundary;
	end_request += "--" "\r\n";

	size_t	contentLength = filesize + start_request.size() + end_request.size();
	esp_http_client_set_header(client, "Content-Length", std::to_string(contentLength).c_str());
	esp_http_client_set_header(client, "Content-Type", (std::string("multipart/form-data; boundary=") + boundary).c_str());

	esp_err_t	err	= esp_http_client_open(client, contentLength);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Faild to open client: %s", esp_err_to_name(err));
		return false;
	}

	//Стартовая запись
	int	wlen	= esp_http_client_write(client, start_request.c_str(), start_request.size());
	if(wlen < 0)	{ESP_LOGE(TAG, "Write failed");}

	//Отправка данных
	const char*	body	= (const char*)data;
	size_t	to_send		= filesize;

	while(to_send > 0)
	{
		size_t	send_size	= std::min(to_send, size_t(16384));
		wlen	= esp_http_client_write(client, body, send_size);
		if(wlen < 0)
		{
			ESP_LOGE(TAG, "Write failed");
			http_fail_status	= true;
			esp_http_client_close(client);
			return false;
		}
		else
		{
			body	+= wlen;
			to_send	-= wlen;
		}
	}

	//Финализация записи
	wlen	= esp_http_client_write(client, end_request.c_str(), end_request.size());
	if(wlen < 0)	{ESP_LOGE(TAG, "Write failed");}

	//Ответ сервера
	int	content_length	= esp_http_client_fetch_headers(client);
	if(content_length < 0)
	{
		ESP_LOGE(TAG, "HTTP client fetch headers failed");
		http_fail_status	= true;
		esp_http_client_close(client);
		return false;
	}
	else
	{
		int data_read = esp_http_client_read_response(client, http_reply, 16384);
		if(data_read < 0)
		{
			ESP_LOGE(TAG, "Failed to read response");
			http_fail_status	= true;
			esp_http_client_close(client);
			return false;
		}
	}

	esp_http_client_close(client);
	return true;
}

bool	Telegram_client::sendSPIFFS(const int64_t& chat_id, const std::string& contentType, const std::string& filename, const char* spiffs)
{
	//Определение размера файла
    struct stat st;
    stat(spiffs, &st);
	size_t	file_size	= st.st_size;
	std::ifstream	file(spiffs);
	if(!file.is_open())
	{
		ESP_LOGE(TAG, "Failed to open file for reading");
		return false;
	}

	if(http_fail_status)	return false;

	//Создание запроса
	esp_http_client_set_url(client, (url + "/bot" + token + "/sendDocument?chat_id=" + std::to_string(chat_id)).c_str());
	esp_http_client_set_method(client, HTTP_METHOD_POST);

	const std::string	boundary("------------------------b8f610217e83e29b");

	std::string	start_request;
	start_request += "--";
	start_request += boundary;
	start_request += "\r\ncontent-disposition: form-data; name=\"document\"; filename=\"";
	start_request += filename;
	start_request += "\"\r\n" "Content-Type: ";
	start_request += contentType;
	start_request += "\r\n\r\n";

	std::string	end_request;
	end_request += "\r\n" "--";
	end_request += boundary;
	end_request += "--" "\r\n";

	size_t	contentLength = file_size + start_request.size() + end_request.size();
	esp_http_client_set_header(client, "Content-Length", std::to_string(contentLength).c_str());
	esp_http_client_set_header(client, "Content-Type", (std::string("multipart/form-data; boundary=") + boundary).c_str());

	esp_err_t	err	= esp_http_client_open(client, contentLength);
	if(err != ESP_OK)
	{
		ESP_LOGE(TAG, "Faild to open client: %s", esp_err_to_name(err));
		return false;
	}

	//Стартовая запись
	int	wlen	= esp_http_client_write(client, start_request.c_str(), start_request.size());
	if(wlen < 0)	{ESP_LOGE(TAG, "Write failed");}

	//Отправка данных
	size_t	to_send		= file_size;

	while(!(file >> std::ws).eof())
	{
		file.read(send_buf, 16384);
		size_t	send_size	= std::min(to_send, size_t(16384));
		wlen	= esp_http_client_write(client, send_buf, send_size);
		if(wlen < 0)
		{
			ESP_LOGE(TAG, "Write failed");
			http_fail_status	= true;
			esp_http_client_close(client);
			return false;
		}
		else
		{
			to_send	-= wlen;
		}
	}

	//Финализация записи
	wlen	= esp_http_client_write(client, end_request.c_str(), end_request.size());
	if(wlen < 0)	{ESP_LOGE(TAG, "Write failed");}

	//Ответ сервера
	int	content_length	= esp_http_client_fetch_headers(client);
	if(content_length < 0)
	{
		ESP_LOGE(TAG, "HTTP client fetch headers failed");
		http_fail_status	= true;
		esp_http_client_close(client);
		return false;
	}
	else
	{
		int data_read = esp_http_client_read_response(client, http_reply, 16384);
		if(data_read < 0)
		{
			ESP_LOGE(TAG, "Failed to read response");
			http_fail_status	= true;
			esp_http_client_close(client);
			return false;
		}
		http_reply[data_read]	= 0;
		ESP_LOGI(TAG, "data_read = %d", data_read);
		ESP_LOGI(TAG, "http_reply = %s", http_reply);
	}

	esp_http_client_close(client);
	return true;
}

esp_err_t	Telegram_client::update_firmware(const Message& message)
{
	//Запрос ссылки на файл прошивки
	esp_http_client_set_url(client, (url + "/bot" + token + "/getFile").c_str());
	esp_http_client_set_method(client, HTTP_METHOD_POST);
	esp_http_client_set_header(client, "Content-Type", "application/json");

	json	body_json{
		{"file_id", message.text}
	};
	std::string	body	= body_json.dump();
	esp_http_client_set_post_field(client, body.c_str(), body.size());

	//Выполнение запроса
	esp_err_t	err	= esp_http_client_perform(client);
	if(err == ESP_OK)
	{
		int	statusCode		= esp_http_client_get_status_code(client);
		int	content_length	= esp_http_client_get_content_length(client);

		//Финализация строки ответа
		http_reply[content_length] = 0;
		if(statusCode == 200)
		{
			ESP_LOGI("update_firmware", "Получена ссылка на файл");

			//Разбор сообщения
			json	j	= json::parse(http_reply, nullptr, false);
			if(j.is_discarded())
			{
				ESP_LOGE("update_firmware", "Ошибка в строке json: %s", http_reply);
				return ESP_FAIL;
			}
			else
			{
				if(!j.contains("ok"))		return false;
				if(!j.at("ok").get<bool>())	return false;
				if(!j.contains("result"))	return false;

				json result	= j.at("result");
				if(result.contains("file_path"))
				{
					std::string	file_path	= result.at("file_path").get<std::string>();
					ESP_LOGI("update_firmware", "file_path: %s", file_path.c_str());
					std::string	file_url	= url + "/file/bot" + token + "/" + file_path;
					ESP_LOGI("update_firmware", "file_url: %s", file_url.c_str());

					esp_http_client_config_t	config;
					memset(&config, 0, sizeof(config));
					config.url 		= file_url.c_str();
					config.cert_pem = server_root_cert_pem_start;

					esp_https_ota_config_t ota_config;
					memset(&ota_config, 0, sizeof(ota_config));
					ota_config.http_config = &config;

					//Закрытие существующего подключения
					esp_http_client_close(client);
					esp_http_client_cleanup(client);

					err	= esp_https_ota(&ota_config);
					if(err != ESP_OK){
						//Восстановление подключения к telegram в случае ошибки обновления
						create_client();
					}
				}
				else
					return ESP_FAIL;
			}
		}
		else
		{
			ESP_LOGI("update_firmware", "Status = %d, content = %s", statusCode, http_reply);
		}
	}
	else
	{
		ESP_LOGE("update_firmware", "POST request failed: %s", esp_err_to_name(err));
		http_fail_status	= true;
	}

	return err;
}
