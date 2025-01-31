#ifndef TCP_SERVER_H
#define TCP_SERVER_H

void	tcp_server(void* unused);
bool	tcp_server_is_running();

enum class TCP_message_t : uint8_t {set_boiler_data, BLOR, test_ot_command, baxi_ampera_thermostat, PID_thermostat};

//Структуры для очередей обмена сообщениями с OpenTherm
struct	fromTCP_to_ot
{
	TCP_message_t	type;	//Тип сообщения
	json			params;	//Параметры команды
};

struct	toTCP_from_ot
{
	json	response;
};

extern QueueHandle_t from_TCP_ot_queue;
extern QueueHandle_t to_TCP_ot_queue;


#endif  //TCP_SERVER_H