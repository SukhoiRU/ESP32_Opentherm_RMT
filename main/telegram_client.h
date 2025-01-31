#ifndef TELEGRAM_CLIENT_H
#define TELEGRAM_CLIENT_H

#include <string>
#include <vector>
#include "esp_http_client.h"

class	Telegram_client
{
public:
	struct Message
	{
		int64_t	chat_id		= 0;
		int64_t	message_id	= 0;
		std::string	text;
		bool	authorized	= true;
		bool	is_firmware	= false;
	};

private:
	const char*	TAG = "telegram";
	const char*	TAG_send = "telegram_send";

	esp_http_client_handle_t	client	= nullptr;

	const std::string	url{"https://api.telegram.org"};
	const std::string	token;
	std::vector<int64_t>	authorized_users;
	char*	http_reply	= nullptr;
	bool	http_fail_status	= false;

	uint64_t	update_id	= 0;
	std::vector<Message>	messages;

	void	create_client();
	bool	parseBotReply();

public:

	explicit Telegram_client(const std::string& token, char* buf);
	virtual ~Telegram_client();

	void	setUsers(const std::initializer_list<int64_t>& users);
	bool	sendMessage(const std::string& text, const int64_t& chat_id, const int64_t& reply_id = 0, const std::string& parse_mode = "");
	const std::vector<Telegram_client::Message>&	getMessages(int timeout);
	bool	http_fail(){return http_fail_status;}
	void	reconnect();
	bool	sendFile(const int64_t& chat_id, const std::string& contentType, const std::string& filename, const size_t& filesize, const uint8_t* data);
	bool	sendSPIFFS(const int64_t& chat_id, const std::string& contentType, const std::string& filename, const char* spiffs);

	esp_err_t	update_firmware(const Message& message);
};

#endif  //TELEGRAM_CLIENT_H