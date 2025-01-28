/*
 * JackTransport.cpp - support for synchronization with jack transport
 *
 * This file is part of LMMS - https://lmms.io
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program (see COPYING); if not, write to the
 * Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor,
 * Boston, MA 02110-1301 USA.
 *
 */

#include "JackTransport.h"

#ifdef LMMS_HAVE_JACK

#ifndef LMMS_HAVE_WEAKJACK
#include <jack/jack.h>
#else
#include <weak_libjack.h>
#endif

#include <chrono>
#include <thread>

#include "Engine.h"
#include "Song.h"

namespace lmms 
{


/* -----------------------ExSync private --------------------------- */

/**
	Model controled by user interface 
	using View/Controller in SongEditor
	(include/SongEditor.h, src/gui/editors/SongEditor.cpp)
 */ 


/* class SyncHook && class SyncCtl: private part */


static bool s_SyncFollow = false;
static bool s_SyncLead = true;
static bool s_SyncOn = false;
static SyncCtl::SyncMode s_SyncMode = SyncCtl::Leader; // (for ModeButton state)


static void lmmsSyncMode(bool playing)
{
	auto lSong = Engine::getSong();

	if ((! lSong->isExporting()) && s_SyncOn && (lSong->isPlaying() != playing)) 
	{
		if ( lSong->isStopped() ) { lSong->playSong(); } else {	lSong->togglePause(); }
	}
}


static void lmmsSyncPosition(uint32_t frames)
{
	auto lSong = Engine::getSong();

	if ((! lSong->isExporting()) && s_SyncOn && (lSong->playMode()  == Song::PlayMode::Song))
	{
		lSong->setToTime(TimePos::fromFrames(frames , Engine::framesPerTick()));
	}
}


/* Jack Transport implementation (public part): */

static jack_client_t * s_syncJackd = nullptr; //!< Set by Jack audio 
static bool s_threadOn = true;
static int s_pulsePeriod = 1000;


static void s_pulseFunction(int ms)
 {
	s_pulsePeriod = ms;
 	fprintf(stderr, "DEBUG::pulse_function started\n"); // DEBUG
	while (s_threadOn)
 	{
		if (s_syncJackd) { SyncHook::pulse(); }
		std::this_thread::sleep_for(std::chrono::milliseconds(s_pulsePeriod));
 	}
 	fprintf(stderr, "DEBUG::pulse_function stopped\n"); // DEBUG
}


static std::thread s_pulseThread(s_pulseFunction, 50);


void syncJackd(jack_client_t* client)
{
	s_syncJackd = client;
}




class JackTransport
{
public:
	static bool On();
	static void Start();
	static void Stop();
	static void Jump(f_cnt_t frame);
	static void Follow(bool set);
	static bool Stopped();
};

/* class SyncHook: public part */


static f_cnt_t s_lastFrame = 0; // Save last frame position to catch change
void SyncHook::pulse()
{
	auto lSong = Engine::getSong();
	f_cnt_t lFrame = 0;
	if (s_SyncFollow && JackTransport::Stopped()) 
	{
		lmmsSyncMode(false); 
	}
	if (lSong->isStopped())
	{
		lFrame = lSong->getFrames();
		if (s_SyncLead && s_SyncOn && (lFrame != s_lastFrame) )
		{
			s_lastFrame = lFrame;
			jump();
		}
	}
}


void SyncHook::pulseStop() //!< Placed in  AudioEngine destructor (AudioRngine.cpp)
{
	s_threadOn = false;
	s_pulseThread.join();
}


void SyncHook::jump()
{
	auto lSong = Engine::getSong();
	if ((! lSong->isExporting()) && s_SyncLead && s_SyncOn)
	{
		JackTransport::Jump(lSong->getFrames());
	}
}


void SyncHook::start()
{
	if (s_SyncOn)
	{
		JackTransport::Start();
		if( SyncCtl::Leader == s_SyncMode) { jump(); }
	}
}


void SyncHook::stop()
{
	if (s_SyncOn)
	{
		JackTransport::Stop();
		if( SyncCtl::Leader == s_SyncMode) { jump(); }
	}
}


/* class SyncCtl: public part */


SyncCtl::SyncMode SyncCtl::toggleMode()
{
	if ( !JackTransport::On() ) 
	{
		return s_SyncMode;
	}
	// Make state change (Master -> Slave -> Duplex -> Master -> ...)
	switch(s_SyncMode)
	{
	case Duplex: // Duplex -> Leader
		s_SyncMode = Leader;
		break;
	case Leader: // Leader -> Follower
		s_SyncMode = Follower;
		break;
	case Follower: // Follower -> Duplex
		s_SyncMode = Duplex;
		break;
	default: // never happens, but our compiler want this
		s_SyncMode = Leader;
	}
	setMode(s_SyncMode);
	return s_SyncMode;
}


void SyncCtl::setMode(SyncCtl::SyncMode mode)
{
	if ( !JackTransport::On() ) 
	{
		return;
	}
	switch(mode)
	{
	case Leader:
		s_SyncFollow = false;
		s_SyncLead = true;
		JackTransport::Follow(false);
		break;
	case Follower:
		s_SyncFollow = true;
		s_SyncLead = false;
		JackTransport::Follow(true);
		break;
	case Duplex:
		s_SyncFollow = true;
		s_SyncLead = true;
		JackTransport::Follow(true);
		break;
	default:
		s_SyncOn = false; // turn Off 
	}
}


SyncCtl::SyncMode SyncCtl::getMode()
{
	return s_SyncMode;
}


bool SyncCtl::toggleOnOff()
{
	if ( JackTransport::On() ) 
	{
		if (s_SyncOn) {	s_SyncOn = false; } else { s_SyncOn = true; }
	} else {
		s_SyncOn = false;
	}
	return s_SyncOn;
}


bool SyncCtl::have()
{
	return JackTransport::On();
}





// Communication with jack transport:





/*! Function adapt events from Jack Transport to LMMS  */
static int syncCallBack(jack_transport_state_t state, jack_position_t *pos, void *arg)
{
	if (s_SyncFollow)
	{
		switch(state)
		{
		case JackTransportStopped:
			lmmsSyncMode(false);
			lmmsSyncPosition(pos->frame);
			break;
		case JackTransportStarting:
			lmmsSyncMode(true);
			lmmsSyncPosition(pos->frame);
			break;
		case JackTransportRolling: // mostly not called with this state
			lmmsSyncMode(true);
			lmmsSyncPosition(pos->frame);
			break;
		default:
			; // not use JackTransportLooping  and JackTransportNetStarting enum
		}
	}
	return 1; 
}


/* Class JackTransport implementation : */

bool JackTransport::On()
{
	if (s_syncJackd) { return true; } else { return false; }
}


void JackTransport::Start()
{
	if (s_syncJackd) { jack_transport_start(s_syncJackd); }
}


void JackTransport::Stop()
{
	if (s_syncJackd) { jack_transport_stop(s_syncJackd); }
}


void JackTransport::Jump(f_cnt_t frame)
{
	if (s_syncJackd) { jack_transport_locate(s_syncJackd, frame); }
}


void JackTransport::Follow(bool set)
{
	if (s_syncJackd)
	{
		if (set)
		{
			jack_set_sync_callback(s_syncJackd, &syncCallBack, nullptr);
		} else {
			jack_set_sync_callback(s_syncJackd, nullptr, nullptr);
		}
	}
}


static jack_transport_state_t s_lastJackState = JackTransportStopped;
bool JackTransport::Stopped()
{
	bool justStopped = false;

	if (s_syncJackd)
	{ 
		jack_transport_state_t state = jack_transport_query(s_syncJackd, nullptr);
		if ((JackTransportStopped == state) && (state != s_lastJackState))
		{
			justStopped = true;
		}
		s_lastJackState = state;
	} else {
		s_lastJackState = JackTransportStopped;
	}

	return justStopped;
}




} // namespace lmms 

#endif // LMMS_HAVE_JACK

