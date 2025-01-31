#include <cstdio>
#include <cmath>
#include <cstring>
#include <string>
#include <vector>
#include "esp_log.h"
#include "esp_log.h"
#include "esp_system.h"
#include "sdkconfig.h"
#include "mqtt.h"

static const char*	TAG = "mqtt_task";

//Глобальные переменные
esp_mqtt_client_handle_t	mqtt_client	= nullptr;
std::string					mqtt_uri;
std::vector<std::string>	mqtt_topic_list;	//Список запрашиваемых топиков

static void log_error_if_nonzero(const char *message, int error_code)
{
	if (error_code != 0)
		ESP_LOGE(TAG, "Last error %s: 0x%x", message, error_code);
}

/*
 * @brief Event handler registered to receive MQTT events
 *
 *  This function is called by the MQTT client event loop.
 *
 * @param handler_args user data registered to the event.
 * @param base Event base for the handler(always MQTT Base in this example).
 * @param event_id The id for the received event.
 * @param event_data The data for the event, esp_mqtt_event_handle_t.
 */
static void mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
	ESP_LOGD(TAG, "Event dispatched from event loop base=%s, event_id=%" PRIi32 "", base, event_id);
	esp_mqtt_event_handle_t		event	= reinterpret_cast<esp_mqtt_event_handle_t>(event_data);
	esp_mqtt_client_handle_t	client	= event->client;

	// int	msg_id;
	switch((esp_mqtt_event_id_t)(event_id))
	{
		case MQTT_EVENT_CONNECTED:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_CONNECTED");

			//Передача клиента в глобальную видимость
			mqtt_client	= client;
		}break;

		case MQTT_EVENT_DISCONNECTED:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_DISCONNECTED");

			//Сброс клиента
			mqtt_client	= nullptr;

			//Повторное подключение
			esp_mqtt_client_start(client);
		}break;

		case MQTT_EVENT_SUBSCRIBED:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED, msg_id=%d", event->msg_id);
		}break;

		case MQTT_EVENT_UNSUBSCRIBED:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED, msg_id=%d", event->msg_id);
		}break;

		case MQTT_EVENT_PUBLISHED:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED, msg_id=%d", event->msg_id);
		}break;

		case MQTT_EVENT_DATA:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_DATA");
			printf("TOPIC=%.*s\r\n", event->topic_len, event->topic);
			printf("DATA=%.*s\r\n", event->data_len, event->data);


		}break;

		case MQTT_EVENT_ERROR:
		{
			ESP_LOGI(TAG, "MQTT_EVENT_ERROR");
			if(event->error_handle->error_type == MQTT_ERROR_TYPE_TCP_TRANSPORT)
			{
				log_error_if_nonzero("reported from esp-tls", event->error_handle->esp_tls_last_esp_err);
				log_error_if_nonzero("reported from tls stack", event->error_handle->esp_tls_stack_err);
				log_error_if_nonzero("captured as transport's socket errno", event->error_handle->esp_transport_sock_errno);
				ESP_LOGI(TAG, "Last errno string (%s)", strerror(event->error_handle->esp_transport_sock_errno));
			}
		}break;

		default:
		{
			ESP_LOGI(TAG, "Other event id:%d", event->event_id);
		}break;
	}
}

void	mqtt_init(const char* uri)
{
	//Создание клиента MQTT
	esp_mqtt_client_config_t	mqtt_cfg;
	memset(&mqtt_cfg, 0, sizeof(mqtt_cfg));
	mqtt_cfg.broker.address.uri	= uri;
	mqtt_uri	= uri;

	esp_mqtt_client_handle_t	client	= esp_mqtt_client_init(&mqtt_cfg);
	esp_mqtt_client_register_event(client, esp_mqtt_event_id_t::MQTT_EVENT_ANY, mqtt_event_handler, NULL);
	esp_mqtt_client_start(client);
}
