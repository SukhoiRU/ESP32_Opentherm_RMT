#include <string>
#include <sstream>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "esp_timer.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "driver/gpio.h"
#include "json.hpp"
using json = nlohmann::json;

#include "secure_config.h"
#include "ot_boiler.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "rmt_opentherm.h"
#include "telegram.h"
#include "mqtt.h"

static const char*	TAG = "ot_boiler";

constexpr	uint32_t	bit_0	= 0x00000001;
constexpr	uint32_t	bit_1	= 0x00000002;
constexpr	uint32_t	bit_2	= 0x00000004;
constexpr	uint32_t	bit_3	= 0x00000008;
constexpr	uint32_t	bit_4	= 0x00000010;
constexpr	uint32_t	bit_5	= 0x00000020;
constexpr	uint32_t	bit_6	= 0x00000040;
constexpr	uint32_t	bit_7	= 0x00000080;
constexpr	uint32_t	bit_8	= 0x00000100;
constexpr	uint32_t	bit_9	= 0x00000200;
constexpr	uint32_t	bit_10	= 0x00000400;
constexpr	uint32_t	bit_11	= 0x00000800;
constexpr	uint32_t	bit_12	= 0x00001000;
constexpr	uint32_t	bit_13	= 0x00002000;
constexpr	uint32_t	bit_14	= 0x00004000;
constexpr	uint32_t	bit_15	= 0x00008000;
constexpr	uint32_t	bit_16	= 0x00010000;
constexpr	uint32_t	bit_17	= 0x00020000;
constexpr	uint32_t	bit_18	= 0x00040000;
constexpr	uint32_t	bit_19	= 0x00080000;
constexpr	uint32_t	bit_20	= 0x00100000;
constexpr	uint32_t	bit_21	= 0x00200000;
constexpr	uint32_t	bit_22	= 0x00400000;
constexpr	uint32_t	bit_23	= 0x00800000;
constexpr	uint32_t	bit_24	= 0x01000000;
constexpr	uint32_t	bit_25	= 0x02000000;
constexpr	uint32_t	bit_26	= 0x04000000;
constexpr	uint32_t	bit_27	= 0x08000000;
constexpr	uint32_t	bit_28	= 0x10000000;
constexpr	uint32_t	bit_29	= 0x20000000;
constexpr	uint32_t	bit_30	= 0x40000000;
constexpr	uint32_t	bit_31	= 0x80000000;

OT_Boiler::OT_Boiler(const gpio_num_t pin_in, const gpio_num_t pin_out, const std::string& topic, const std::string& OT_topic, const int slave_ID)
{
	boiler_topic	= topic;
	boiler_OT_topic	= OT_topic;
	slaveID			= slave_ID;
	ot_boiler_state.faultFlags.all	= 0;

	//Настройка шины Opentherm
	rmt_ot	= new RMT_Opentherm(pin_in, pin_out, boiler_OT_topic + "RMT");

	//Чтение прошлых настроек
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READONLY, &nvs_settings) == ESP_OK)
	{
		uint8_t	val;
		if(nvs_get_u8(nvs_settings, "CH", &val) == ESP_OK)				ot_boiler_data.CH			= (val != 0);
		if(nvs_get_u8(nvs_settings, "DHW", &val) == ESP_OK)				ot_boiler_data.DHW			= (val != 0);
		if(nvs_get_u8(nvs_settings, "SummerMode", &val) == ESP_OK)		ot_boiler_data.SummerMode	= (val != 0);
		if(nvs_get_u8(nvs_settings, "ch_temp_zad", &val) == ESP_OK)		ot_boiler_data.ch_temp_zad	= val;
		if(nvs_get_u8(nvs_settings, "ch_temp_max", &val) == ESP_OK)		ot_boiler_data.ch_temp_max	= val;
		if(nvs_get_u8(nvs_settings, "ch_mod_max", &val) == ESP_OK)		ot_boiler_data.ch_mod_max	= val;
		nvs_close(nvs_settings);
	}
}

bool	OT_Boiler::check_parity(uint32_t frame)
{
	//Проверка нечетности
	uint8_t	p = 0;
	while(frame > 0)
	{
		if(frame & 1)	p++;
		frame	= frame >> 1;
	}

	return (p & 1);
}

