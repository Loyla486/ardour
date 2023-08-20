/*
 * Copyright (C) 2016-2018 Paul Davis <paul@linuxaudiosystems.com>
 * Copyright (C) 2017-2018 Robin Gareus <robin@gareus.org>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#include <algorithm>
#include <bitset>

#include <stdlib.h>
#include <pthread.h>

#include "pbd/compose.h"
#include "pbd/convert.h"
#include "pbd/debug.h"
#include "pbd/failed_constructor.h"
#include "pbd/file_utils.h"
#include "pbd/search_path.h"
#include "pbd/enumwriter.h"

#include "midi++/parser.h"

#include "temporal/time.h"
#include "temporal/bbt_time.h"

#include "ardour/amp.h"
#include "ardour/async_midi_port.h"
#include "ardour/audioengine.h"
#include "ardour/debug.h"
#include "ardour/midiport_manager.h"
#include "ardour/midi_track.h"
#include "ardour/midi_port.h"
#include "ardour/session.h"
#include "ardour/tempo.h"
#include "ardour/types_convert.h"

#include "gtkmm2ext/gui_thread.h"
#include "gtkmm2ext/rgb_macros.h"

#include "gtkmm2ext/colors.h"

#include "gui.h"
#include "lppro.h"

#include "pbd/i18n.h"

#ifdef PLATFORM_WINDOWS
#define random() rand()
#endif

using namespace ARDOUR;
using namespace PBD;
using namespace Glib;
using namespace ArdourSurface;
using namespace Gtkmm2ext;

#include "pbd/abstract_ui.cc" // instantiate template

#define NOVATION          0x1235
#define LAUNCHPADPROMK3   0x0123
static const std::vector<MIDI::byte> sysex_header ({ 0xf0, 0x00, 0x20, 0x29, 0x2, 0xe });

const LaunchPadPro::PadID LaunchPadPro::all_pad_ids[] = {
	Shift, Left, Right, Session, Note, Chord, Custom, Sequencer, Projects,
	Patterns, Steps, PatternSettings, Velocity, Probability, Mutation, MicroStep, PrintToClip,
	StopClip, Device, Sends, Pan, Volume, Solo, Mute, RecordArm,
	CaptureMIDI, Play, FixedLength, Quantize, Duplicate, Clear, Down, Up,
	Lower1, Lower2, Lower3, Lower4, Lower5, Lower6, Lower7, Lower8,
};

const LaunchPadPro::Layout LaunchPadPro::AllLayouts[] = {
	SessionLayout, Fader, ChordLayout, CustomLayout, NoteLayout, Scale, SequencerSettings,
	SequencerSteps, SequencerVelocity, SequencerPatternSettings, SequencerProbability, SequencerMutation,
	SequencerMicroStep, SequencerProjects, SequencerPatterns, SequencerTempo, SequencerSwing, ProgrammerLayout, Settings, CustomSettings
};

bool
LaunchPadPro::available ()
{
	/* no preconditions other than the device being present */
	return true;
}

bool
LaunchPadPro::match_usb (uint16_t vendor, uint16_t device)
{
	return vendor == NOVATION && device == LAUNCHPADPROMK3;
}

bool
LaunchPadPro::probe (std::string& i, std::string& o)
{
	vector<string> midi_inputs;
	vector<string> midi_outputs;
	AudioEngine::instance()->get_ports ("", DataType::MIDI, PortFlags (IsOutput|IsTerminal), midi_inputs);
	AudioEngine::instance()->get_ports ("", DataType::MIDI, PortFlags (IsInput|IsTerminal), midi_outputs);

	auto has_lppro = [](string const& s) {
		std::string pn = AudioEngine::instance()->get_hardware_port_name_by_name (s);
		return pn.find ("Launchpad Pro MK3 MIDI 1") != string::npos;
	};

	auto pi = std::find_if (midi_inputs.begin (), midi_inputs.end (), has_lppro);
	auto po = std::find_if (midi_outputs.begin (), midi_outputs.end (), has_lppro);

	if (pi == midi_inputs.end () || po == midi_outputs.end ()) {
		return false;
	}

	i = *pi;
	o = *po;
	return true;
}

