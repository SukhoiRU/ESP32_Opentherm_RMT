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

	//Установка параметров
	if(j.contains("Kt") && j.at("Kt").is_number())			params.Kt		= j.at("Kt").get<float>();
	if(j.contains("Kdt") && j.at("Kdt").is_number())		params.Kdt		= j.at("Kdt").get<float>();
	if(j.contains("Ki") && j.at("Ki").is_number())			params.Ki		= j.at("Ki").get<float>();
	if(j.contains("Ki_dt") && j.at("Ki_dt").is_number())	params.Ki_dt	= j.at("Ki_dt").get<float>();
	if(j.contains("K_mod") && j.at("K_mod").is_number())	params.K_mod	= j.at("K_mod").get<float>();

	//Установка интегралов
	temp_f	= thermometers.at(room_temp_index).value;
	if(j.contains("temp_f") && j.at("temp_f").is_number())	temp_f			= j.at("temp_f").get<float>();
	if(j.contains("Idt") && j.at("Idt").is_number())		Idt				= j.at("Idt").get<float>();
}

json	RoomThermostat::getPID_params()
{
	json	j	= {
		{"Kt", params.Kt},
		{"Kdt", params.Kdt},
		{"Ki", params.Ki},
		{"Ki_dt", params.Ki_dt},
		{"K_mod", params.K_mod},
		{"temp_f", temp_f},
		{"Idt", Idt}
	};

	return j;
}

void	RoomThermostat::Life(const float timeStep)
{
	float	room		= thermometers.at(room_temp_index).value;
	float	radiator	= thermometers.at(room_rad_index).value;
	float	outdoor		= thermometers.at(outdoor_index).value;

	//Фильтрация температуры
	float	dt_calc	= 0.03f*(radiator - room) - 0.007f*(room - outdoor);
	float	e_temp	= 0.0005f*(room - temp_f);
	temp_f	+= (dt_calc/3600.f + dt0 + e_temp)*timeStep;
	dt0		+= (0.0001f*e_temp)*timeStep;
	float	dt	= dt_calc + 3600.f*dt0;

	float	Ki;
	if(abs(dt) > 0.3f)			Ki	= 0.0f;
	else if(abs(dt) > 0.1f)		Ki	= 1.0 - (abs(dt) - 0.1f)/0.2f;
	else						Ki	= 1.0f;

	//Управление
	Idt		+= Ki*(params.Ki*(temp_zad - temp_f) - params.Ki_dt*dt)*timeStep;
	if(Idt < 15.f)		Idt	= 15.f;
	else if(Idt > 60.f)	Idt	= 60.f;
	float	ch_temp_zad		= params.Kt*(temp_zad - temp_f) + params.Kdt*dt + Idt;

	//Уменьшение модуляции при уменьшении заданной температуры ниже 30°
	float	mod_zad	= mod_max - (30. - ch_temp_zad)*params.K_mod;
	if(mod_zad < 0)				mod_zad	= 0;
	else if(mod_zad > mod_max)	mod_zad	= mod_max;

	out.dt			= dt;
	out.mod_max		= mod_zad;
	out.ch_temp_zad	= ch_temp_zad;
	if(out.ch_temp_zad < 30.f)		out.ch_temp_zad	= 30.f;
	else if(out.ch_temp_zad > 60.f)	out.ch_temp_zad	= 60.f;

	//Отладочные переменные
	if(mqtt_client){
		char	buf[256];
		sprintf(buf, "%g", temp_f);
		esp_mqtt_client_publish(mqtt_client, (topic + "temp_f").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", dt);
		esp_mqtt_client_publish(mqtt_client, (topic + "dt").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", Idt);
		esp_mqtt_client_publish(mqtt_client, (topic + "Idt").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", out.ch_temp_zad);
		esp_mqtt_client_publish(mqtt_client, (topic + "ch_temp_zad").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", mod_zad);
		esp_mqtt_client_publish(mqtt_client, (topic + "modulation").c_str(), buf, 0, 0, 0);
		sprintf(buf, "%g", e_temp);
		esp_mqtt_client_publish(mqtt_client, (topic + "e_temp").c_str(), buf, 0, 0, 0);
	}
}