#ifndef BOILER_TASK_H
#define BOILER_TASK_H

#include "ot_boiler.h"

extern bool	OT_is_enabled;
extern const OT_Boiler*	pBoiler;
extern const json*		pControlStatus;
void	boiler_task(void* unused);

#endif	//BOILER_TASK_H