LaunchPadPro::LaunchPadPro (ARDOUR::Session& s)
	: MIDISurface (s, X_("Novation Launchpad Pro"), X_("Launchpad Pro"), false)
	, _daw_out_port (nullptr)
	, _gui (nullptr)
	, _current_layout (SessionLayout)
{
	run_event_loop ();
	port_setup ();

	std::string  pn_in, pn_out;
	if (probe (pn_in, pn_out)) {
		_async_in->connect (pn_in);
		_async_out->connect (pn_out);
	}

	connect_daw_ports ();

	build_pad_map ();
	build_layout_maps ();
}

LaunchPadPro::~LaunchPadPro ()
{
	DEBUG_TRACE (DEBUG::Launchpad, "push2 control surface object being destroyed\n");

	stop_event_loop ();

	MIDISurface::drop ();
}

int
LaunchPadPro::set_active (bool yn)
{
	DEBUG_TRACE (DEBUG::Launchpad, string_compose("Launchpad Pro::set_active init with yn: %1\n", yn));

	if (yn == active()) {
		return 0;
	}

	if (yn) {

		if (device_acquire ()) {
			return -1;
		}

	} else {
		/* Control Protocol Manager never calls us with false, but
		 * insteads destroys us.
		 */
	}

	ControlProtocol::set_active (yn);

	DEBUG_TRACE (DEBUG::Launchpad, string_compose("Launchpad Pro::set_active done with yn: '%1'\n", yn));

	return 0;
}

void
LaunchPadPro::run_event_loop ()
{
	DEBUG_TRACE (DEBUG::Launchpad, "start event loop\n");
	BaseUI::run ();
}

void
LaunchPadPro::stop_event_loop ()
{
	DEBUG_TRACE (DEBUG::Launchpad, "stop event loop\n");
	BaseUI::quit ();
}

int
LaunchPadPro::begin_using_device ()
{
	DEBUG_TRACE (DEBUG::Launchpad, "begin using device\n");

	connect_to_port_parser (*_daw_in_port);

	/* Connect DAW input port to event loop */

	AsyncMIDIPort* asp;

	asp = dynamic_cast<AsyncMIDIPort*> (_daw_in_port);
	asp->xthread().set_receive_handler (sigc::bind (sigc::mem_fun (this, &MIDISurface::midi_input_handler), _daw_in_port));
	asp->xthread().attach (main_loop()->get_context());

	/* request current layout */
	MidiByteArray msg (sysex_header);
	msg.push_back (0x0);
	msg.push_back (0xf7);
	write (msg);

	set_device_mode (DAW);
	set_layout (SessionLayout);

	/* catch current selection, if any so that we can wire up the pads if appropriate */
	stripable_selection_changed ();

	return MIDISurface::begin_using_device ();
}

int
LaunchPadPro::stop_using_device ()
{
	DEBUG_TRACE (DEBUG::Launchpad, "stop using device\n");

	if (!_in_use) {
		DEBUG_TRACE (DEBUG::Launchpad, "nothing to do, device not in use\n");
		return 0;
	}

	set_device_mode (Standalone);

	return MIDISurface::stop_using_device ();
}

XMLNode&
LaunchPadPro::get_state() const
{
	XMLNode& node (MIDISurface::get_state());

	XMLNode* child = new XMLNode (X_("DAWInput"));
	child->add_child_nocopy (_daw_in->get_state());
	node.add_child_nocopy (*child);
	child = new XMLNode (X_("DAWOutput"));
	child->add_child_nocopy (_daw_out->get_state());
	node.add_child_nocopy (*child);

	return node;
}

int
LaunchPadPro::set_state (const XMLNode & node, int version)
{
	DEBUG_TRACE (DEBUG::Launchpad, string_compose ("LaunchPadPro::set_state: active %1\n", active()));

	int retval = 0;

	if (MIDISurface::set_state (node, version)) {
		return -1;
	}

	return retval;
}

std::string
LaunchPadPro::input_port_name () const
{
#ifdef __APPLE__
	/* the origin of the numeric magic identifiers is known only to Novation
	   and may change in time. This is part of how CoreMIDI works.
	*/
	return X_("system:midi_capture_1319078870");
#else
	return X_("Launchpad Pro MK3 MIDI 1");
#endif
}

std::string
LaunchPadPro::input_daw_port_name () const
{
#ifdef __APPLE__
	/* the origin of the numeric magic identifiers is known only to Novation
	   and may change in time. This is part of how CoreMIDI works.
	*/
	return X_("system:midi_capture_1319078870");
#else
	return X_("Launchpad Pro MK3 MIDI 3");
#endif
}

