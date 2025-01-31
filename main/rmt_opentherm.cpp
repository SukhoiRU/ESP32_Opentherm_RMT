#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "driver/rmt_tx.h"
#include "driver/rmt_rx.h"
#include "esp_log.h"
#include "esp_timer.h"
#include <sstream>
#include <vector>
#include "mqtt.h"
#include "rmt_opentherm.h"

static const char*	TAG = "rmt_opentherm";
static bool rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_data);
static size_t encoder_callback(const void* data, size_t data_size, size_t symbols_written, size_t symbols_free, rmt_symbol_word_t* symbols, bool* done, void* arg);

RMT_Opentherm::RMT_Opentherm(const gpio_num_t pin_in, const gpio_num_t pin_out, const std::string& topic)
{
	log_topic	= topic;

	//Настройка канала приема
	ESP_LOGI(TAG, "create RMT RX channel");
	rmt_rx_channel_config_t	rx_channel_cfg = {
		.gpio_num			= pin_in,
		.clk_src			= RMT_CLK_SRC_DEFAULT,
		.resolution_hz		= 1000000,
		.mem_block_symbols	= 128, // amount of RMT symbols that the channel can store at a time
		.intr_priority		= 0,
		.flags	= {
			.invert_in		= false,
			.with_dma		= false,
			.io_loop_back	= false
		}
	};

	receive_config = {
		.signal_range_min_ns	= 200,				//200 мкс
		.signal_range_max_ns	= 2000*1000,		//2 мс - два периода передачи бита
		.flags	= {
			.en_partial_rx	= false
		}
	};

	ESP_ERROR_CHECK(rmt_new_rx_channel(&rx_channel_cfg, &rx_channel));

	//Создание очереди ожидания приема
	ESP_LOGI(TAG, "register RX done callback");
	receive_queue	= xQueueCreate(1, sizeof(rmt_rx_done_event_data_t));
	assert(receive_queue);
	rmt_rx_event_callbacks_t cbs = {
		.on_recv_done = rmt_rx_done_callback,
	};
	ESP_ERROR_CHECK(rmt_rx_register_event_callbacks(rx_channel, &cbs, receive_queue));

	//Настройка канала передачи
	ESP_LOGI(TAG, "create RMT TX channel");
	rmt_tx_channel_config_t	tx_channel_cfg = {
		.gpio_num			= pin_out,
		.clk_src			= RMT_CLK_SRC_DEFAULT,
		.resolution_hz		= 1000000,
		.mem_block_symbols	= 64, // amount of RMT symbols that the channel can store at a time
		.trans_queue_depth	= 4,  // number of transactions that allowed to pending in the background, this example won't queue multiple transactions, so queue depth > 1 is sufficient
		.intr_priority		= 0,
		.flags	= {
			.invert_out		= false,
			.with_dma		= false,
			.io_loop_back	= false,
			.io_od_mode		= false
		}
	};

	transmit_config = {
		.loop_count	= 0,
		.flags	= {
			.eot_level			= 1,
			.queue_nonblocking	= 1
		}
	};

	ESP_ERROR_CHECK(rmt_new_tx_channel(&tx_channel_cfg, &tx_channel));

	//Энкодер для преобразования uint32_t в формат RMT
	ESP_LOGI(TAG, "Create simple callback-based encoder");
	const rmt_simple_encoder_config_t	simple_encoder_cfg = {
		.callback 		= encoder_callback,
		.arg			= nullptr,
		.min_chunk_size	= 64
	};
	ESP_ERROR_CHECK(rmt_new_simple_encoder(&simple_encoder_cfg, &tx_encoder));

	//Включение канала передачи
	ESP_LOGI(TAG, "enable RMT TX and RX channels");
	ESP_ERROR_CHECK(rmt_enable(tx_channel));

	//Выставка высокого уровня на линии путем отправки одного импульса
	rmt_copy_encoder_config_t	copy_config	= {};
	rmt_encoder_handle_t		copy_encoder;
	rmt_new_copy_encoder(&copy_config, &copy_encoder);
	rmt_symbol_word_t pullup_symbol = {.duration0 = 1, .level0 = 1, .duration1 = 0, .level1 = 1};
	rmt_transmit(tx_channel, copy_encoder, &pullup_symbol, sizeof(pullup_symbol), &transmit_config);
	rmt_del_encoder(copy_encoder);

	//Нужно секунду подождать, чтобы первый обмен не завершился ошибкой
	//vTaskDelay будет выполнен в processOT, поэтому сдвиг на 900 мс
	time_last_receive	= esp_timer_get_time() + 900000;
}