OT_Boiler::OT_Response	OT_Boiler::processOT(const Command cmd, const uint8_t id, const uint16_t data, bool data_invalid_expected /* = false */)
{
	uint8_t	msg_type;
	switch(cmd)
	{
		case Command::write:	msg_type = static_cast<uint8_t>(MsgType::WRITE_DATA);	break;
		case Command::invalid:	msg_type = static_cast<uint8_t>(MsgType::INVALID_DATA);	break;
		default:				msg_type = static_cast<uint8_t>(MsgType::READ_DATA);	break;
	}

	//Формирование запроса
	OT_Message_t	request;
	request.all				= 0;
	request.bit.msg_type	= msg_type;
	request.bit.id			= id;
	request.bit.spare		= 0;
	request.bit.data		= data;
	request.bit.parity		= check_parity(request.all);

	//Отправка команды
	OT_Message_t	response;
	std::vector<rmt_symbol_word_t>	received_symbols;
	RMT_Opentherm::Result	response_status	= rmt_ot->processOT(request.all, &response.all, received_symbols);

	OT_Response	out;
	out.data		= 0;
	out.response	= response.all;
	out.status 		= OT_Status::sucsess;

	if(response_status == RMT_Opentherm::Result::sucsess){
		//Проверка четности
		if(check_parity(response.all))			{failsCounter.parityFail++;								out.status	= OT_Status::parityFail;}
		else{
			//Проверка на ACK
			switch(static_cast<MsgType>(response.bit.msg_type)){
				case MsgType::READ_ACK:			{if(cmd == Command::write)	{failsCounter.ACK_fail++;	out.status	= OT_Status::ACK_fail;}}	break;	//READ_ACK
				case MsgType::WRITE_ACK:		{if(cmd == Command::read) 	{failsCounter.ACK_fail++;	out.status	= OT_Status::ACK_fail;}}	break;	//WRITE_ACK
				case MsgType::DATA_INVALID:		{if(!data_invalid_expected) {failsCounter.dataInvalid++;out.status	= OT_Status::dataInvalid;}}	break;	//DATA_INVALID
				case MsgType::UNKNOWN_DATAID:	{failsCounter.unknownID++;								out.status	= OT_Status::unknownID;}	break;	//UNKNOWN_DATAID
				default:						{failsCounter.msgType_unknown++;						out.status	= OT_Status::msgType_unknown;}
			}

			//Проверка на SPARE
			if(out.status == OT_Status::sucsess && response.bit.spare != 0)
												{failsCounter.SPARE_fail++;								out.status	= OT_Status::SPARE_fail;}

			if(out.status == OT_Status::sucsess){
				//Проверка на ID
				if(response.bit.id != id)		{failsCounter.responseID_fail++;						out.status	= OT_Status::responseID_fail;}
				else{
					//И только в этом случае все проверки на корректность ответа котла пройдены!
					out.data	= response.bit.data;

					//Сброс счетчика ошибок связи
					if(error_counter >= 60)
						sendNotification("Восстановление связи по цифровой шине");
					error_counter	= 0;

					// //Отладочная печать
					// std::ostringstream	ss;
					// ss << "request id: " << int(id) << std::endl;
					// ss << "request: 0x" << std::hex << request.all << std::dec << std::endl;
					// ss << "response: 0x" << std::hex << out.data << std::dec << std::endl;
					// ss << "num_symbols: " << received_symbols.size() << std::endl;
					// for(const rmt_symbol_word_t& word : received_symbols)
					// {
					// 	ss << "(" << word.level0 << ": " << word.duration0 << "; ";
					// 	ss << word.level1 << ": " << word.duration1 << ")" << std::endl;
					// }

					// if(mqtt_client){
					// 	ss << std::endl;
					// 	esp_mqtt_client_publish(mqtt_client, (boiler_OT_topic + "log_info").c_str(), ss.str().c_str(), 0, 0, 0);
					// }
				}
			}
		}
	}else if(response_status == RMT_Opentherm::Result::receive_timeout){
		failsCounter.timeout++;
		out.status	= OT_Status::timeout;
	}
	else{
		failsCounter.rx_invalid++;
		out.status	= OT_Status::rx_invalid;
	}

	if(out.status != OT_Status::sucsess && out.status != OT_Status::timeout){
		//Ошибка опроса котла
		json	fails{
			{"counters", {
				{"notInited",			failsCounter.notInited},
				{"timeout", 			failsCounter.timeout},
				{"rx_invalid", 			failsCounter.rx_invalid},
				{"parityFail", 			failsCounter.parityFail},
				{"unknownID", 			failsCounter.unknownID},
				{"dataInvalid", 		failsCounter.dataInvalid},
				{"ACK_fail", 			failsCounter.ACK_fail},
				{"msgType_unknown", 	failsCounter.msgType_unknown},
				{"SPARE_fail", 			failsCounter.SPARE_fail},
				{"responseID_fail", 	failsCounter.responseID_fail}
			}},
			{"uptime", uint64_t(esp_timer_get_time()*0.000001)},
			{"request", {
				{"all", request.all},
				{"parity", uint8_t(request.bit.parity)},
				{"msg_type", uint8_t(request.bit.msg_type)},
				{"spare", uint8_t(request.bit.spare)},
				{"id", uint8_t(request.bit.id)},
				{"data", uint16_t(request.bit.data)}
			}},
			{"response", {
				{"all", response.all},
				{"parity", uint8_t(response.bit.parity)},
				{"msg_type", uint8_t(response.bit.msg_type)},
				{"spare", uint8_t(response.bit.spare)},
				{"id", uint8_t(response.bit.id)},
				{"data", uint16_t(response.bit.data)}
			}},
			{"status", OT_Status_to_string(out.status)}
		};

		char	buf[512];
		for(const rmt_symbol_word_t& word : received_symbols){
			sprintf(buf, "(%d: %d; %d: %d)", word.level0, word.duration0, word.level1, word.duration1);
			fails["symbols"].push_back(buf);
		}

		if(mqtt_client)
			esp_mqtt_client_publish(mqtt_client, (boiler_OT_topic + "fails").c_str(), fails.dump().c_str(), 0, 0, 0);

		error_counter++;
		if(error_counter > 61)	error_counter	= 61;
		if(error_counter == 60)
			sendNotification(std::string("Потеря связи по цифровой шине\n") + fails.dump(4));
	}

	return out;
}