std::string
LaunchPadPro::output_port_name () const
{
#ifdef __APPLE__
	/* the origin of the numeric magic identifiers is known only to Novation
	   and may change in time. This is part of how CoreMIDI works.
	*/
	return X_("system:midi_playback_3409210341");
#else
	return X_("Launchpad Pro MK3 MIDI 1");
#endif
}

std::string
LaunchPadPro::output_daw_port_name () const
{
#ifdef __APPLE__
	/* the origin of the numeric magic identifiers is known only to Novation
	   and may change in time. This is part of how CoreMIDI works.
	*/
	return X_("system:midi_playback_3409210341");
#else
	return X_("Launchpad Pro MK3 MIDI 3");
#endif
}

void
LaunchPadPro::stripable_selection_changed ()
{
}

void
LaunchPadPro::build_pad_map ()
{
#define EDGE_PAD(id) if (!(pad_map.insert (make_pair<int,Pad> ((id),  Pad ((id)))).second)) abort()

	EDGE_PAD (Shift);

	EDGE_PAD (Left);
	EDGE_PAD (Right);
	EDGE_PAD (Session);
	EDGE_PAD (Note);
	EDGE_PAD (Chord);
	EDGE_PAD (Custom);
	EDGE_PAD (Sequencer);
	EDGE_PAD (Projects);

	EDGE_PAD (Patterns);
	EDGE_PAD (Steps);
	EDGE_PAD (PatternSettings);
	EDGE_PAD (Velocity);
	EDGE_PAD (Probability);
	EDGE_PAD (Mutation);
	EDGE_PAD (MicroStep);
	EDGE_PAD (PrintToClip);

	EDGE_PAD (StopClip);
	EDGE_PAD (Device);
	EDGE_PAD (Sends);
	EDGE_PAD (Pan);
	EDGE_PAD (Volume);
	EDGE_PAD (Solo);
	EDGE_PAD (Mute);
	EDGE_PAD (RecordArm);

	EDGE_PAD (CaptureMIDI);
	EDGE_PAD (Play);
	EDGE_PAD (FixedLength);
	EDGE_PAD (Quantize);
	EDGE_PAD (Duplicate);
	EDGE_PAD (Clear);
	EDGE_PAD (Down);
	EDGE_PAD (Up);

	EDGE_PAD (Lower1);
	EDGE_PAD (Lower2);
	EDGE_PAD (Lower3);
	EDGE_PAD (Lower4);
	EDGE_PAD (Lower5);
	EDGE_PAD (Lower6);
	EDGE_PAD (Lower7);
	EDGE_PAD (Lower8);

	/* Now add the 8x8 central pad grid */

	for (int row = 0; row < 8; ++row) {
		for (int col = 0; col < 8; ++col) {
			int pid = (11 + (row * 10)) + col;
			std::pair<int,Pad> p (pid, Pad (pid, row, col));
			if (!pad_map.insert (p).second) abort();
		}
	}

	/* The +1 is for the shift pad at upper left */
	assert (pad_map.size() == (64 + (5 * 8) + 1));
}

LaunchPadPro::Pad*
LaunchPadPro::pad_by_id (int pid)
{
	PadMap::iterator p = pad_map.find (pid);
	if (p == pad_map.end()) {
		return nullptr;
	}
	return &p->second;
}

void
LaunchPadPro::light_pad (int pad_id, int color, Pad::ColorMode mode)
{
	Pad* pad = pad_by_id (pad_id);
	if (!pad) {
		return;
	}
	pad->set (color, mode);
	daw_write (pad->state_msg());
}

void
LaunchPadPro::pad_off (int pad_id)
{
	Pad* pad = pad_by_id (pad_id);
	if (!pad) {
		return;
	}
	pad->set (0, Pad::Static);
	daw_write (pad->state_msg());
}

void
LaunchPadPro::all_pads_off ()
{
	MidiByteArray msg (sysex_header);
	msg.reserve (msg.size() + (106 * 3) + 3);
	msg.push_back (0x3);
	for (size_t n = 1; n < 32; ++n) {
		msg.push_back (0x0);
		msg.push_back (n);
		msg.push_back (13);
	}
	msg.push_back (0xf7);
	daw_write (msg);
}

