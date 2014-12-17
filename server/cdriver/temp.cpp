#include "cdriver.h"

void Temp::load(int32_t &addr, int id)
{
	R0 = read_float(addr);
	R1 = read_float(addr);
	logRc = read_float(addr);
	Tc = read_float(addr);
	beta = read_float(addr);
	K = exp(logRc - beta / Tc);
	//debug("K %f R0 %f R1 %f logRc %f Tc %f beta %f", F(K), F(R0), F(R1), F(logRc), F(Tc), F(beta));
	/*
	core_C = read_float(addr);
	shell_C = read_float(addr);
	transfer = read_float(addr);
	radiation = read_float(addr);
	power = read_float(addr);
	*/
	power_pin[0].read(read_16(addr));
	power_pin[1].read(read_16(addr));
	int old_pin = thermistor_pin.write();
	bool old_valid = thermistor_pin.valid();
	thermistor_pin.read(read_16(addr));
	target[1] = read_float(addr);
	for (int i = 0; i < 2; ++i) {
		adctarget[i] = toadc(target[i]);
		SET_OUTPUT(power_pin[i]);
		if (is_on[i])
			SET(power_pin[i]);
		else
			RESET(power_pin[i]);
	}
	if (old_pin != thermistor_pin.write() && old_valid)
		arch_setup_temp(~0, old_pin, false);
	if (thermistor_pin.valid())
		arch_setup_temp(id, thermistor_pin.pin, true, power_pin[0].valid() ? power_pin[0].pin : ~0, power_pin[0].inverted(), adctarget[0], power_pin[1].valid() ? power_pin[1].pin : ~0, power_pin[1].inverted(), adctarget[1]);
}

void Temp::save(int32_t &addr)
{
	write_float(addr, R0);
	write_float(addr, R1);
	write_float(addr, logRc);
	write_float(addr, Tc);
	write_float(addr, beta);
	/*
	write_float(addr, core_C);
	write_float(addr, shell_C);
	write_float(addr, transfer);
	write_float(addr, radiation);
	write_float(addr, power);
	*/
	write_16(addr, power_pin[0].write());
	write_16(addr, power_pin[1].write());
	write_16(addr, thermistor_pin.write());
	write_float(addr, target[1]);
}

float Temp::fromadc(int32_t adc) {
	if (adc <= 0)
		return INFINITY;
	if (adc >= MAXINT)
		return NAN;
	//debug("adc: %d", adc);
	// Symbols: A[ms]: adc value, R[01s]: resistor value, V[m01s]: voltage
	// with m: maximum value, 0: series resistor, 1: parallel resistor, s: sensed value (thermistor)
	// Compute Rs from As:
	// As/Am = Vs/Vm = [1/(1/R1+1/R0)]/[R0+1/(1/R1+1/R0)]
	// Am/As = 1+R0/Rs+R0/R1
	// => 1/Rs = R0*Am/As-1/R0-1/R1
	// Compute T from R (:= Rs).
	// R = K * exp(beta / T)
	// T = beta/log(R/K)=-beta/log(K*Am/R0/As-K/R0-K/R1)
	if (isnan(beta)) {
		// beta == NAN is used for calibration: return raw value as K.
		return adc * R0 + R1;
	}
	//debug("K: %f adc: %d beta: %f", F(K), adc, F(beta));
	return -beta / log(K * ((1 << ADCBITS) / R0 / adc - 1 / R0 - 1 / R1));
}

int32_t Temp::toadc(float T) {
	if (isnan(T) || T <= 0)
		return MAXINT;
	if (isinf(T) && T > 0)
		return -1;
	if (isnan(beta)) {
		return T - R1 / R0;
	}
	float Rs = K * exp(beta * 1. / T);
	//debug("K %f Rs %f R0 %f logRc %f Tc %f beta %f", F(K), F(Rs), F(R0), F(logRc), F(Tc), F(beta));
	return ((1 << ADCBITS) - 1) * Rs / (Rs + R0);
}