size_t	OT_Boiler::repeat_old_messages()
{
	//Повтор прошлых неудачных обменов.
	//Если и повтор будет с ошибкой, то сообщение опять встанет в конец очереди
	size_t	repeat_size	= repeat_queue.size();
	for(size_t i = 0; i < repeat_size; i++)
	{
		RepeatQueue_t	item	= repeat_queue.front();
		repeat_queue.pop();
		size_t	size_before		= repeat_queue.size();

		if(item.counter < 5)
		{
			switch(item.msg)
			{
				case RepeatType::set_slave:				set_slave();										break;
				case RepeatType::read_slaveConfig:		read_slaveConfig();									break;
				case RepeatType::read_status:			read_status();										break;
				case RepeatType::read_faultCode:		read_faultCode();									break;
				case RepeatType::read_diagCode:			read_diagCode();									break;
				case RepeatType::read_ch_temp:			read_ch_temp();										break;
				case RepeatType::read_dhw_temp:			read_dhw_temp();									break;
				case RepeatType::read_modulation:		read_modulation();									break;
				case RepeatType::set_ch_temp_zad:		set_ch_temp_zad(ot_boiler_data.ch_temp_zad);		break;
				case RepeatType::set_dhw_temp_zad:		set_dhw_temp_zad(ot_boiler_data.dhw_temp_zad);		break;
				case RepeatType::set_ch_temp_max:		set_ch_temp_max(ot_boiler_data.ch_temp_max);		break;
				case RepeatType::set_ch_mod_max:		set_ch_mod_max(ot_boiler_data.ch_mod_max);			break;
				case RepeatType::BLOR:					BLOR();												break;

				default:
					break;
			}
		}
		else
		{
			std::string	msg("5 раз подряд не удалось выполнить команду:\n");
			switch (item.msg) {
				case OT_Boiler::RepeatType::set_slave:				msg += "set_slave";				break;
				case OT_Boiler::RepeatType::read_slaveConfig:		msg += "read_slaveConfig";		break;
				case OT_Boiler::RepeatType::read_status:			msg += "read_status";			break;
				case OT_Boiler::RepeatType::read_faultCode:			msg += "read_faultCode";		break;
				case OT_Boiler::RepeatType::read_diagCode:			msg += "read_diagCode";			break;
				case OT_Boiler::RepeatType::read_ch_temp:			msg += "read_ch_temp";			break;
				case OT_Boiler::RepeatType::read_dhw_temp:			msg += "read_dhw_temp";			break;
				case OT_Boiler::RepeatType::read_modulation:		msg += "read_modulation";		break;
				case OT_Boiler::RepeatType::set_ch_temp_zad:		msg += "set_ch_temp_zad";		break;
				case OT_Boiler::RepeatType::set_dhw_temp_zad:		msg += "set_dhw_temp_zad";		break;
				case OT_Boiler::RepeatType::set_ch_temp_max:		msg += "set_ch_temp_max";		break;
				case OT_Boiler::RepeatType::set_ch_mod_max:			msg += "set_ch_mod_max";		break;
				case OT_Boiler::RepeatType::BLOR:					msg += "BLOR";					break;
			}
			sendNotification(msg);
		}

		//Проверка, что сообщение вернулось в конец очереди
		if(repeat_queue.size() > size_before)
		{
			//Надо увеличить счетчик повторений.
			repeat_queue.back().counter	= item.counter++;
		}
	}

	return repeat_queue.size();
}

void	OT_Boiler::repeat(RepeatType msg)
{
	repeat_queue.push(RepeatQueue_t(msg, 0));
}

void	OT_Boiler::clear_old_message()
{
	while(!repeat_queue.empty())
		repeat_queue.pop();
}

void	OT_Boiler::set_slave()
{
	OT_Response	slave	= processOT(Command::write, 2, slaveID);
	if(slave.status	!= OT_Status::sucsess)
		repeat(RepeatType::set_slave);
}

void	OT_Boiler::read_slaveConfig()
{
	OT_Response	slaveConfig	= processOT(Command::read, 3, 0);
	if(slaveConfig.status == OT_Status::sucsess)
	{
		std::ostringstream	ss;
		ss << "Параметры котла:" << std::endl;
		ss << "DHW present: " << (!(slaveConfig.data & bit_0) ? "dhw not present" : "dhw is present ") << std::endl;
		ss << "Control type: " << (!(slaveConfig.data & bit_1) ? "modulating" : "on/off") << std::endl;
		ss << "Cooling: " << (!(slaveConfig.data & bit_2) ? "not supported" : "supported") << std::endl;
		ss << "DHW config: " << (!(slaveConfig.data & bit_3) ? "instantaneous or not-specified" : "storage tank") << std::endl;
		ss << "Master low-off&pump control function: " << (!(slaveConfig.data & bit_4) ? "allowed" : "not allowed") << std::endl;
		ss << "CH2 present: " << (!(slaveConfig.data & bit_4) ? "CH2 not present" : "CH2 present") << std::endl;

		ESP_LOGI(TAG, "slaveConfig: %s", ss.str().c_str());
	}
	else
		repeat(RepeatType::read_slaveConfig);
}

