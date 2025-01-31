#ifndef TELEGRAM_H
#define TELEGRAM_H

#include <string>
enum class telegram_message_t : uint8_t {floor_1_on, floor_1_off, floor_2_on, floor_2_off,program, reset_worktime, set_worktime};
enum class telegram_ot_message_t : uint8_t {set_boiler_data, BLOR};

struct	fromTelegram
{
	telegram_message_t	type;
	int64_t			chat_id		= 0;
	int64_t			reply_id	= 0;
	std::string		text;
};

struct	fromTelegram_ot
{
	telegram_ot_message_t	type;
	int64_t			chat_id		= 0;
	int64_t			reply_id	= 0;
	std::string		text;
};

struct	toTelegram
{
	std::string		text;
	std::string		parse_mode;
	int64_t			chat_id		= 0;
	int64_t			reply_id	= 0;
};

extern QueueHandle_t from_telegram_gpio_queue;
extern QueueHandle_t from_telegram_ot_queue;
extern QueueHandle_t to_telegram_queue;

void	telegram(void* unused);

#endif  //TELEGRAM_H