void
LaunchPadPro::all_pads_on (int color)
{
	MidiByteArray msg (sysex_header);
	msg.push_back (0xe);
	msg.push_back (color & 0x7f);
	msg.push_back (0xf7);
	daw_write (msg);

#if 0
	for (PadMap::iterator p = pad_map.begin(); p != pad_map.end(); ++p) {
		Pad& pad (p->second);
		pad.set (random() % color_map.size(), Pad::Static);
		daw_write (pad.state_msg());
	}
#endif
}

void
LaunchPadPro::set_layout (Layout l, int page)
{
	MidiByteArray msg (sysex_header);
	msg.push_back (0x0);
	msg.push_back (l);
	msg.push_back (page);
	msg.push_back (0x0);
	msg.push_back (0xf7);
	std::cerr << "switch to layout " << l << " using " << msg << std::endl;
	daw_write (msg);
}

void
LaunchPadPro::set_device_mode (DeviceMode m)
{
	/* LP Pro MK3 programming manual, pages 14 and 18 */
	MidiByteArray standalone_or_daw (sysex_header);
	MidiByteArray live_or_programmer (sysex_header);

	switch (m) {
	case Standalone:
		std::cerr << "entering standalone mode\n";
		live_or_programmer.push_back (0xe);
		live_or_programmer.push_back (0x0);
		live_or_programmer.push_back (0xf7);
		/* Back to "live" state */
		write (live_or_programmer);
		g_usleep (100000);
		/* disable "daw" mode */
		standalone_or_daw.push_back (0x10);
		standalone_or_daw.push_back (0x0);
		standalone_or_daw.push_back (0xf7);
		write (standalone_or_daw);
		break;

	case DAW:
		// live_or_programmer.push_back (0xe);
		// live_or_programmer.push_back (0x0);
		// live_or_programmer.push_back (0xf7);
		/* Back to "live" state */
		// daw_write (live_or_programmer);
		// g_usleep (100000);
		/* Enable DAW mode */
		standalone_or_daw.push_back (0x10);
		standalone_or_daw.push_back (0x1);
		standalone_or_daw.push_back (0xf7);
		write (standalone_or_daw);
		break;

	case Programmer:
		std::cerr << "entering programmer mode\n";
		live_or_programmer.push_back (0xe);
		live_or_programmer.push_back (0x1);
		live_or_programmer.push_back (0xf7);
		/* enter "programmer" state */
		write (live_or_programmer);
		break;
	}
}

void
LaunchPadPro::handle_midi_sysex (MIDI::Parser&, MIDI::byte* raw_bytes, size_t sz)
{
	DEBUG_TRACE (DEBUG::Launchpad, string_compose ("Sysex, %1 bytes\n", sz));

	if (sz < sysex_header.size() + 1) {
		return;
	}

	const size_t num_layouts = sizeof (AllLayouts) / sizeof (AllLayouts[0]);

	raw_bytes += sysex_header.size();

	switch (raw_bytes[0]) {
	case 0x0: /* layout info */
		if (sz < sysex_header.size() + 2) {
			return;
		}

		if (raw_bytes[1] < num_layouts) {
			_current_layout = AllLayouts[raw_bytes[1]];
			std::cerr << "Current layout = " << _current_layout << std::endl;
		} else {
			std::cerr << "ignore illegal layout index " << (int) raw_bytes[1] << std::endl;
		}
		break;
	default:
		break;
	}
}

void
LaunchPadPro::handle_midi_controller_message (MIDI::Parser&, MIDI::EventTwoBytes* ev)
{
	DEBUG_TRACE (DEBUG::Launchpad, string_compose ("CC %1 (value %2)\n", (int) ev->controller_number, (int) ev->value));

}

void
LaunchPadPro::handle_midi_note_on_message (MIDI::Parser& parser, MIDI::EventTwoBytes* ev)
{
	if (ev->velocity == 0) {
		handle_midi_note_off_message (parser, ev);
		return;
	}

	DEBUG_TRACE (DEBUG::Launchpad, string_compose ("Note On %1/0x%3%4%5 (velocity %2)\n", (int) ev->note_number, (int) ev->velocity, std::hex, (int) ev->note_number, std::dec));
	std::pair<int,int> coord = note_to_xy (ev->note_number);
	std::cerr << "x: " << coord.first << " y: " << coord.second << std::endl;
}

