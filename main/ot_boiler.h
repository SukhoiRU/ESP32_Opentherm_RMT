#ifndef OT_BOILER_H
#define OT_BOILER_H

#include <string>
#include <queue>
class RMT_Opentherm;

class OT_Boiler
{
private:
	RMT_Opentherm*	rmt_ot	= nullptr;

	//Формат сообщения OpenTherm
	union OT_Message_t
	{
		struct {
			uint16_t	data		:16;	//Data value
			uint8_t		id			:8;		//Data ID
			uint8_t		spare		:4;		//SPARE
			uint8_t		msg_type	:3;		//Message type
			uint8_t		parity		:1;		//Бит четности
		}bit;
		uint32_t	all	= 0;
	};

	enum class MsgType: uint8_t{
		//Master to slave
		READ_DATA 		= 0b000,
		WRITE_DATA 		= 0b001,
		INVALID_DATA	= 0b010,
		reserved		= 0b011,

		//Slave to master
		READ_ACK		= 0b100,
		WRITE_ACK		= 0b101,
		DATA_INVALID	= 0b110,
		UNKNOWN_DATAID	= 0b111
	};

	//Состояние котла
	struct ot_boiler_state_t
	{
		bool	centralHeating;	//Работа отопления
		bool	dhw;			//Работа нагревателя горячей воды
		bool	flame;			//Работа горелки
		bool	fault;			//Признак отказа

		union FaultFlags_t{
			struct{
				int	ServiceRequest:1;
				int	LockoutReset:1;
				int	LowWaterPress:1;
				int	GasFlame_fault:1;
				int	AirPress_fault:1;
				int	WaterOverTemp:1;
			}bit;
			uint8_t	all;
		};
		FaultFlags_t	faultFlags;		//Флаги ошибки
		uint8_t		OEMfaultCode = 0;	//Код ошибки
		uint32_t	diagCode	= 0;	//OEM диагностический код
		float		ch_temp;			//Температура отопления
		float		pressure;			//Давление теплоносителя
		float		modulation;			//Модуляция горелки, %
	}ot_boiler_state;

	//Заданные параметры котла
	struct ot_boiler_data_t
	{
		bool	CH			= true;
		bool	DHW			= false;
		bool	SummerMode	= false;
		float	ch_temp_zad	= 40;		//Заданная температура теплоносителя
		float	ch_temp_max	= 82;		//Максимальная температура теплоносителя
		float	ch_mod_max	= 100;		//Максимально допустимая модуляция горелки
		float	room_temp	= 20;		//Текущая температура в комнате
		float	room_temp_zad	= 22;	//Заданная температура в комнате
	}ot_boiler_data;

	//Необходимость оповещения
	bool	is_first_fault	= true;
	int		error_counter	= 0;
	float	sended_pressure	= 0;		//Последнее отправленное в MQTT значение

	std::string	boiler_topic;
	std::string	boiler_OT_topic;
	int		slaveID	= 4;				//Код для перевода котла в slave

	struct FailsCounter
	{
		uint32_t	notInited		= 0;
		uint32_t	timeout			= 0;
		uint32_t	rx_invalid		= 0;
		uint32_t	parityFail		= 0;
		uint32_t	unknownID		= 0;
		uint32_t	dataInvalid		= 0;
		uint32_t	ACK_fail		= 0;
		uint32_t	msgType_unknown	= 0;
		uint32_t	SPARE_fail		= 0;
		uint32_t	responseID_fail	= 0;
	}failsCounter;

	bool	check_parity(uint32_t	word);
	void	sendNotification(const std::string& text);

	//Основная функция обмена с котлом
	enum class OT_Status: uint8_t{sucsess, notInited, timeout, rx_invalid, parityFail, unknownID, dataInvalid, ACK_fail, msgType_unknown, SPARE_fail, responseID_fail};
	struct	OT_Response
	{
		OT_Status	status;
		uint16_t	data;
		uint32_t	response;

		float	get_float()	{return (data & 32768) ? -(65536 - data)/256.f : data/256.f;}
	};
	enum class Command: uint8_t {read, write, invalid};

	OT_Response	processOT(const Command cmd, const uint8_t id, const uint16_t data, bool invalid_data_expected = false);

	std::string	OT_Status_to_string(const OT_Status& status);

	//Очередь сообщений, которые необходимо повторить
	enum class RepeatType: uint8_t{set_slave, read_slaveConfig, read_status, read_faultCode, read_diagCode, read_ch_temp, read_pressure, read_modulation,
		set_ch_temp_zad, set_ch_temp_max, set_ch_mod_max, BLOR, set_room_temperature, set_room_temp_zad};
	struct RepeatQueue_t
	{
		RepeatType	msg;
		uint8_t		counter;
	};
	std::queue<RepeatQueue_t>	repeat_queue;

	void	repeat(RepeatType	msg);

public:
	OT_Boiler(const gpio_num_t pin_in, const gpio_num_t pin_out, const std::string& topic, const std::string& OT_topic, const int slaveID);

	size_t	repeat_old_messages();	//Возвращает остаток очереди
	void	clear_old_message();

	//Функции обмена конкретными параметрами
	void	set_slave(int slaveID);
	void	read_slaveConfig();
	void	read_status();
	void	read_faultCode();
	void	read_diagCode();
	void	read_ch_temp();
	void	read_pressure();
	void	read_modulation();
	void	set_ch_temp_zad(float ch_temp_zad, bool data_invalid_expected = false);
	void	set_ch_temp_max(float ch_temp_max);
	void	set_ch_mod_max(float ch_mod_max, bool data_invalid_expected = false);
	void	set_CH(bool CH);
	void	set_DHW(bool DHW);
	void	set_SummerMode(bool SummerMode);
	bool	BLOR();
	void	set_room_temperature(float temp, bool data_invalid_expected = false);
	void	set_room_temp_zad(float temp_zad);

	//Функция тестирования обмена
	json	test_ot_command(json params);

	//Константный доступ из других задач
	void	print_status(std::ostringstream& ss) const;
	json	json_status() const;

	void	log_head(std::ostringstream& ss) const;
	void	log_data(std::ostringstream& ss) const;

	json	set_boiler_data(const json& j);
	bool	openTherm_is_correct() const;

	void	send_all_mqtt();
	bool	is_CH_on();
};

#endif	//OT_BOILER_H
