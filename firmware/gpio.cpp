#include "firmware.h"

void Gpio::load (int16_t &addr, bool eeprom)
{
	uint8_t oldmaster = master;
	pin.read (read_16 (addr, eeprom));
	state = read_8 (addr, eeprom);
#ifndef LOWMEM
	master = read_8 (addr, eeprom);
	value = read_float (addr, eeprom) + 273.15;
#else
	read_8 (addr, eeprom);
	read_float (addr, eeprom);
#endif
	// State:  0: disabled, 1: pullup input, 2: low, 3: high
	if (state & 2) {
		SET_OUTPUT (pin);
		if (state & 1)
			SET (pin);
		else
			RESET (pin);
	}
	else if (state & 1)
		SET_INPUT (pin);
	else
		SET_INPUT_NOPULLUP (pin);
#ifndef LOWMEM
	if (master >= MAXOBJECT || !temps[master])
		master = 0;
	if (oldmaster != master)
	{
		// Disable old links.
		if (oldmaster != 0)
		{
			if (prev)
				prev->next = next;
			else
				temps[oldmaster]->gpios = next;
			if (next)
				next->prev = prev;
		}
		// Set new links.
		if (master != 0)
		{
			prev = NULL;
			next = temps[master]->gpios;
			temps[master]->gpios = this;
			if (next)
				next->prev = this;
			// Also, set pin to output.
		}
		else
		{
			prev = NULL;
			next = NULL;
		}
	}
#endif
}


void Gpio::save (int16_t &addr, bool eeprom)
{
	write_16 (addr, pin.write (), eeprom);
	write_8 (addr, state, eeprom);
#ifndef LOWMEM
	write_8 (addr, master, eeprom);
	write_float (addr, value - 273.15, eeprom);
#else
	write_8 (addr, 0, eeprom);
	write_float (addr, NAN, eeprom);
#endif
}