void	OT_Boiler::read_status()
{
	//Ежесекундный опрос состояния
	uint16_t data = 0;
	if(ot_boiler_data.CH)			data |=	bit_8;
	if(ot_boiler_data.DHW)			data |= bit_9;
	if(ot_boiler_data.SummerMode)	data |= bit_13;

	OT_Response	out	= processOT(Command::read, 0, data);
	if(out.status == OT_Status::sucsess)
	{
		//Разбор статуса котла
		bool	fault			= out.data & bit_0;
		bool	centralHeating	= out.data & bit_1;
		bool	dhw				= out.data & bit_2;
		bool	flame			= out.data & bit_3;

		if(fault != ot_boiler_state.fault)
		{
			ot_boiler_state.fault	= fault;
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "fault").c_str(), fault ? "1" : "0", 0, 0, 0);
		}

		if(centralHeating != ot_boiler_state.centralHeating)
		{
			ot_boiler_state.centralHeating	= centralHeating;
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "centralHeating").c_str(), centralHeating ? "1" : "0", 0, 0, 0);
		}

		if(dhw != ot_boiler_state.dhw)
		{
			ot_boiler_state.dhw	= dhw;
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "dhw").c_str(), dhw ? "1" : "0", 0, 0, 0);
		}

		if(flame != ot_boiler_state.flame)
		{
			ot_boiler_state.flame	= flame;
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "flame").c_str(), flame ? "1" : "0", 0, 0, 0);
		}

		//Однократное уведомление в телеграмм при первом появлении ошибки
		if(ot_boiler_state.fault)
		{
			if(is_first_fault)
			{
				is_first_fault		= false;

				read_faultCode();
				// read_diagCode();

				std::ostringstream ss;
				ss << "*Ошибка котла!*" << std::endl;
				if(ot_boiler_state.faultFlags.bit.ServiceRequest)	ss << "ServiceRequest" << std::endl;
				if(ot_boiler_state.faultFlags.bit.LockoutReset)		ss << "LockoutReset" << std::endl;
				if(ot_boiler_state.faultFlags.bit.LowWaterPress)	ss << "Мало давление теплоносителя" << std::endl;
				if(ot_boiler_state.faultFlags.bit.GasFlame_fault)	ss << "Нет газа или пламени" << std::endl;
				if(ot_boiler_state.faultFlags.bit.AirPress_fault)	ss << "Давление воздуха" << std::endl;
				if(ot_boiler_state.faultFlags.bit.WaterOverTemp)	ss << "Перегрев теплоносителя" << std::endl;
				ss << "OEM код ошибки: " << ot_boiler_state.OEMfaultCode << std::endl;
				ss << "Диагностический код: " << ot_boiler_state.diagCode << std::endl;

				sendNotification(ss.str());
			}
		}
		else
		{
			//Ошибки котла нет
			if(!is_first_fault)
			{
				read_faultCode();
				// read_diagCode();
				sendNotification("Ошибка сброшена");
			}
			is_first_fault	= true;
		}
	}
}

void	OT_Boiler::read_faultCode()
{
	//Запрос кода ошибки
	OT_Response	faultCode	= processOT(Command::read, 5, 0);
	if(faultCode.status	== OT_Status::sucsess)
	{
		uint8_t	OEMfaultCode	= faultCode.data & 0xff;
		uint8_t	faultFlags		= (faultCode.data >> 8);

		if(OEMfaultCode != ot_boiler_state.OEMfaultCode)
		{
			ot_boiler_state.OEMfaultCode	= OEMfaultCode;
			char	value[16];
			sprintf(value, "%d", OEMfaultCode);
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "OEMfaultCode").c_str(), value, 0, 0, 0);
		}

		if(faultFlags != ot_boiler_state.faultFlags.all)
		{
			ot_boiler_state.faultFlags.all	= faultFlags;
			char	value[16];
			sprintf(value, "%d", faultFlags);
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "faultFlags").c_str(), value, 0, 0, 0);
		}
	}
	else
		repeat(RepeatType::read_faultCode);
}

void	OT_Boiler::read_diagCode()
{
	//Дополнительный диагностический код
	OT_Response	diagCode	= processOT(Command::read, 115, 0);
	if(diagCode.status	== OT_Status::sucsess)
	{
		if(diagCode.data != ot_boiler_state.diagCode)
		{
			ot_boiler_state.diagCode	= diagCode.data;
			char	value[16];
			sprintf(value, "%d", diagCode.data);
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "diagCode").c_str(), value, 0, 0, 0);
		}
	}
	else
		repeat(RepeatType::read_diagCode);
}

