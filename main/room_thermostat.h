#ifndef ROOM_THERMOSTAT_H
#define ROOM_THERMOSTAT_H

class RoomThermostat
{
public:
	struct Params_t
	{
		float	Kt		= 30;			//Коэффициент усиления по температуре
		float	Kdt		= 0;			//Коэффициент усиления по скорости температуры
		float	Ki		= 0.002;
		float	Ki_dt	= 0.005;
		float	K_mod	= 40;			//Коэффициент снижения модуляции при температуре ниже 30°
	};

private:
	uint8_t		room_temp_index	= 2;	//Номер датчика температуры в комнате
	uint8_t		room_rad_index	= 5;	//Номер датчика радиатора
	uint8_t		outdoor_index	= 0;	//Номер датчика температуры на улице
	float		mod_max		= 100;		//Разрешенная модуляция
	float		temp_zad	= 15;		//Заданная температура

	float		temp_f		= 15;		//Фильтрованная температура
	float		dt0			= 0;		//Погрешность модели температуры
	float		Idt			= 25;		//Интеграл ошибки

	Params_t	params;
	std::string	topic;

public:
	RoomThermostat(uint8_t temp_index, uint8_t rad_index, const std::string& str_name, const std::string& topic_name);
	void	Life(const float timeStep);
	void	setParams(const float& temp, const float& room_mod_max, const json& j);
	json	getPID_params();

	std::string	name;	//Имя термостата для поиска в векторе

	struct Output{
		float	dt;
		float	ch_temp_zad	= 30;		//Заданная температура теплоносителя
		float	mod_max	= 100;			//Максимальная модуляция
	}out;
};

#endif	//ROOM_THERMOSTAT_H