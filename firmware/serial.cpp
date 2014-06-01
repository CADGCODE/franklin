#include "firmware.h"

//#define DEBUG_SERIAL
//#define DEBUG_FF

// Commands which can be sent:
// Packet: n*4 bytes, of which n*3 bytes are data.
// Ack: 1 byte.
// Nack: 1 byte.

// The first byte of a packet is the length: 0lllllll
// The second byte of a packet is the flipflop and the command (fccccccc)
// All other commands have bit 7 set, so they cannot be mistaken for a packet.
// They have 4 bit data and 3 bit parity: 1pppdddd
// static const uint8_t MASK1[3] = {0x4b, 0x2d, 0x1e}
// Codes (low nybble is data): 80 (e1 d2) b3 b4 (d5 e6) 87 (f8) 99 aa (cb cc) ad 9e (ff)
// Codes which have duplicates in printer id codes are not used.
// These are defined in firmware.h.

static uint8_t ff_in = 0;
static uint8_t ff_out = 0;
static unsigned long last_micros = 0;
static bool had_data = false;

// Parity masks for decoding.
static const uint8_t MASK[5][4] = {
	{0xc0, 0xc3, 0xff, 0x09},
	{0x38, 0x3a, 0x7e, 0x13},
	{0x26, 0xb5, 0xb9, 0x23},
	{0x95, 0x6c, 0xd5, 0x43},
	{0x4b, 0xdc, 0xe2, 0x83}};

// There may be serial data available.
void serial ()
{
	if (!had_data && command_end > 0 && micros () - last_micros >= 100000)
	{
		// Command not finished; ignore it and wait for next.
#ifdef WATCHDOG
		wdt_reset ();
#endif
		command_end = 0;
	}
	had_data = false;
	while (command_end == 0)
	{
		if (!Serial.available ()) {
#ifdef WATCHDOG
			wdt_reset ();
#endif
			return;
		}
		had_data = true;
		command[0] = Serial.read ();
#ifdef DEBUG_SERIAL
		debug ("received: %x", command[0]);
#endif
		// If this is a 1-byte command, handle it.
		switch (command[0])
		{
		case CMD_ACK0:
		case CMD_ACK1:
			// Ack: everything was ok; flip the flipflop.
			if (out_busy && (!ff_out ^ (command[0] == CMD_ACK1))) {	// Only if we expected it and it is the right type.
				ff_out ^= 0x80;
#ifdef DEBUG_FF
				debug ("new ff_out: %d", ff_out);
#endif
				out_busy = false;
				try_send_next ();
			}
			continue;
		case CMD_NACK:
			// Nack: the host didn't properly receive the packet: resend.
			// Unless the last packet was already received; in that case ignore the NACK.
			if (out_busy)
				send_packet (last_packet);
			continue;
		case CMD_ID:
			// Request printer id.  This is called when the host
			// connects to the printer.  This may be a reconnect,
			// and can happen at any time.
			// Response is to send the printer id.
			Serial.write (CMD_ID);
			for (uint8_t i = 0; i < ID_SIZE; ++i)
				Serial.write (printerid[i]);
			continue;
		default:
			break;
		}
		if (command[0] < 3 || command[0] == 4 || command[0] >= 0x80) {
			// These lengths are not allowed; this cannot be a good packet.
			Serial.write (CMD_NACK);
			continue;
		}
		command_end = 1;
		last_micros = micros ();
	}
	int len = Serial.available ();
	if (len == 0)
	{
#ifdef DEBUG_SERIAL
		debug ("no more data available now");
#endif
#ifdef WATCHDOG
		wdt_reset ();
#endif
		// If an out packet is waiting for ACK for too long, assume it didn't arrive and resend it.
		if (out_busy && micros () - out_time >= 200000) {
#ifdef DEBUG_SERIAL
			debug ("resending packet");
#endif
			send_packet (last_packet);
		}
		return;
	}
	had_data = true;
	if (len + command_end > COMMAND_SIZE)
		len = COMMAND_SIZE - command_end;
	uint8_t cmd_len = command[0] & COMMAND_LEN_MASK;
	cmd_len += (cmd_len + 2) / 3;
	if (command_end + len > cmd_len)
		len = cmd_len - command_end;
	Serial.readBytes (reinterpret_cast <char *> (&command[command_end]), len);
#ifdef DEBUG_SERIAL
	debug ("read %d bytes", len);
#endif
	last_micros = micros ();
	command_end += len;
	if (command_end < cmd_len)
	{
#ifdef DEBUG_SERIAL
		debug ("not done yet; %d of %d received.", command_end, cmd_len);
#endif
#ifdef WATCHDOG
		wdt_reset ();
#endif
		return;
	}
#ifdef WATCHDOG
	wdt_reset ();
#endif
	command_end = 0;
	// Check packet integrity.
	// Size must be good.
	if (command[0] <= 2 || command[0] == 4)
	{
		Serial.write (CMD_NACK);
		return;
	}
	// Checksum must be good.
	len = command[0] & ~0x80;
	for (uint8_t t = 0; t < (len + 2) / 3; ++t)
	{
		uint8_t sum = command[len + t];
		if ((sum & 0x7) != (t & 0x7))
		{
			Serial.write (CMD_NACK);
			return;
		}
		for (uint8_t bit = 0; bit < 5; ++bit)
		{
			uint8_t check = sum & MASK[bit][3];
			for (uint8_t p = 0; p < 3; ++p)
				check ^= command[3 * t + p] & MASK[bit][p];
			check ^= check >> 4;
			check ^= check >> 2;
			check ^= check >> 1;
			if (check & 1)
			{
				Serial.write (CMD_NACK);
				return;
			}
		}
	}
	// Packet is good.
#ifdef DEBUG_SERIAL
	debug ("good");
#endif
	// Flip-flop must have good state.
	if ((command[1] & 0x80) != ff_in)
	{
		// Wrong: this must be a retry to send the previous packet, so our ack was lost.
		// Resend the ack, but don't do anything (the action has already been taken).
#ifdef DEBUG_SERIAL
		debug ("duplicate");
#endif
#ifdef DEBUG_FF
		debug ("old ff_in: %d", ff_in);
#endif
		Serial.write (ff_in ? CMD_ACK0 : CMD_ACK1);
		return;
	}
	// Right: update flip-flop and send ack.
	ff_in ^= 0x80;
#ifdef DEBUG_FF
	debug ("new ff_in: %d", ff_in);
#endif
	// Clear flag for easier parsing.
	command[1] &= ~0x80;
	packet ();
}