void
LaunchPadPro::handle_midi_note_off_message (MIDI::Parser&, MIDI::EventTwoBytes* ev)
{
	DEBUG_TRACE (DEBUG::Launchpad, string_compose ("Note Off %1 (velocity %2)\n", (int) ev->note_number, (int) ev->velocity));

}

void
LaunchPadPro::port_registration_handler ()
{
	MIDISurface::port_registration_handler ();
	connect_daw_ports ();
}

void
LaunchPadPro::connect_daw_ports ()
{
	if (!_daw_in || !_daw_out) {
		/* ports not registered yet */
		std::cerr << "no daw port registered\n";
		return;
	}

	if (_daw_in->connected() && _daw_out->connected()) {
		/* don't waste cycles here */
		std::cerr << "daw port already connected\n";
		return;
	}

	std::vector<std::string> in;
	std::vector<std::string> out;

	AudioEngine::instance()->get_ports (string_compose (".*%1", input_daw_port_name()), DataType::MIDI, PortFlags (IsPhysical|IsOutput), in);
	AudioEngine::instance()->get_ports (string_compose (".*%1", output_daw_port_name()), DataType::MIDI, PortFlags (IsPhysical|IsInput), out);

	if (!in.empty() && !out.empty()) {
		if (!_daw_in->connected()) {
			AudioEngine::instance()->connect (_daw_in->name(), in.front());
		}
		if (!_daw_out->connected()) {
			AudioEngine::instance()->connect (_daw_out->name(), out.front());
		}
	}
}

int
LaunchPadPro::ports_acquire ()
{
	int ret = MIDISurface::ports_acquire ();

	if (!ret) {
		_daw_in  = AudioEngine::instance()->register_input_port (DataType::MIDI, string_compose (X_("%1 daw in"), port_name_prefix), true);
		if (_daw_in) {
			_daw_in_port = std::dynamic_pointer_cast<AsyncMIDIPort>(_daw_in).get();
			_daw_out = AudioEngine::instance()->register_output_port (DataType::MIDI, string_compose (X_("%1 daw out"), port_name_prefix), true);
		}
		if (_daw_out) {
			_daw_out_port = std::dynamic_pointer_cast<AsyncMIDIPort>(_async_out).get();
			return 0;
		}

		ret = -1;
	}

	return ret;
}

void
LaunchPadPro::ports_release ()
{
	/* wait for button data to be flushed */
	MIDI::Port* daw_port = std::dynamic_pointer_cast<AsyncMIDIPort>(_daw_out).get();
	AsyncMIDIPort* asp;
	asp = dynamic_cast<AsyncMIDIPort*> (daw_port);
	asp->drain (10000, 500000);

	{
		Glib::Threads::Mutex::Lock em (AudioEngine::instance()->process_lock());
		AudioEngine::instance()->unregister_port (_daw_in);
		AudioEngine::instance()->unregister_port (_daw_out);
	}

	_daw_in.reset ((ARDOUR::Port*) 0);
	_daw_out.reset ((ARDOUR::Port*) 0);

	MIDISurface::ports_release ();
}

void
LaunchPadPro::daw_write (const MidiByteArray& data)
{
	/* immediate delivery */
	_daw_out_port->write (&data[0], data.size(), 0);
}

void
LaunchPadPro::daw_write (MIDI::byte const * data, size_t size)
{
	_daw_out_port->write (data, size, 0);
}

void
LaunchPadPro::scroll_text (std::string const & txt, int color, bool loop, float speed)
{
	MidiByteArray msg (sysex_header);

	msg.push_back (0x32);
	msg.push_back (color);
	msg.push_back (loop ? 1: 0);

	for (std::string::size_type i = 0; i < txt.size(); ++i) {
		msg.push_back (txt[i] & 0xf7);
	}

	msg.push_back (0xf7);
	daw_write (msg);

	if (speed != 0.f) {
		msg[sysex_header.size() + 3] = (MIDI::byte) (floor (1.f + (speed * 6.f)));
		msg[sysex_header.size() + 4] = 0xf7;
		msg.resize (sysex_header.size() + 5);
		daw_write (msg);
	}
}

