#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "lwip/err.h"
#include "lwip/sys.h"

static const char*	TAG = "wifi_main";
static EventGroupHandle_t	s_wifi_event_group	= nullptr;
volatile bool	wifi_connected	= false;

#define WIFI_CONNECTED_BIT	BIT0
#define WIFI_FAIL_BIT		BIT1

static void	event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
	if(event_base == WIFI_EVENT)
	{
		switch(event_id)
		{
			case WIFI_EVENT_STA_START:	esp_wifi_connect();	break;
			case WIFI_EVENT_STA_DISCONNECTED:
			{
				wifi_connected	= false;
				vTaskDelay(pdMS_TO_TICKS(10000));
				esp_wifi_connect();
				ESP_LOGI(TAG, "try to reconnect to the AP");
			}break;

			default:
				break;
		}
	}
	else if(event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
	{
		wifi_connected	= true;
		ip_event_got_ip_t*	event	= (ip_event_got_ip_t*)event_data;
		ESP_LOGI(TAG, "got ip:" IPSTR, IP2STR(&event->ip_info.ip));
		xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
	}
}

void wifi_init_sta(const char* ssid, const char* pass)
{
	s_wifi_event_group = xEventGroupCreate();
	ESP_ERROR_CHECK(esp_netif_init());
	ESP_ERROR_CHECK(esp_event_loop_create_default());
	esp_netif_create_default_wifi_sta();

	wifi_init_config_t	cfg = WIFI_INIT_CONFIG_DEFAULT();
	ESP_ERROR_CHECK(esp_wifi_init(&cfg));

	esp_event_handler_instance_t	instance_any_id;
	esp_event_handler_instance_t	instance_got_ip;
	ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, nullptr, &instance_any_id));
	ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, nullptr, &instance_got_ip));

	wifi_config_t	wifi_config;
	memset(&wifi_config, 0, sizeof(wifi_config));
	strcpy((char*)wifi_config.sta.ssid, ssid);
	strcpy((char*)wifi_config.sta.password, pass);
	wifi_config.sta.threshold.authmode	= WIFI_AUTH_WPA2_PSK;
	wifi_config.sta.pmf_cfg.capable		= true;
	wifi_config.sta.pmf_cfg.required	= false;

	ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
	ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
	ESP_ERROR_CHECK(esp_wifi_start());

	/* Waiting until either the connection is established (WIFI_CONNECTED_BIT) or connection failed for the maximum
		* number of re-tries (WIFI_FAIL_BIT). The bits are set by event_handler() (see above) */
	EventBits_t	bits	= xEventGroupWaitBits(s_wifi_event_group,
			WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
			pdFALSE,
			pdFALSE,
			portMAX_DELAY);

	/* xEventGroupWaitBits() returns the bits before the call returned, hence we can test which event actually
		* happened. */
	if(bits & WIFI_CONNECTED_BIT)	{ESP_LOGI(TAG, "connected to %s", ssid);}
	else if(bits & WIFI_FAIL_BIT)	{ESP_LOGI(TAG, "Failed to connect to SSID:%s, password:%s", ssid, pass);}
	else {ESP_LOGE(TAG, "UNEXPECTED EVENT");}

	/* The event will not be processed after unregister */
//	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(IP_EVENT, IP_EVENT_STA_GOT_IP, instance_got_ip));
//	ESP_ERROR_CHECK(esp_event_handler_instance_unregister(WIFI_EVENT, ESP_EVENT_ANY_ID, instance_any_id));
//	vEventGroupDelete(s_wifi_event_group);
}
