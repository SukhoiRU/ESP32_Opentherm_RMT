#include <cmath>
#include <vector>
#include "json.hpp"
using json = nlohmann::json;

#include "room_thermostat.h"
#include "thermo.h"
#include "mqtt.h"

RoomThermostat::RoomThermostat(uint8_t temp_index, uint8_t rad_index, const std::string& str, const std::string& topic_name)
{
	room_temp_index	= temp_index;
	room_rad_index	= rad_index;
	name			= str;
	topic			= topic_name;
}

void	RoomThermostat::setParams(const float& temp, const float& room_mod_max, const json& j)
{
	temp_zad	= temp;
	mod_max		= room_mod_max;

	if(j.contains("T_f") && j.at("T_f").is_number()){
		params.T_f	= j.at("T_f").get<float>();
	}
	if(j.contains("Kt") && j.at("Kt").is_number()){
		params.Kt	= j.at("Kt").get<float>();
	}
	if(j.contains("Kdt") && j.at("Kdt").is_number()){
		params.Kdt	= j.at("Kdt").get<float>();
	}
	if(j.contains("K_mod") && j.at("K_mod").is_number()){
		params.K_mod	= j.at("K_mod").get<float>();
	}

	//Установка интегралов
	temp_f	= thermometers.at(room_temp_index).value;
	temp_I	= thermometers.at(room_rad_index).value;

	if(j.contains("temp_f") && j.at("temp_f").is_number()){
		temp_f	= j.at("temp_f").get<float>();
	}
	if(j.contains("temp_I") && j.at("temp_I").is_number()){
		temp_I	= j.at("temp_I").get<float>();
	}
}

json	RoomThermostat::getPID_params()
{
	json	j	= {
		{"T_f", params.T_f},
		{"Kt", params.Kt},
		{"Kdt", params.Kdt},
		{"K_mod", params.K_mod}
	};

	return j;
}

void	RoomThermostat::Life(const float timeStep)
{
	float	temp		= thermometers.at(room_temp_index).value;
	float	radiator	= thermometers.at(room_rad_index).value;

	//Фильтрация температуры
	float	dt	= 0.05*(radiator - temp_bal)/3600.;
	temp_f	+= (temp + params.T_f*dt - temp_f)/params.T_f*timeStep;

	//Подстройка балансировки
	temp_bal	+= (-0.01*(temp - temp_f))*timeStep;
	if(temp_bal < 15)		temp_bal	= 15;
	else if(temp_bal > 45)	temp_bal	= 45;

	temp_I	+= (0.001*(temp_bal - radiator))*timeStep;

	out.dt	= 3600.*dt;
	float	ch_temp_zad	= temp_I + params.Kt*(temp_zad - temp_f) + params.Kdt*3600.*dt;

	//Уменьшение модуляции при уменьшении заданной температуры ниже 30°
	float	modulation	= mod_max - (30. - ch_temp_zad)*params.K_mod;
	if(modulation < 0)				modulation	= 0;
	else if(modulation > mod_max)	modulation	= mod_max;
	out.mod_max		= modulation;

	out.ch_temp_zad	= ch_temp_zad;
	if(out.ch_temp_zad < 30)	out.ch_temp_zad	= 30;

	//Отладочные переменные
	if(mqtt_client){
		char	buf[256];
		sprintf(buf, "%g", temp_f);
		esp_mqtt_client_publish(mqtt_client, (topic + "temp_f").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", temp_bal);
		esp_mqtt_client_publish(mqtt_client, (topic + "temp_bal").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", dt*3600);
		esp_mqtt_client_publish(mqtt_client, (topic + "dt").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", ch_temp_zad);
		esp_mqtt_client_publish(mqtt_client, (topic + "ch_temp_zad").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", temp_I);
		esp_mqtt_client_publish(mqtt_client, (topic + "temp_I").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", modulation);
		esp_mqtt_client_publish(mqtt_client, (topic + "modulation").c_str(), buf, 0, 0, 0);
	}
}