void	OT_Boiler::read_ch_temp()
{
	//Опрос температуры теплоносителя
	OT_Response	resp	= processOT(Command::read, 25, 0);
	if(resp.status == OT_Status::sucsess)
	{
		float	ch_temp	= resp.get_float();
		if(ch_temp != ot_boiler_state.ch_temp)
		{
			ot_boiler_state.ch_temp	= ch_temp;
			if(mqtt_client)
			{
				char	value[16];
				sprintf(value, "%.1f", ch_temp);
				esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_temp").c_str(), value, 0, 0, 0);
			}
		}
	}
}

void	OT_Boiler::read_dhw_temp()
{
	//Опрос температуры теплоносителя
	OT_Response	resp	= processOT(Command::read, 26, 0);
	if(resp.status == OT_Status::sucsess)
	{
		float	dhw_temp	= resp.get_float();
		if(dhw_temp != ot_boiler_state.dhw_temp)
		{
			ot_boiler_state.dhw_temp	= dhw_temp;
			if(mqtt_client)
			{
				char	value[16];
				sprintf(value, "%.1f", dhw_temp);
				esp_mqtt_client_publish(mqtt_client, (boiler_topic + "dhw_temp").c_str(), value, 0, 0, 0);
			}
		}
	}
}

float	OT_Boiler::read_modulation()
{
	//Опрос модуляции горелки
	OT_Response	resp	= processOT(Command::read, 17, 0);
	if(resp.status == OT_Status::sucsess)
	{
		float	modulation	= resp.get_float();
		if(modulation != ot_boiler_state.modulation)
		{
			ot_boiler_state.modulation	= modulation;
			if(mqtt_client)
			{
				char	value[16];
				sprintf(value, "%.2f", modulation);
				esp_mqtt_client_publish(mqtt_client, (boiler_topic + "modulation").c_str(), value, 0, 0, 0);
			}
		}

		return modulation;
	}

	return 0;
}

float	OT_Boiler::read_flame_current()
{
	//Опрос тока ионизации
	OT_Response	resp	= processOT(Command::read, 36, 0);
	if(resp.status == OT_Status::sucsess)
	{
		float	flame_current	= resp.get_float();
		if(flame_current != ot_boiler_state.modulation)
		{
			ot_boiler_state.flame_current	= flame_current;
			if(mqtt_client)
			{
				char	value[16];
				sprintf(value, "%.2f", flame_current);
				esp_mqtt_client_publish(mqtt_client, (boiler_topic + "flame_current").c_str(), value, 0, 0, 0);
			}
		}

		return flame_current;
	}

	return 0;
}

void	OT_Boiler::set_ch_temp_zad(float ch_temp_zad, bool data_invalid_expected /* = false */)
{
	//Установка заданной температуры теплоносителя
	if(ch_temp_zad < 0.)	ch_temp_zad	= 0;
	if(ch_temp_zad > 100.)	ch_temp_zad	= 100.;
	ot_boiler_data.ch_temp_zad	= ch_temp_zad;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "ch_temp_zad", ot_boiler_data.ch_temp_zad);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	//Выполнение запроса
	OT_Response	resp	= processOT(Command::write, 1, uint16_t(ot_boiler_data.ch_temp_zad*256.f), data_invalid_expected);
	if(resp.status == OT_Status::sucsess)
	{
		if(mqtt_client)
		{
			char	value[16];
			sprintf(value, "%.0f", ch_temp_zad);
			esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_temp_zad").c_str(), value, 0, 0, 0);
		}
	}
	else
		repeat(RepeatType::set_ch_temp_zad);
}

void	OT_Boiler::set_dhw_temp_zad(float dhw_temp_zad, bool data_invalid_expected /* = false */)
{
	//Установка заданной температуры теплоносителя
	if(dhw_temp_zad < 0.)	dhw_temp_zad	= 0;
	if(dhw_temp_zad > 100.)	dhw_temp_zad	= 100.;
	ot_boiler_data.dhw_temp_zad	= dhw_temp_zad;
	ot_boiler_data.DHW			= dhw_temp_zad > 0;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "dhw_temp_zad", ot_boiler_data.dhw_temp_zad);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	//Выполнение запроса
	OT_Response	resp	= processOT(Command::write, 56, uint16_t(ot_boiler_data.dhw_temp_zad*256.f), data_invalid_expected);
	if(resp.status == OT_Status::sucsess)
	{
		if(mqtt_client)
		{
			char	value[16];
			sprintf(value, "%.0f", dhw_temp_zad);
			esp_mqtt_client_publish(mqtt_client, (boiler_topic + "dhw_temp_zad").c_str(), value, 0, 0, 0);
		}
	}
	else
		repeat(RepeatType::set_dhw_temp_zad);
}

