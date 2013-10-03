# vim: set foldmethod=marker :

show_own_debug = False
show_firmware_debug = True

# Imports.  {{{
import serial
import time
import math
import struct
import os
import re
import sys
# }}}

def dprint (x, data): # {{{
	if show_own_debug:
		print ('%s: %s' % (x, ' '.join (['%02x' % ord (c) for c in data])))
# }}}

class Printer: # {{{
	# Internal stuff.  {{{
	# Constants.  {{{
	# Serial timeout for normal communication in seconds.
	default_timeout = 5	# TODO: this is for debugging; really should be .005
	long_timeout = 3	# Timeout when waiting for the printer to boot.  May need 15 for non-optiboot.
	# Masks for computing checksums.
	mask = [	[0xc0, 0xc3, 0xff, 0x09],
			[0x38, 0x3a, 0x7e, 0x13],
			[0x26, 0xb5, 0xb9, 0x23],
			[0x95, 0x6c, 0xd5, 0x43],
			[0x4b, 0xdc, 0xe2, 0x83]]
	# Single-byte commands.
	single = { 'ACK': '\x80', 'NACK': '\xe1', 'ACKWAIT': '\xd2', 'STALL': '\xb3', 'RESET': '\xf4', 'INIT': '\x95', 'ACKRESET': '\xa6', 'DEBUG': '\xc7' }
	command = {
			'BEGIN': 0x00,
			'GOTO': 0x01,
			'GOTOCB': 0x02,
			'RUN': 0x03,
			'SLEEP': 0x04,
			'SETTEMP': 0x05,
			'WAITTEMP': 0x06,
			'READTEMP': 0x07,
			'SETPOS': 0x08,
			'GETPOS': 0x09,
			'LOAD': 0x0a,
			'SAVE': 0x0b,
			'READ': 0x0c,
			'WRITE': 0x0d,
			'PAUSE': 0x0e,
			'PING': 0x0f,
			'PLAY': 0x10}
	rcommand = {
			'START': '\x11',
			'TEMP': '\x12',
			'POS': '\x13',
			'DATA': '\x14',
			'PONG': '\x15',
			'MOVECB': '\x16',
			'TEMPCB': '\x17',
			'CONTINUE': '\x18',
			'LIMIT': '\x19',
			'MESSAGE': '\x1a'}
	# }}}
	def __init__ (self, name = None): # {{{
		# Assume a GNU/Linux system; if you have something else, you need to come up with a way to iterate over all your serial ports and implement it here.  Patches welcome, especially if they are platform-independent.
		blacklist = 'console$|ttyS?\d*$'
		for p in os.listdir ('/sys/class/tty'):
			if re.match (blacklist, p):
				continue
			try:
				self.printer = serial.Serial ('/dev/' + p, baudrate = 115200, timeout = .01)
				# Reset firmware.
				self.printer.setDTR (False)
				time.sleep (.1)
				self.printer.setDTR (True)
				time.sleep (.1)
				self.printer.readall ()	# flush buffer.
				# Wait for firmware to boot.
				self.printer.setTimeout (Printer.long_timeout)
				r = self.printer.read ()
				dprint ('read (0)', r)
				if len (r) == 0:
					continue
				self.printer.setTimeout (Printer.default_timeout)
				while True:
					if r == self.single['DEBUG']:
						self.handle_debug ()
						r = self.printer.read ()
						dprint ('read (0)', r)
						continue
					if r == self.single['INIT']:
						break
					dprint ('unexpected data', r)
					r = self.printer.read ()
					dprint ('read (0)', r)
				self.printer.setTimeout (Printer.default_timeout)
				# Set up state.
				self.ff_in = False
				self.ff_out = False
				self.limits = {}
				self.wait = False
				self.movewait = 0
				self.tempwait = set ()
				self.begin ()
				self.namelen, self.maxaxes, self.maxextruders, self.maxtemps = struct.unpack ('<BBBB', self.read (0))
				self.load (1)
				if name is None or not re.match (name, self.name):
					self.axis = [Printer.Axis (self, t) for t in range (self.maxaxes)]
					for a in range (self.maxaxes):
						self.axis[a].read (self.read (2 + a))
					self.extruder = [Printer.Extruder () for t in range (self.maxextruders)]
					for e in range (self.maxextruders):
						self.extruder[e].read (self.read (2 + self.maxaxes + e))
					self.temp = [Printer.Temp () for t in range (self.maxtemps)]
					for t in range (self.maxtemps):
						self.temp[t].read (self.read (2 + self.maxaxes + self.maxextruders + t))
					global show_own_debug
					if show_own_debug is None:
						show_own_debug = True
					return
			except:
				pass
		sys.stderr.write ('Printer not found\n')
		sys.exit (0)
	# }}}
	def make_packet (self, data): # {{{
		data = chr (len (data) + 1) + data
		for t in range ((len (data) + 2) / 3):
			check = t & 0x7
			for bit in range (5):
				sum = check & Printer.mask[bit][3]
				for byte in range (3):
					sum ^= ord (data[3 * t + byte]) & Printer.mask[bit][byte]
				sum ^= sum >> 4
				sum ^= sum >> 2
				sum ^= sum >> 1
				if sum & 1:
					check |= 1 << (bit + 3)
			data += chr (check)
		#print ' '.join (['%02x' % ord (x) for x in data])
		return data
	# }}}
	def parse_packet (self, data): # {{{
		#print ' '.join (['%02x' % ord (x) for x in data])
		if ord (data[0]) + (ord (data[0]) + 2) / 3 != len (data):
			return None
		length = ord (data[0])
		checksum = data[length:]
		for t in range (len (checksum)):
			r = data[3 * t:3 * t + 3] + checksum[t]
			if (ord (checksum[t]) & 0x7) != (t & 7):
				return None
			for bit in range (5):
				sum = 0
				for byte in range (4):
					sum ^= ord (r[byte]) & Printer.mask[bit][byte]
				sum ^= sum >> 4
				sum ^= sum >> 2
				sum ^= sum >> 1
				if (sum & 1) != 0:
					return None
		return data[1:length]
	# }}}
	def send_packet (self, data): # {{{
		if self.ff_out:
			data = chr (ord (data[0]) | 0x80) + data[1:]
		self.ff_out = not self.ff_out
		data = self.make_packet (data)
		events = 0
		ready = 0
		while True:
			dprint ('(1) writing', data);
			self.printer.write (data)
			while True:
				r = self.printer.read (1)
				if r != self.single['DEBUG']:
					dprint ('(1) read', r)
				if r == '':
					break	# Packet was not received.  Break from this loop to resend it.
				if r == self.single['DEBUG']:
					self.handle_debug ()
					continue
				if r == self.single['ACK']:
					return	# Done.
				if r == self.single['NACK']:
					break	# Break from this loop, so the packet is sent again.
				if r == self.single['ACKWAIT']:
					self.wait = True
					return	# Done.
				assert r != self.single['STALL']	# This must not happen and if it does, there's no way to handle it, so raise an exception.
				assert r != self.single['RESET']	# This should only be sent to the firmware, not to the host.
				assert r != self.single['INIT']		# This means the printer has reset; that shouldn't happen (but should be handled better; TODO).
				assert r != self.single['ACKRESET']	# This should only happen after we request it.
				if (ord (r) & 0x80) != 0:
					dprint ('writing (1)', self.single['NACK']);
					self.printer.write (self.single['NACK'])
					continue
				# Regular packet received.  Must be asynchronous; handle it.
				#print repr (r)
				reply = self.recv_packet (r, True)
				#print repr (reply)
				assert reply == ''
	# }}}
	def recv_packet (self, buffer = '', want_any = False, end_time = None): # {{{
		if end_time:
			self.printer.setTimeout (end_time - time.time ())
		while True:
			while len (buffer) < 1:
				r = self.printer.read (1)
				if r != self.single['DEBUG']:
					dprint ('(3) read', r)
				if r == '':
					if end_time and time.time () >= end_time:
						self.printer.setTimeout (Printer.default_timeout)
						return ''
					dprint ('writing (2)', self.single['NACK']);
					self.printer.write (self.single['NACK'])
					continue
				if r == self.single['DEBUG']:
					self.handle_debug ()
					continue
				assert r != self.single['INIT']	# This should be handled more gracefully.  TODO.
				assert (ord (r) & 0x80) == 0	# This must not happen.
				buffer += r
			length = ord (buffer[0]) + (ord (buffer[0]) + 2) / 3
			while len (buffer) < length:
				r = self.printer.read (length - len (buffer))
				dprint ('read (%d/%d)' % (len (r) + len (buffer), length), r)
				if r == '':
					dprint ('writing (3)', self.single['NACK']);
					self.printer.write (self.single['NACK'])
					buffer = ''
					break	# Start over.
				buffer += r
			else:
				# Entire buffer has been received.
				data = self.parse_packet (buffer)
				if data is None:
					dprint ('writing (4)', self.single['NACK']);
					self.printer.write (self.single['NACK'])
					buffer = ''
					continue	# Start over.
				if bool (ord (data[0]) & 0x80) != self.ff_in:
					# This was a retry of the previous packet; accept it and retry.
					dprint ('(2) writing', self.single['ACK']);
					self.printer.write (self.single['ACK'])
					buffer = ''
					continue	# Start over.
				# Packet successfully received.
				dprint ('(3) writing', self.single['ACK']);
				self.printer.write (self.single['ACK'])
				# Clear the flip flop.
				data = chr (ord (data[0]) & ~0x80) + data[1:]
				# Flip it.
				self.ff_in = not self.ff_in
				# Handle the asynchronous events here; don't bother the caller with them.
				if data[0] == self.rcommand['MOVECB']:
					for i in range (ord (data[1])):
						assert self.movewait > 0
						self.movewait -= 1
					if want_any:
						if end_time:
							self.printer.setTimeout (Printer.default_timeout)
						return ''
					buffer = ''
					continue	# Start over.
				if data[0] == self.rcommand['TEMPCB']:
					assert ord (data[0]) in self.tempwait
					self.tempwait.remove (ord (data[0]))
					if want_any:
						if end_time:
							self.printer.setTimeout (Printer.default_timeout)
						return ''
					buffer = ''
					continue	# Start over.
				if data[0] == self.rcommand['CONTINUE']:
					self.wait = False
					if want_any:
						if end_time:
							self.printer.setTimeout (Printer.default_timeout)
						return ''
					buffer = ''
					continue	# Start over.
				if data[0] == self.rcommand['LIMIT']:
					self.limits[ord (data[1])] = struct.unpack ('<l', data[2:])[0]
					if want_any:
						if end_time:
							self.printer.setTimeout (Printer.default_timeout)
						return ''
					buffer = ''
					continue	# Start over.
				if data[0] == self.rcommand['MESSAGE']:
					self.messages.append ((struct.unpack ('<l', data[1:5])[0], data[5:]))
					print ('Message: %d %s' % self.messages[-1])
					continue	# Start over.
				if end_time:
					self.printer.setTimeout (Printer.default_timeout)
				return data
	# }}}
	def block (self, timeout = 30, probe = True): # {{{
		self.printer.setTimeout (timeout)
		r = self.printer.read ()
		dprint ('(2) read', r)
		assert probe or r != ''
		self.printer.setTimeout (Printer.default_timeout)
		assert self.recv_packet (r, True) == ''
	# }}}
	def handle_debug (self): # {{{
		s = ''
		while True:
			r = self.printer.read (1)
			if r in ('', '\0'):
				break
			s += r
		if show_firmware_debug:
			print ('Debug: %s' % s)
	# }}}
	# Config stuff.  {{{
	class Temp: # {{{
		def read (self, data):
			self.alpha, self.beta, self.core_C, self.shell_C, self.transfer, self.radiation, self.power, self.power_pin, self.thermistor_pin = struct.unpack ('<fffffffBB', data[:30])
			return data[30:]
		def write (self):
			return struct.pack ('<fffffffBB', self.alpha, self.beta, self.core_C, self.shell_C, self.transfer, self.radiation, self.power, self.power_pin, self.thermistor_pin)
	# }}}
	class Motor: # {{{
		def read (self, data):
			self.step_pin, self.dir_pin, self.enable_pin, self.steps_per_mm, self.max_f_neg, self.max_f_pos = struct.unpack ('<BBBfff', data[:15])
			return data[15:]
		def write (self):
			return struct.pack ('<BBBfff', self.step_pin, self.dir_pin, self.enable_pin, self.steps_per_mm, self.max_f_neg, self.max_f_pos)
	# }}}
	class Axis: # {{{
		def __init__ (self, printer, id):
			self.printer = printer
			self.id = id
			self.motor = Printer.Motor ()
		def read (self, data):
			data = self.motor.read (data)
			self.limit_min_pin, self.limit_max_pin = struct.unpack ('<BB', data)
		def write (self):
			return self.motor.write () + struct.pack ('<BB', self.limit_min_pin, self.limit_max_pin)
		def set_current_pos (self, pos):
			self.printer.send_packet (struct.pack ('<BBl', self.printer.command['SETPOS'], 2 + self.id, pos))
		def get_current_pos (self):
			self.printer.send_packet (struct.pack ('<BB', self.printer.command['GETPOS'], 2 + self.id))
			ret = self.recv_packet ()
			assert ret[0] == self.rcommand['POS']
			return struct.unpack ('<l', ret[1:])[0]
	# }}}
	class Extruder: # {{{
		def __init__ (self):
			self.motor = Printer.Motor ()
			self.temp = Printer.Temp ()
		def read (self, data):
			data = self.motor.read (data)
			data = self.temp.read (data)
			self.filament_heat, self.nozzle_size, self.filament_size = struct.unpack ('<fff', data)
		def write (self):
			return self.motor.write () + self.temp.write () + struct.pack ('<fff', self.filament_heat, self.nozzle_size, self.filament_size)
	# }}}
	# }}}
	# Internal commands.  {{{
	def begin (self): # {{{
		self.send_packet (struct.pack ('<Bf', self.command['BEGIN'], 0))
		assert struct.unpack ('<Bf', self.recv_packet ()) == (ord (self.rcommand['START']), 0.0)
	# }}}
	def read (self, channel): # {{{
		self.send_packet (struct.pack ('<BB', self.command['READ'], channel))
		p = self.recv_packet ()
		assert p[0] == self.rcommand['DATA']
		return p[1:]
	# }}}
	# }}}
	# }}}
	# Useful commands.  {{{
	def goto (self, axes = {}, e = None, f0 = None, f1 = None, which = 0, cb = False): # {{{
		while self.wait:
			self.block ()
		if cb:
			self.movewait += 1
			p = chr (self.command['GOTOCB'])
		else:
			p = chr (self.command['GOTO'])
		if isinstance (axes, (list, tuple)):
			a = {}
			for i, axis in enumerate (axes):
				a[i] = axis
			axes = a
		targets = [0] * (((2 + self.num_axes + self.num_extruders - 1) >> 3) + 1)
		args = ''
		if f0 is None:
			f0 = float ('inf')
		if f1 is None:
			f1 = f0
		targets[0] |= 1 << 0
		targets[0] |= 1 << 1
		args += struct.pack ('<f', f0)
		args += struct.pack ('<f', f1)
		a = axes.keys ()
		a.sort ()
		for axis in a:
			assert axis < self.num_axes
			targets[(axis + 2) >> 3] |= 1 << ((axis + 2) & 0x7)
			args += struct.pack ('<f', axes[axis])
		if e is not None:
			targets[(2 + self.num_axes + which) >> 3] |= 1 << ((2 + self.num_axes + which) & 0x7)
			args += struct.pack ('<f', e)
		self.send_packet (p + ''.join ([chr (t) for t in targets]) + args)
	# }}}
	def run_axis (self, which, speed): # {{{
		self.run (2 + which, speed)
	# }}}
	def run_extruder (self, which, speed): # {{{
		self.run (2 + self.maxaxes + which, speed)
	# }}}
	def run (self, channel, speed):	# speed: float; 0 means off. # {{{
		self.send_packet (struct.pack ('<BBf', self.command['RUN'], channel, speed))
	# }}}
	def sleep_axis (self, which, sleeping = True): # {{{
		self.sleep (2 + which, sleeping)
	# }}}
	def sleep_extruder (self, which, sleeping = True): # {{{
		self.sleep (2 + self.maxaxes + which, sleeping)
	# }}}
	def sleep (self, channel, sleeping = True): # {{{
		self.send_packet (struct.pack ('<BB', self.command['SLEEP'], (channel & 0x7f) | (0x80 if sleeping else 0)))
	# }}}
	def settemp_extruder (self, which, temp):	# {{{
		self.settemp (2 + self.maxaxes + which, temp)
	# }}}
	def settemp_temp (self, which, temp):	# {{{
		self.settemp (2 + self.maxaxes + self.maxextruders + which, temp)
	# }}}
	def settemp (self, channel, temp): # {{{
		self.send_packet (struct.pack ('<BBf', self.command['SETTEMP'], channel, temp))
	# }}}
	def waittemp_extruder (self, which, min, max):	# {{{
		self.waittemp (2 + self.maxaxes + which, min, max)
	# }}}
	def waittemp_temp (self, which, min, max):	# {{{
		self.waittemp (2 + self.maxaxes + self.maxextruders + which, min, max)
	# }}}
	def waittemp (self, channel, min, max): # {{{
		if min is None:
			min = float ('nan')
		if max is None:
			max = float ('nan')
		self.send_packet (struct.pack ('<BBff', self.command['WAITTEMP'], channel, min, max))
		if math.isnan (min) and math.isnan (max):
			self.tempwait.discard (channel)
		else:
			self.tempwait.add (channel)
	# }}}
	def blocktemps (self): # {{{
		while len (self.tempwait) > 0:
			self.block ()
	# }}}
	def readtemp (self, channel): # {{{
		self.send_packet (struct.pack ('<BB', self.command['READTEMP'], channel))
		ret = self.recv_packet ()
		assert ret[0] == self.rcommand['TEMP']
		return struct.unpack ('<f', ret[1:])[0]
	# }}}
	def load_variables (self): # {{{
		self.load (1)
	# }}}
	def load_axis (self, which): # {{{
		self.load (2 + which)
	# }}}
	def load_extruder (self, which): # {{{
		self.load (2 + self.maxaxes + which)
	# }}}
	def load_temp (self, which): # {{{
		self.load (2 + self.maxaxes + self.maxextruders + which)
	# }}}
	def load (self, channel): # {{{
		self.send_packet (struct.pack ('<BB', self.command['LOAD'], channel))
		if channel == 1:
			data = self.read (1)
			self.name = data[:self.namelen]
			self.num_axes, self.num_extruders, self.num_temps, self.printer_type, self.led_pin, self.room_T, self.motor_limit, self.temp_limit = struct.unpack ('<BBBBBfLL', data[self.namelen:])
		elif 2 <= channel < 2 + self.maxaxes:
			self.axis[channel - 2].read (self.read (channel))
		elif 2 + self.maxaxes <= channel < 2 + self.maxaxes + self.maxextruders:
			self.extruder[channel - 2 - self.maxaxes].read (self.read (channel))
		else:
			assert channel < 2 + self.maxaxes + self.maxextruders + self.maxtemps
			self.temp[channel - 2 - self.maxaxes - self.maxextruders].read (self.read (channel))
	# }}}
	def load_all (self): # {{{
		for i in range (1, 2 + self.maxaxes + self.maxextruders + self.maxtemps):
			print ('loading %d' % i)
			self.load (i)
	# }}}
	def save_variables (self): # {{{
		self.save (1)
	# }}}
	def save_axis (self, which): # {{{
		self.save (2 + which)
	# }}}
	def save_extruder (self, which): # {{{
		self.save (2 + self.maxaxes + which)
	# }}}
	def save_temp (self, which): # {{{
		self.save (2 + self.maxaxes + self.maxextruders + which)
	# }}}
	def save (self, channel): # {{{
		self.send_packet (struct.pack ('<BB', self.command['SAVE'], channel))
	# }}}
	def save_all (self): # {{{
		for i in range (1, 2 + self.maxaxes + self.maxextruders + self.maxtemps):
			print ('saving %d' % i)
			self.save (i)
	# }}}
	def write_variables (self): # {{{
		data = (self.name + chr (0) * self.namelen)[:self.namelen] + struct.pack ('<BBBBBfLL', self.num_axes, self.num_extruders, self.num_temps, self.printer_type, self.led_pin, self.room_T, self.motor_limit, self.temp_limit)
		self.send_packet (struct.pack ('<BB', self.command['WRITE'], 1) + data)
	# }}}
	def write_axis (self, which): # {{{
		data = self.axis[which].write ()
		self.send_packet (struct.pack ('<BB', self.command['WRITE'], 2 + which) + data)
	# }}}
	def write_extruder (self, which): # {{{
		data = self.extruder[which].write ()
		self.send_packet (struct.pack ('<BB', self.command['WRITE'], 2 + self.maxaxes + which) + data)
	# }}}
	def write_temp (self, which): # {{{
		data = self.temp[which].write ()
		self.send_packet (struct.pack ('<BB', self.command['WRITE'], 2 + self.maxaxes + self.maxextruders + which) + data)
	# }}}
	def write (self, channel): # {{{
		if channel == 1:
			self.write_variables ()
		elif 2 <= channel < 2 + self.maxaxes:
			self.write_axis (channel - 2)
		elif 2 + self.maxaxes <= channel < 2 + self.maxaxes + self.maxextruders:
			self.write_extruder (channel - 2 - self.maxaxes)
		else:
			self.write_temp (channel - 2 - self.maxaxes - self.maxextruders)
	# }}}
	def write_all (self): # {{{
		for i in range (1, 2 + self.maxaxes + self.maxextruders + self.maxtemps):
			print ('(4) writing %d' % i)
			self.write (i)
	# }}}
	def pause (self, pausing = True): # {{{
		self.send_packet (struct.pack ('<BB', self.command['PAUSE'], pausing))
	# }}}
	def ping (self, arg = 0): # {{{
		self.send_packet (struct.pack ('<BB', self.command['PING'], arg))
		assert struct.unpack ('<BB', self.recv_packet ()) == (ord (self.rcommand['PONG']), arg)
	# }}}
	def play (self, data): # {{{
		self.send_packet (struct.pack ('<BQ', self.command['PLAY'], len (data) - len (data) % 32))
		self.printer.write (data[:60])
		p = 60
		r = self.printer.read (1)
		if r != Printer.single['INIT']:
			dprint ('huh?', r)
			return
		while p + 30 <= len (data):
			# Send fragment
			self.printer.write (data[p:p+30])
			p += 30
			# Wait for ack
			r = self.printer.read (1)
			if r != Printer.single['INIT']:
				dprint ('huh?', r)
				break
	# }}}
	# }}}
# }}}