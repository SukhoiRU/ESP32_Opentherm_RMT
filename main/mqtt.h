#ifndef MQTT_H
#define MQTT_H
#include <mqtt_client.h>

extern esp_mqtt_client_handle_t	mqtt_client;
extern QueueHandle_t			from_mqtt_queue;
extern std::vector<std::string>	mqtt_topic_list;	//Список запрашиваемых топиков

struct	fromMQTT
{
	std::string		topic;
	std::string		text;
};

void	mqtt_init(const char* uri);


#endif  //MQTT_H