// Command sending method:
// When sending a command:
// - fill appropriate command buffer
// - set flag to signal data is ready
// - call try_send_next
//
// When ACK is received:
// - call try_send_next
//
// When NACK is received:
// - call send_packet to resend the last packet

// Set checksum bytes.
static void prepare_packet (char *the_packet)
{
#ifdef DEBUG_SERIAL
	debug ("prepare");
#endif
	if (the_packet[0] >= COMMAND_SIZE)
	{
		debug ("packet is too large: %d > %d", the_packet[0], COMMAND_SIZE);
		return;
	}
	// Set flipflop bit.
	the_packet[1] &= ~0x80;
	the_packet[1] ^= ff_out;
#ifdef DEBUG_FF
	debug ("use ff_out: %d", ff_out);
#endif
	// Compute the checksums.  This doesn't work for size in (1, 2, 4), so
	// the protocol doesn't allow those.
	// For size % 3 != 0, the first checksums are part of the data for the
	// last checksum.  This means they must have been filled in at that
	// point.  (This is also the reason (1, 2, 4) are not allowed.)
	uint8_t cmd_len = the_packet[0] & COMMAND_LEN_MASK;
	for (uint8_t t = 0; t < (cmd_len + 2) / 3; ++t)
	{
		uint8_t sum = t & 7;
		for (uint8_t bit = 0; bit < 5; ++bit)
		{
			uint8_t check = 0;
			for (uint8_t p = 0; p < 3; ++p)
				check ^= the_packet[3 * t + p] & MASK[bit][p];
			check ^= sum & MASK[bit][3];
			check ^= check >> 4;
			check ^= check >> 2;
			check ^= check >> 1;
			if (check & 1)
				sum |= 1 << (bit + 3);
		}
		the_packet[cmd_len + t] = sum;
	}
}

// Send packet to host.
void send_packet (char *the_packet)
{
#ifdef DEBUG_SERIAL
	debug ("send");
#endif
	last_packet = the_packet;
	uint8_t cmd_len = the_packet[0] & COMMAND_LEN_MASK;
	for (uint8_t t = 0; t < cmd_len + (cmd_len + 2) / 3; ++t)
		Serial.write (the_packet[t]);
	out_busy = true;
	out_time = micros ();
}