void	OT_Boiler::set_ch_temp_max(float ch_temp_max)
{
	//Установка максимальной температуры теплоносителя
	if(ch_temp_max < 0.)	ch_temp_max	= 0;
	if(ch_temp_max > 127.)	ch_temp_max	= 127.;
	ot_boiler_data.ch_temp_max	= ch_temp_max;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "ch_temp_max", ot_boiler_data.ch_temp_max);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	//Выполнение запроса
	OT_Response	resp	= processOT(Command::write, 57, uint16_t(ot_boiler_data.ch_temp_max*256.f));
	if(resp.status == OT_Status::sucsess)
	{
		if(mqtt_client)
		{
			char	value[16];
			sprintf(value, "%.0f", ch_temp_max);
			esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_temp_max").c_str(), value, 0, 0, 0);
		}
	}
	else
		repeat(RepeatType::set_ch_temp_max);
}

void	OT_Boiler::set_ch_mod_max(float ch_mod_max, bool data_invalid_expected /* = false */)
{
	//Установка максимальной разрешенной модуляции
	if(ch_mod_max < 0.)		ch_mod_max	= 0;
	if(ch_mod_max > 100.)	ch_mod_max	= 100.;
	ot_boiler_data.ch_mod_max	= ch_mod_max;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "ch_mod_max", ot_boiler_data.ch_mod_max);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	//Выполнение запроса
	OT_Response	resp	= processOT(Command::write, 14, uint16_t(ot_boiler_data.ch_mod_max*256.f), data_invalid_expected);
	if(resp.status == OT_Status::sucsess)
	{
		if(mqtt_client)
		{
			char	value[16];
			sprintf(value, "%.0f", ch_mod_max);
			esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_mod_max").c_str(), value, 0, 0, 0);
		}
	}
	else
		repeat(RepeatType::set_ch_mod_max);
}

bool	OT_Boiler::BLOR()
{
	//Boiler Lock-out  Reset
	OT_Response	resp	= processOT(Command::write, 4, 1);	//Boiler Lock-out Reset request
	resp	= processOT(Command::write, 4, 10);				//Request to reset service request flag
	resp	= processOT(Command::write, 4, 0);				//Back to Normal oparation mode
	if(resp.status == OT_Status::sucsess)
	{
		if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (boiler_topic + "BLOR").c_str(), (resp.data > 128 ? "done" : "failed"), 0, 0, 0);
		return resp.data > 128;
	}
	else
		repeat(RepeatType::BLOR);

	return false;
}

void	OT_Boiler::set_CH(bool CH)
{
	ot_boiler_data.CH	= CH;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "CH", ot_boiler_data.CH);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	if(mqtt_client)
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "control/CH").c_str(), ot_boiler_data.CH ? "1" : "0", 0, 0, 0);

	//Установка вместе с модуляцией
	read_status();
	// set_ch_mod_max(ot_boiler_data.ch_mod_max);
}

void	OT_Boiler::set_DHW(bool DHW)
{
	ot_boiler_data.DHW	= DHW;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "DHW", ot_boiler_data.DHW);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	if(mqtt_client)
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "control/DHW").c_str(), ot_boiler_data.DHW ? "1" : "0", 0, 0, 0);

	read_status();
}

void	OT_Boiler::set_SummerMode(bool SummerMode)
{
	ot_boiler_data.SummerMode	= SummerMode;

	//Запоминание
	nvs_handle_t	nvs_settings;
	if(nvs_open("boiler", NVS_READWRITE, &nvs_settings) == ESP_OK){
		nvs_set_u8(nvs_settings, "SummerMode", ot_boiler_data.SummerMode);
		nvs_commit(nvs_settings);
		nvs_close(nvs_settings);
	}

	if(mqtt_client)
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "control/SummerMode").c_str(), ot_boiler_data.SummerMode ? "1" : "0", 0, 0, 0);

	read_status();
}

void	OT_Boiler::sendNotification(const std::string& text)
{
	toTelegram*	send	= new toTelegram;
	send->chat_id		= SecureConfig::telegram_user_id;
	send->reply_id		= 0;
	send->text			= text;
	xQueueGenericSend(to_telegram_queue, &send, 10, queueSEND_TO_BACK);
}