void
LaunchPadPro::build_layout_maps ()
{
	static const XY_Note_Map maps[] = {
		{ /* Session */
			{ 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58 },
			{ 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e },
			{ 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44 },
			{ 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a },
			{ 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30 },
			{ 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 },
			{ 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c },
			{ 0xb,  0xc,  0xe,  0xd,  0xf,  0x10, 0x11, 0x12 }
		},
#if 0
		{ /*  Fader */
			{ 0x0, 0x15, 0x2a, 0x3f, 0x40, 0x55, 0x6a, 0x7f },
			{ 0x0, 0x15, 0x2a, 0x3f, 0x40, 0x55, 0x6a, 0x7f },
			{ 0x0, 21, 42, 63, 64, 85, 106, 127 },
			{ 0x0, 21, 42, 63, 64, 85, 106, 127 },
			{ 0x0, 0x11, 0x22, 0x34, 0x46, 0x59, 0x6c, 0x7f },
			{ 0x0, 0x11, 0x22, 0x34, 0x46, 0x59, 0x6c, 0x7f },
			{ 0x0, 17, 34, 52, 70, 89, 108, 127 },
			{ 0x0, 17, 34, 52, 70, 89, 108, 127 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Vertical Fader */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  Programmer */

			{ 0xb,  0xc,  0xd,  0xe,  0xf,  0x10, 0x11, 0x12 },
			{ 0x15, 0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c },
			{ 0x1f, 0x20, 0x21, 0x22, 0x23, 0x24, 0x25, 0x26 },
			{ 0x29, 0x2a, 0x2b, 0x2c, 0x2d, 0x2e, 0x2f, 0x30 },
			{ 0x33, 0x34, 0x35, 0x36, 0x37, 0x38, 0x39, 0x3a },
			{ 0x3d, 0x3e, 0x3f, 0x40, 0x41, 0x42, 0x43, 0x44 },
			{ 0x47, 0x48, 0x49, 0x4a, 0x4b, 0x4c, 0x4d, 0x4e },
			{ 0x51, 0x52, 0x53, 0x54, 0x55, 0x56, 0x57, 0x58 }
		},
		{ /*  Settings */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		},
		{ /*  CustomSettings */
			{ 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }, { 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0, 0x0 }
		}
#endif
	};

	const size_t num_layouts = sizeof (maps) / sizeof (maps[0]);
	std::cerr << "We got " << num_layouts << " layouts\n";

	layout_xy_note_map.resize (num_layouts * 64);

	for (size_t layout = 0; layout < num_layouts; ++layout) {
		for (int row = 0; row < 8; ++row) {
			for (int col = 0; col < 8; ++col) {
				layout_xy_note_map[xy_note_index (layout, col, row)] = maps[layout][row][col];
				// std::cerr << "IN col " << col << " row " << row << " note 0x" << std::hex << maps[layout][row][col] << std::dec << std::endl;
			}
		}
	}

	layout_note_xy_map.resize (128 * num_layouts);

	for (size_t layout = 0; layout < num_layouts; ++layout) {
		for (int row = 0; row < 8; ++row) {
			for (int col = 0; col < 8; ++col) {
				const int note = layout_xy_note_map[xy_note_index (layout, col, row)];
				std::cerr << "OUT layout " << layout << " col " << col << " row " << row << " note " << note << std::endl;
				layout_note_xy_map[note_xy_index(layout,note)] = row*8 + col;
			}
		}
	}
}

int
LaunchPadPro::note_xy_index (int layout, int note) const
{
	return (layout * 127) + note;
}

int
LaunchPadPro::xy_note_index (int layout, int col, int row) const
{
	return (layout * 64) + (row * 8) + col;
}

std::pair<int, int>
LaunchPadPro::note_to_xy (int note) const
{
	int coord = layout_note_xy_map[note_xy_index ((int) _current_layout, note)];
	return std::pair<int,int> (coord % 8, coord / 8);
}

LaunchPadPro::StripableSlot
LaunchPadPro::get_stripable_slot (int x, int y) const
{
	x += scroll_x_offset;
	y += scroll_y_offset;

	if ((StripableSlotColumn::size_type) x > stripable_slots.size()) {
		return StripableSlot (-1, -1);
	}

	if ((StripableSlotRow::size_type) y > stripable_slots[x].size()) {
		return StripableSlot (-1, -1);
	}

	return stripable_slots[x][y];
}