RMT_Opentherm::Result	RMT_Opentherm::processOT(const uint32_t request, uint32_t* response)
{
	Result	out;

	//Ожидание не менее 100 мс после последнего приема
	int64_t	dt	= (esp_timer_get_time() - time_last_receive)/1000;
	if(dt < 100)
		vTaskDelay(pdMS_TO_TICKS(100 - dt));

	//Включение приема и передача команды
	rmt_enable(rx_channel);
	esp_err_t	receive_state	= rmt_receive(rx_channel, rx_symbols_buf, sizeof(rx_symbols_buf), &receive_config);
	ESP_ERROR_CHECK(rmt_transmit(tx_channel, tx_encoder, &request, sizeof(request), &transmit_config));

	if(receive_state == ESP_OK)
	{
		//Ожидание сигнала о завершении приема
		rmt_rx_done_event_data_t rx_data;
		if(xQueueReceive(receive_queue, &rx_data, pdMS_TO_TICKS(1500)) == pdPASS)
		{
			ESP_LOGW(TAG, "receive done");
			time_last_receive	= esp_timer_get_time();

			//Проверка, что пришли все 34 бита и все начинаются с 1
			size_t	slots_count	= 0;
			bool	all_bits_starts_from_1	= true;
			for(int i = 0; i < rx_data.num_symbols; i++)
			{
				if(rx_data.received_symbols[i].duration0 < 700)	slots_count++;
				else											slots_count	+= 2;

				if(rx_data.received_symbols[i].duration1 < 700)	slots_count++;
				else											slots_count	+= 2;

				if(rx_data.received_symbols[i].level0 == 0)	all_bits_starts_from_1	= false;
			}

			//Отладочная печать
			std::ostringstream	ss;
			ss << "request: 0x" << std::hex << request << std::dec << std::endl;
			ss << "slots_count: " << slots_count << std::endl;
			ss << "num_symbols: " << rx_data.num_symbols << std::endl;
			for(size_t i = 0; i < rx_data.num_symbols; i++)
			{
				ss << "(" << rx_data.received_symbols[i].level0 << ": " << rx_data.received_symbols[i].duration0 << "; ";
				ss << rx_data.received_symbols[i].level1 << ": " << rx_data.received_symbols[i].duration1 << ")" << std::endl;
			}

			if(mqtt_client && !all_bits_starts_from_1){
				ss << "Обнаружен level0 = 0" << std::endl;
				esp_mqtt_client_publish(mqtt_client, (log_topic + "/debug").c_str(), ss.str().c_str(), 0, 0, 0);
			}

			if(mqtt_client && rx_data.received_symbols[0].duration0 > 700){
				ss << "Обнаружен duration0 > 700" << std::endl;
				esp_mqtt_client_publish(mqtt_client, (log_topic + "/debug").c_str(), ss.str().c_str(), 0, 0, 0);
			}

			if(mqtt_client && rx_data.received_symbols[rx_data.num_symbols-1].duration1 != 0){
				ss << "Обнаружен конец, не равный 0" << std::endl;
				esp_mqtt_client_publish(mqtt_client, (log_topic + "/debug").c_str(), ss.str().c_str(), 0, 0, 0);
			}

			//Разбор ответа
			uint32_t	resp		= 0;
			size_t		bits_count	= 0;
			uint8_t		slot_index	= 1;
			for(int i = 0; i < rx_data.num_symbols; i++)
			{
				const rmt_symbol_word_t&	item	= rx_data.received_symbols[i];

				if(item.duration0 < 700)	slot_index	+= 1;
				else						slot_index	+= 2;

				if(slot_index >= 2)
				{
					bits_count++;
					if(bits_count != 1)	resp	= (resp << 1) | (item.level0? 1 : 0);
					if(bits_count > 32) break;
					slot_index	-= 2;
				}

				if(item.duration1 < 700)	slot_index	+= 1;
				else						slot_index	+= 2;

				if(slot_index >= 2)
				{
					bits_count++;
					if(bits_count != 1)	resp	= (resp << 1) | (item.level1? 1 : 0);
					if(bits_count > 32) break;
					slot_index	-= 2;
				}
			}

			*response	= resp;
			out	= Result::sucsess;
		}
		else
		{
			//Через 1.5 секунды ответ не пришел
			ESP_LOGW(TAG, "timeout");
			out	= Result::receive_timeout;
			if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (log_topic + "/log").c_str(), "timeout", 0, 0, 0);
		}
	}
	else if(receive_state == ESP_ERR_INVALID_STATE){
		if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (log_topic + "/log").c_str(), "receive_invalid_state", 0, 0, 0);
		out	= Result::receive_invalid_state;
	}
	else if(receive_state == ESP_ERR_INVALID_ARG){
		if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (log_topic + "/log").c_str(), "receive_invalid_arg", 0, 0, 0);
		out	= Result::receive_invalid_arg;
	}
	else if(receive_state == ESP_FAIL){
		if(mqtt_client)	esp_mqtt_client_publish(mqtt_client, (log_topic + "/log").c_str(), "receive_fail", 0, 0, 0);
		out	= Result::receive_fail;
	}
	else
		out	= Result::fail;

	rmt_disable(rx_channel);
	return out;
}

static bool rmt_rx_done_callback(rmt_channel_handle_t channel, const rmt_rx_done_event_data_t* edata, void* user_data)
{
	BaseType_t		high_task_wakeup	= pdFALSE;
	QueueHandle_t	receive_queue		= static_cast<QueueHandle_t>(user_data);

	// send the received RMT symbols to the parser task
	xQueueSendFromISR(receive_queue, edata, &high_task_wakeup);
	return high_task_wakeup == pdTRUE;
}

//Кодирование отправки
static const rmt_symbol_word_t	opentherm_bit0	= {
	.duration0	= 500,
	.level0		= 1,
	.duration1	= 500,
	.level1		= 0
};

static const rmt_symbol_word_t	opentherm_bit1	= {
	.duration0	= 500,
	.level0		= 0,
	.duration1	= 500,
	.level1		= 1
};

static size_t encoder_callback(const void* data, size_t data_size, size_t symbols_written, size_t symbols_free, rmt_symbol_word_t* symbols, bool* done, void* arg)
{
	if(symbols_free < 34)
		return 0;

	//Стартовый бит
	symbols[0]	= opentherm_bit1;

	//request
	const uint32_t	request	= *static_cast<const uint32_t*>(data);
	size_t	index	= 1;
	for(uint32_t bitmask = 0x80000000; bitmask != 0; bitmask >>= 1)
	{
		if(request & bitmask)	symbols[index++]	= opentherm_bit1;
		else					symbols[index++]	= opentherm_bit0;
	}

	//Стоповый бит
	symbols[33]	= opentherm_bit1;

	//Завершение передачи
	*done	= true;
	return	34;
}