void	OT_Boiler::print_status(std::ostringstream& ss) const
{
	if(ot_boiler_state.fault)
	{
		ss << "*ОШИБКА* " << std::endl;
		if(ot_boiler_state.faultFlags.bit.ServiceRequest)	ss << "ServiceRequest" << std::endl;
		if(ot_boiler_state.faultFlags.bit.LockoutReset)		ss << "LockoutReset" << std::endl;
		if(ot_boiler_state.faultFlags.bit.LowWaterPress)	ss << "Мало давление теплоносителя" << std::endl;
		if(ot_boiler_state.faultFlags.bit.GasFlame_fault)	ss << "Нет газа или пламени" << std::endl;
		if(ot_boiler_state.faultFlags.bit.AirPress_fault)	ss << "Давление воздуха" << std::endl;
		if(ot_boiler_state.faultFlags.bit.WaterOverTemp)	ss << "Перегрев теплоносителя" << std::endl;
		ss << "OEM код ошибки: " << ot_boiler_state.OEMfaultCode << std::endl;
		ss << "Диагностический код: " << ot_boiler_state.diagCode << std::endl;
	}
	ss << "*Отопление* " << (ot_boiler_state.centralHeating ? "вкл" : "откл") << std::endl;
	ss << "*ГВС* " << (ot_boiler_state.dhw ? "вкл" : "откл") << std::endl;
	if(ot_boiler_state.flame)
		ss << "*Горелка* " << "🔥" << ot_boiler_state.modulation << "%" << std::endl;
	else
		ss << "*Горелка* откл" << std::endl;
	ss << "*Теплоноситель*  " << ot_boiler_state.ch_temp << " ℃" << std::endl;
	ss << "*Заданная*  " << ot_boiler_data.ch_temp_zad << " ℃" << std::endl;

	ss << "*failsCounter*" << std::endl;
	ss << "```" << std::endl;
	ss << "notInited = " << failsCounter.notInited << std::endl;
	ss << "timeout = " << failsCounter.timeout << std::endl;
	ss << "rx_invalid = " << failsCounter.rx_invalid << std::endl;
	ss << "parityFail = " << failsCounter.parityFail << std::endl;
	ss << "unknownID = " << failsCounter.unknownID << std::endl;
	ss << "dataInvalid = " << failsCounter.dataInvalid << std::endl;
	ss << "ACK_fail = " << failsCounter.ACK_fail << std::endl;
	ss << "msgType_unknown = " << failsCounter.msgType_unknown << std::endl;
	ss << "SPARE_fail = " << failsCounter.SPARE_fail << std::endl;
	ss << "responseID_fail = " << failsCounter.responseID_fail << std::endl;
	ss << "```" << std::endl;
}

json	OT_Boiler::json_status() const
{
	return json{
		{"centralHeating", ot_boiler_state.centralHeating},
		{"dhw",  ot_boiler_state.dhw},
		{"flame",  ot_boiler_state.flame},
		{"flame_current",  ot_boiler_state.flame_current},
		{"fault",  ot_boiler_state.fault},
		{"faultFlags",  ot_boiler_state.faultFlags.all},
		{"faultCode",  ot_boiler_state.OEMfaultCode},
		{"diagCode",  ot_boiler_state.diagCode},
		{"ch_temp",  ot_boiler_state.ch_temp},
		{"dhw_temp",  ot_boiler_state.dhw_temp},
		{"dhw_temp_zad", ot_boiler_data.dhw_temp_zad},
		{"modulation",  ot_boiler_state.modulation},
		{"ch_temp_zad", ot_boiler_data.ch_temp_zad},
		{"ch_temp_max", ot_boiler_data.ch_temp_max},
		{"ch_mod_max", ot_boiler_data.ch_mod_max},
		{"failsCounter", {
			{"notInited", failsCounter.notInited},
			{"timeout", failsCounter.timeout},
			{"rx_invalid", failsCounter.rx_invalid},
			{"parityFail", failsCounter.parityFail},
			{"unknownID", failsCounter.unknownID},
			{"dataInvalid", failsCounter.dataInvalid},
			{"ACK_fail", failsCounter.ACK_fail},
			{"msgType_unknown", failsCounter.msgType_unknown},
			{"SPARE_fail", failsCounter.SPARE_fail},
			{"responseID_fail", failsCounter.responseID_fail},
			{"uptime", uint64_t(esp_timer_get_time()*0.000001)}
		}}
	};
}

void	OT_Boiler::log_head(std::ostringstream& ss) const
{
	ss << "CH; DHW; flame; ch_temp; dhw_temp; modulation; ch_temp_zad; dhw_temp_zad";
}

void	OT_Boiler::log_data(std::ostringstream& ss) const
{
	ss << (ot_boiler_state.centralHeating ? "1" : "0") << "; ";
	ss << (ot_boiler_state.dhw ? "1" : "0") << "; ";
	ss << (ot_boiler_state.flame ? "1" : "0") << "; ";
	ss << ot_boiler_state.ch_temp << "; ";
	ss << ot_boiler_state.dhw_temp << "; ";
	ss << ot_boiler_state.modulation << "; ";
	ss << ot_boiler_data.ch_temp_zad << "; ";
	ss << ot_boiler_data.dhw_temp_zad << "; ";
}

bool	OT_Boiler::openTherm_is_correct() const
{
	return error_counter < 5;
}

json	OT_Boiler::set_boiler_data(const json& j)
{
	json	res;
	if(j.contains("centralHeating") && j.at("centralHeating").is_boolean())
	{
		set_CH(j.at("centralHeating").get<bool>());
		res["CH"]	= "ok";
	}

	if(j.contains("dhw") && j.at("dhw").is_boolean())
	{
		set_DHW(j.at("dhw").get<bool>());
		res["dhw"]	= "ok";
	}

	if(j.contains("SummerMode") && j.at("SummerMode").is_boolean())
	{
		set_SummerMode(j.at("SummerMode").get<bool>());
		res["SummerMode"]	= "ok";
	}

	if(j.contains("ch_temp_zad") && j.at("ch_temp_zad").is_number_integer())
	{
		set_ch_temp_zad(j.at("ch_temp_zad").get<int>());
		res["ch_temp_zad"]	= "ok";
	}

	if(j.contains("dhw_temp_zad") && j.at("dhw_temp_zad").is_number_integer())
	{
		set_dhw_temp_zad(j.at("dhw_temp_zad").get<int>());
		res["dhw_temp_zad"]	= "ok";
	}

	if(j.contains("ch_temp_max") && j.at("ch_temp_max").is_number_integer())
	{
		set_ch_temp_max(j.at("ch_temp_max").get<int>());
		res["ch_temp_max"]	= "ok";
	}

	if(j.contains("ch_mod_max") && j.at("ch_mod_max").is_number_integer())
	{
		// set_ch_mod_max(j.at("ch_mod_max").get<int>());
		res["ch_mod_max"]	= "ok";
	}

	return res;
}

