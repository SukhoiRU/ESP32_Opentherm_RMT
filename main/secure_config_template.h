#ifndef SECURE_PARAMS_H
#define SECURE_PARAMS_H

namespace SecureConfig{
	//Настройки Wi-Fi
	constexpr	const char*		wifi_ssid			= "";
	constexpr	const char*		wifi_password		= "";

	//Настройки telegram bot
	constexpr	const char*		bot_API_token		= "";
	constexpr	const int64_t	telegram_user_id	= 0;				//Главный пользователь бота
	constexpr	const int64_t	common_chat_id		= 0;				//Общий чат для ежедневных логов
	constexpr	const char*		firmware_name		= "project.bin";	//Название прошивки для проверок OTA
	constexpr	const std::initializer_list<int64_t>					//Список id пользователей бота, которым разрешен доступ
	telegram_acsess_list	= {
		0
	};

	//MQTT
	constexpr	const char*	mqtt_broker				= "mqtt://...";
	constexpr	const char*	boiler_topic			= "...Котёл/";
	constexpr	const char*	boiler_OT_topic			= "...OpenTherm/";
	constexpr	const char*	boiler_command_topic	= "...Котёл/command/";
	constexpr	const char*	thermo_topic			= "...thermometers/";
	constexpr	const char*	thermo_errors_topic		= "...thermo_errors/";
	constexpr	const char*	boiler_debug_topic		= "...boiler_task/from_TCP_ot_queue/";

	//Датчики температуры
	const std::vector<std::pair<uint64_t, std::string>>	known_sensors{
		{0x..., "Улица"},
		{0x..., "1 этаж"},
		{0x..., "2 этаж"}
	};
}

#endif	//SECURE_PARAMS_H
