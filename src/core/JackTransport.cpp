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


#include "Engine.h"
#include "Song.h"

namespace lmms 
{


/* -----------------------ExSync private --------------------------- */

/**
	 Functions to control LMMS position/playing in Slave || Duplex
	 LMMS react only in if ExSync is on (button is green)
	 MUST be provided by ExSync driver
	 
	 External code MUST NOT use this functions: this is adapter
	 from external device events to LMMS (so not included in ExSync.h)
*/
struct ExSyncCallbacks
{
	//! @playing [true : to start; false : to pause] 
	void (* mode)(bool playing); 
	//! change position to @frames;
	void (* position)(uint32_t frames);
	//! to calculate frames from time (not used here - jack is working in frames)
	sample_rate_t (* processingSampleRate)();
};

static struct ExSyncCallbacks * getFollowerCallBacks();


static jack_client_t * s_syncJackd = nullptr; //!< Set by Jack audio 
static jack_transport_state_t s_lastJackState = JackTransportStopped;


/*! Function adapt events from Jack Transport to LMMS  */
static int syncCallBack(jack_transport_state_t state, jack_position_t *pos, void *arg)
{
	struct ExSyncCallbacks *slaveCallBacks  = getFollowerCallBacks();
	// Now slaveCallBacks is local copy - never be changed by other thread ...
	if (slaveCallBacks)
	{
		switch(state)
		{
		case JackTransportStopped:
			slaveCallBacks->mode(false);
			slaveCallBacks->position(pos->frame);
			break;
		case JackTransportStarting:
			slaveCallBacks->mode(true);
			slaveCallBacks->position(pos->frame);
			break;
		case JackTransportRolling: //!< mostly not called with this state
			slaveCallBacks->mode(true);
			slaveCallBacks->position(pos->frame);
			break;
		default:
			; // not use JackTransportLooping  and JackTransportNetStarting enum
		}
	}
	return 1; 
}


/* Functions needed  to control Jack Transport (adapt events from LMMS) */


static bool jackAvailable()
{
	if (s_syncJackd) { return true; } else { return false; }
}


static void jackPlay(bool playing)
{
	if (s_syncJackd)
	{
		if (playing) {
			jack_transport_start(s_syncJackd);
		} else {
			jack_transport_stop(s_syncJackd);
		}
	}
}


static void jackPosition(f_cnt_t frame)
{
	if (s_syncJackd)
	{
		jack_transport_locate(s_syncJackd, frame);
	}
}

static bool jackStopped()
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


static void jackSlave(bool set)
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



/**
	Model controled by user interface 
	using View/Controller in SongEditor
	(include/SongEditor.h, src/gui/editors/SongEditor.cpp)
 */ 

static struct ExSyncCallbacks *s_followerCallBacks= nullptr;

static struct ExSyncCallbacks * getFollowerCallBacks() {return s_followerCallBacks; }


/* class ExSyncHook && class ExSyncCtl: private part */


static bool s_SyncFollow = false; //!< (Receave)
static bool s_SyncLead = true; //!< (Send)
static bool s_SyncOn = false; //!< (React and Send)
static SyncCtl::SyncMode s_SyncMode = SyncCtl::Leader; //!< (for ModeButton state)


static void exSyncMode(bool playing)
{
	auto lSong = Engine::getSong();

	if ((! lSong->isExporting()) && s_SyncOn && (lSong->isPlaying() != playing)) 
	{
		if ( lSong->isStopped() ) { lSong->playSong(); } else {	lSong->togglePause(); }
	}
}


static void exSyncPosition(uint32_t frames)
{
	auto lSong = Engine::getSong();

	if ((! lSong->isExporting()) && s_SyncOn && (lSong->playMode()  == Song::PlayMode::Song))
	{
		lSong->setToTime(TimePos::fromFrames(frames , Engine::framesPerTick()));
	}
}


static sample_rate_t exSyncSampleRate()
{
	return Engine::audioEngine()->outputSampleRate();
}


//! Function used by internal code to send messages to LMMS::Song from
//! external device (in Slave , Duplex modes)
static struct ExSyncCallbacks s_SyncCallbacks = {
	&exSyncMode,
	&exSyncPosition,
	&exSyncSampleRate
};


/* Jack Transport implementation (public part): */


void syncJackd(jack_client_t* client)
{
	s_syncJackd = client;
}


/* class SyncHook: public part */


static f_cnt_t s_lastFrame = 0; // Save last frame position to catch change
void SyncHook::pulse()
{
	struct ExSyncCallbacks *slaveCallBacks  = getFollowerCallBacks();
	auto lSong = Engine::getSong();
	f_cnt_t lFrame = 0;
	if (slaveCallBacks && jackStopped()) 
	{ 
		slaveCallBacks->mode(false); 
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


void SyncHook::jump()
{
	auto lSong = Engine::getSong();
	if ((! lSong->isExporting()) && s_SyncLead && s_SyncOn)
	{
		jackPosition(lSong->getFrames());
	}
}


void SyncHook::start()
{
	if (s_SyncOn)
	{
		jackPlay(true);
		if( SyncCtl::Leader == s_SyncMode) { jump(); }
	}
}


void SyncHook::stop()
{
	if (s_SyncOn)
	{
		jackPlay(false);
		if( SyncCtl::Leader == s_SyncMode) { jump(); }
	}
}


/* class SyncCtl: public part */


SyncCtl::SyncMode SyncCtl::toggleMode()
{
	if ( !jackAvailable() ) 
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
	if ( !jackAvailable() ) 
	{
		return;
	}
	switch(mode)
	{
	case Leader:
		s_SyncFollow = false;
		s_SyncLead = true;
		jackSlave(false);
		s_followerCallBacks= nullptr;
		break;
	case Follower:
		s_SyncFollow = true;
		s_SyncLead = false;
		jackSlave(true);
		s_followerCallBacks= &s_SyncCallbacks;
		break;
	case Duplex:
		s_SyncFollow = true;
		s_SyncLead = true;
		jackSlave(true);
		s_followerCallBacks= &s_SyncCallbacks;
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
	if ( jackAvailable() )
	{
		if (s_SyncOn) {	s_SyncOn = false; } else { s_SyncOn = true; }
	} else {
		s_SyncOn = false;
	}
	return s_SyncOn;
}


bool SyncCtl::have()
{
	return jackAvailable();
}


} // namespace lmms 

#endif // LMMS_HAVE_EXTERNALSYNC