void	OT_Boiler::send_all_mqtt()
{
	if(mqtt_client)
	{
		char	value[16];
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "fault").c_str(), ot_boiler_state.fault ? "1" : "0", 0, 0, 0);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "centralHeating").c_str(), ot_boiler_state.centralHeating ? "1" : "0", 0, 0, 0);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "dhw").c_str(), ot_boiler_state.dhw ? "1" : "0", 0, 0, 0);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "flame").c_str(), ot_boiler_state.flame ? "1" : "0", 0, 0, 0);

		sprintf(value, "%d", int(ot_boiler_state.OEMfaultCode));
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "OEMfaultCode").c_str(), value, 0, 0, 0);
		sprintf(value, "%d", ot_boiler_state.faultFlags.all);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "faultFlags").c_str(), value, 0, 0, 0);
		sprintf(value, "%d", int(ot_boiler_state.diagCode));
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "diagCode").c_str(), value, 0, 0, 0);
		sprintf(value, "%.0f", ot_boiler_state.ch_temp);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_temp").c_str(), value, 0, 0, 0);
		sprintf(value, "%.0f", ot_boiler_state.dhw_temp);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "dhw_temp").c_str(), value, 0, 0, 0);
		sprintf(value, "%.2f", ot_boiler_state.modulation);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "modulation").c_str(), value, 0, 0, 0);
		sprintf(value, "%.2f", ot_boiler_state.flame_current);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "flame_current").c_str(), value, 0, 0, 0);
		sprintf(value, "%.0f", ot_boiler_data.ch_temp_zad);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_temp_zad").c_str(), value, 0, 0, 0);
		sprintf(value, "%.0f", ot_boiler_data.dhw_temp_zad);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "dhw_temp_zad").c_str(), value, 0, 0, 0);
		sprintf(value, "%.0f", ot_boiler_data.ch_temp_max);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_temp_max").c_str(), value, 0, 0, 0);
		sprintf(value, "%.0f", ot_boiler_data.ch_mod_max);
		esp_mqtt_client_publish(mqtt_client, (boiler_topic + "ch_mod_max").c_str(), value, 0, 0, 0);
	}
}

json	OT_Boiler::test_ot_command(json ot_data)
{
	json	res;
	if(!ot_data.contains("cmd") || 	//Проверка наличия всех полей
		!ot_data.contains("id") ||
		!ot_data.contains("data"))						res	= {{"fail", "ot_data incorrect"}};
	else if(!ot_data.at("cmd").is_string() || //Проверка типов всех полей
			!ot_data.at("id").is_number_integer() ||
			!ot_data.at("data").is_number_integer())	res	= {{"fail", "ot_data incorrect"}};
	else {
		//Все параметры в норме
		std::string	str_cmd	= ot_data.at("cmd").get<std::string>();
		uint8_t		id		= ot_data.at("id").get<int>();
		uint16_t	data	= ot_data.at("data").get<int>();

		Command		cmd	= Command::read;
		if(str_cmd == "write") 		cmd	= Command::write;
		if(str_cmd == "invalid")	cmd	= Command::invalid;

		OT_Response	out	= processOT(cmd, id, data);

		OT_Message_t	response;
		response.all	= out.response;
		res	= {
			{"status", OT_Status_to_string(out.status)},
			{"data", out.data},
			{"response", {
				{"all", response.all},
				{"parity", uint8_t(response.bit.parity)},
				{"msg_type", uint8_t(response.bit.msg_type)},
				{"spare", uint8_t(response.bit.spare)},
				{"id", uint8_t(response.bit.id)},
				{"data", uint16_t(response.bit.data)}
			}}
		};
	}

	return res;
}

std::string	OT_Boiler::OT_Status_to_string(const OT_Boiler::OT_Status& status)
{
	switch(status)
	{
		case OT_Status::sucsess:			return "sucsess";
		case OT_Status::notInited:			return "notInited";
		case OT_Status::timeout:			return "timeout";
		case OT_Status::rx_invalid:			return "rx_invalid";
		case OT_Status::parityFail:			return "parityFail";
		case OT_Status::unknownID:			return "unknownID";
		case OT_Status::dataInvalid:		return "dataInvalid";
		case OT_Status::ACK_fail:			return "ACK_fail";
		case OT_Status::msgType_unknown:	return "msgType_unknown";
		case OT_Status::SPARE_fail:			return "SPARE_fail";
		case OT_Status::responseID_fail:	return "responseID_fail";
		default:							return "wrong_status";
	}
}

bool	OT_Boiler::is_CH_on()
{
	return ot_boiler_data.CH;
}
