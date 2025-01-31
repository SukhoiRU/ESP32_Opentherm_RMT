#ifndef RMT_OPENTHERM_H
#define RMT_OPENTHERM_H

class RMT_Opentherm
{
private:
	std::string				log_topic;
	rmt_channel_handle_t	rx_channel	= nullptr;
	rmt_channel_handle_t	tx_channel	= nullptr;
	rmt_encoder_handle_t	tx_encoder	= nullptr;
	rmt_receive_config_t	receive_config;
	rmt_transmit_config_t 	transmit_config;
	rmt_symbol_word_t		rx_symbols_buf[128];
	QueueHandle_t			receive_queue;
	int64_t					time_last_receive;	//Момент завершения приёма, после которого надо выждать не менее 100 мс до следующей отправки

public:
	explicit RMT_Opentherm(const gpio_num_t pin_in, const gpio_num_t pin_out, const std::string& topic);

	enum class Result: uint8_t{sucsess, receive_timeout, receive_invalid_state, receive_invalid_arg, receive_fail, fail};
	Result	processOT(const uint32_t request, uint32_t* response);
};

#endif	//RMT_OPENTHERM_H
