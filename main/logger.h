#ifndef LOGGER_H
#define LOGGER_H

#include "telegram_client.h"
void	logger(void* unused);
void	send_log(Telegram_client* bot, int64_t chat_id, bool create_new);

#endif  //LOGGER_H