// Call send_packet if we can.
void try_send_next ()
{
#ifdef DEBUG_SERIAL
	debug ("try send");
#endif
	if (out_busy)
	{
#ifdef DEBUG_SERIAL
		debug ("still busy");
#endif
		// Still busy sending other packet.
		return;
	}
#if MAXAXES > 0
	for (uint8_t w = 0; w < MAXAXES; ++w)
	{
		if (!isnan (limits_pos[w]))
		{
#ifdef DEBUG_SERIAL
			debug ("limit %d", w);
#endif
			out_buffer[0] = 7;
			out_buffer[1] = CMD_LIMIT;
			out_buffer[2] = w;
			ReadFloat f;
			f.f = limits_pos[w];
			out_buffer[3] = f.b[0];
			out_buffer[4] = f.b[1];
			out_buffer[5] = f.b[2];
			out_buffer[6] = f.b[3];
			limits_pos[w] = NAN;
			if (initialized)
			{
				prepare_packet (out_buffer);
				send_packet (out_buffer);
			}
			else
				try_send_next ();
			return;
		}
		if (axis[w].sense_state & 1)
		{
#ifdef DEBUG_SERIAL
			debug ("sense %d %d %d", w, axis[w].sense_state, axis[w].sense_pos);
#endif
			out_buffer[0] = 7;
			out_buffer[1] = CMD_SENSE;
			out_buffer[2] = w | (axis[w].sense_state & 0x80);
			ReadFloat f;
			f.f = axis[w].sense_pos;
			out_buffer[3] = f.b[0];
			out_buffer[4] = f.b[1];
			out_buffer[5] = f.b[2];
			out_buffer[6] = f.b[3];
			axis[w].sense_state &= ~1;
			if (initialized)
			{
				prepare_packet (out_buffer);
				send_packet (out_buffer);
			}
			else
				try_send_next ();
			return;
		}
	}
#endif
	if (num_movecbs > 0)
	{
#ifdef DEBUG_SERIAL
		debug ("sending %d movecbs", num_movecbs);
#endif
		out_buffer[0] = 3;
		out_buffer[1] = CMD_MOVECB;
		out_buffer[2] = num_movecbs;
		num_movecbs = 0;
		prepare_packet (out_buffer);
		send_packet (out_buffer);
		return;
	}
#if MAXTEMPS > 0 || MAXEXTRUDERS > 0
	if (which_tempcbs != 0)
	{
#ifdef DEBUG_SERIAL
		debug ("tempcb %d", which_tempcbs);
#endif
		for (uint8_t w = 0; w < MAXOBJECT; ++w)
		{
			if (which_tempcbs & (1 << w))
			{
				out_buffer[0] = 3;
				out_buffer[1] = CMD_TEMPCB;
				out_buffer[2] = w;
				which_tempcbs &= ~(1 << w);
				break;
			}
		}
		if (initialized)
		{
			prepare_packet (out_buffer);
			send_packet (out_buffer);
		}
		else
			try_send_next ();
		return;
	}
#endif
	if (reply_ready)
	{
#ifdef DEBUG_SERIAL
		debug ("reply %x %d", reply[1], reply[0]);
#endif
		prepare_packet (reply);
		send_packet (reply);
		reply_ready = false;
		return;
	}
	if (continue_cb & 1)
	{
#ifdef DEBUG_SERIAL
		debug ("continue move");
#endif
		out_buffer[0] = 3;
		out_buffer[1] = CMD_CONTINUE;
		out_buffer[2] = 0;
		continue_cb &= ~1;
		if (initialized)
		{
			prepare_packet (out_buffer);
			send_packet (out_buffer);
		}
		else
			try_send_next ();
		return;
	}
	if (continue_cb & 2)
	{
#ifdef DEBUG_SERIAL
		debug ("continue audio");
#endif
		out_buffer[0] = 3;
		out_buffer[1] = CMD_CONTINUE;
		out_buffer[2] = 1;
		continue_cb &= ~2;
		if (initialized)
		{
			prepare_packet (out_buffer);
			send_packet (out_buffer);
		}
		else
			try_send_next ();
		return;
	}
	if (which_autosleep != 0)
	{
		out_buffer[0] = 3;
		out_buffer[1] = CMD_AUTOSLEEP;
		out_buffer[2] = which_autosleep;
		which_autosleep = 0;
		if (initialized)
		{
			prepare_packet (out_buffer);
			send_packet (out_buffer);
		}
		else
			try_send_next ();
		return;
	}
	if (ping != 0)
	{
		for (uint8_t b = 0; b < 8; ++b)
		{
			if (ping & 1 << b)
			{
				reply[0] = 3;
				reply[1] = CMD_PONG;
				reply[2] = b;
				prepare_packet (reply);
				send_packet (reply);
				ping &= ~(1 << b);
				return;
			}
		}
	}
}

void write_ack ()
{
	Serial.write (ff_in ? CMD_ACK0 : CMD_ACK1);
}

void write_ackwait ()
{
	Serial.write (ff_in ? CMD_ACKWAIT0 : CMD_ACKWAIT1);
}
