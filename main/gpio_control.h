#ifndef GPIO_CONTROL_H
#define GPIO_CONTROL_H

void	gpio_control(void* unused);
void	gpio_status(std::ostringstream& ss);

extern bool		pin_heater_1_isOn;
extern bool		pin_heater_2_isOn;

#endif  //GPIO_CONTROL_H