void Temp::init() {
	R0 = NAN;
	R1 = INFINITY;
	logRc = NAN;
	Tc = 20 + 273.15;
	beta = NAN;
	/*
	core_C = NAN;
	shell_C = NAN;
	transfer = NAN;
	radiation = NAN;
	power = NAN;
	*/
	thermistor_pin.init();
	min_alarm = NAN;
	max_alarm = NAN;
	adcmin_alarm = -1;
	adcmax_alarm = MAXINT;
	for (int i = 0; i < 2; ++i) {
		power_pin[i].init();
		target[i] = NAN;
		adctarget[i] = MAXINT;
		is_on[i] = false;
	}
	following_gpios = ~0;
	last_temp_time = utime();
	time_on = 0;
	K = NAN;
}

void Temp::free() {
	if (thermistor_pin.valid())
		arch_setup_temp(~0, thermistor_pin.pin, false);
	power_pin[0].read(0);
	power_pin[1].read(0);
	thermistor_pin.read(0);
}

void Temp::copy(Temp &dst) {
	dst.R0 = R0;
	dst.R1 = R1;
	dst.logRc = logRc;
	dst.Tc = Tc;
	dst.beta = beta;
	/*
	dst.core_C = core_C;
	dst.shell_C = shell_C;
	dst.transfer = transfer;
	dst.radiation = radiation;
	dst.power = power;
	*/
	for (int i = 0; i < 2; ++i) {
		dst.power_pin[i].read(power_pin[i].write());
		dst.target[i] = target[i];
		dst.adctarget[i] = adctarget[i];
		dst.is_on[i] = is_on[i];
	}
	dst.thermistor_pin.read(thermistor_pin.write());
	dst.min_alarm = min_alarm;
	dst.max_alarm = max_alarm;
	dst.adcmin_alarm = adcmin_alarm;
	dst.adcmax_alarm = adcmax_alarm;
	dst.following_gpios = following_gpios;
	dst.last_temp_time = last_temp_time;
	dst.time_on = time_on;
	dst.K = K;
}

void handle_temp(int id, int temp) { // {{{
	if (requested_temp == id) {
		//debug("replying temp");
		float result = temps[requested_temp].fromadc(temp);
		requested_temp = ~0;
		send_host(CMD_TEMP, 0, 0, result);
	}
	//debug("temp for %d: %d", id, temp);
	// If an alarm should be triggered, do so.  Adc values are higher for lower temperatures.
	//debug("alarms: %d %d %d %d", id, temps[id].adcmin_alarm, temps[id].adcmax_alarm, temp);
	if ((temps[id].adcmin_alarm < MAXINT && temps[id].adcmin_alarm >= temp) || temps[id].adcmax_alarm <= temp) {
		temps[id].min_alarm = NAN;
		temps[id].max_alarm = NAN;
		temps[id].adcmin_alarm = MAXINT;
		temps[id].adcmax_alarm = MAXINT;
		send_host(CMD_TEMPCB, id);
	}
	/*
	// TODO: Make this work and decide on units.
	// We have model settings.
	uint32_t dt = current_time - temps[id].last_temp_time;
	if (dt == 0)
		return;
	temps[id].last_temp_time = current_time;
	// Heater and core/shell transfer.
	if (temps[id].is_on)
		temps[id].core_T += temps[id].power / temps[id].core_C * dt;
	float Q = temps[id].transfer * (temps[id].core_T - temps[id].shell_T) * dt;
	temps[id].core_T -= Q / temps[id].core_C;
	temps[id].shell_T += Q / temps[id].shell_C;
	if (temps[id].is_on)
		temps[id].core_T += temps[id].power / temps[id].core_C * dt / 2;
	// Set shell to measured value.
	temps[id].shell_T = temp;
	// Add energy if required.
	float E = temps[id].core_T * temps[id].core_C + temps[id].shell_T * temps[id].shell_C;
	float T = E / (temps[id].core_C + temps[id].shell_C);
	// Set the pin to correct value.
	if (T < temps[id].target) {
		if (!temps[id].is_on) {
			SET(temps[id].power_pin);
			temps[id].is_on = true;
			++temps_busy;
		}
		else
			temps[id].time_on += current_time - temps[id].last_temp_time;
	}
	else {
		if (temps[id].is_on) {
			RESET(temps[id].power_pin);
			temps[id].is_on = false;
			temps[id].time_on += current_time - temps[id].last_temp_time;
			--temps_busy;
		}
	}
	*/
} // }}}
