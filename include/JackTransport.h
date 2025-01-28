/*
 * JackTransport.h - support for synchronization with jack transport
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

#ifndef LMMS_JACKTRANSPORT_H
#define LMMS_JACKTRANSPORT_H

#include "lmmsconfig.h"

#ifdef LMMS_HAVE_JACK

#include "lmms_basics.h"

namespace lmms
{

/** 
	Catch events,needed to sent.
	Events:
	* jump - when song position changed not in "natural" way
	(most challenging event to catch, so even @pulse() is needed);
	* start - when song starts playing ;
	* stop - when song stops playing . 
	Implementation details see in ExSync.cpp
 */ 
class SyncHook
{
public:
	static void jump(); //!< placed where jump introduced by user or by LMMS
	static void pulseStop(); //!< stopes internal thread for pulse()
	static void start();
	static void stop();
	// Used only in JackTransport.cpp:
	static void pulse(); //!< called periodically to catch jump when stopped
};

/**
	Used to control ExSync by GUI (all calls are in SongEditorWindow) 
*/
class SyncCtl
{
public:
	//! ExSync modes named from LMMS point of view, toggled in round robin way 
	enum SyncMode 
	{
		Leader = 0, //!< LMMS send commands, but not react 
		Follower, //!< LMMS react but not send
		Duplex, //!< LMMS send and react, position followed to external application
		Last //!< used for array element count 
	};
	static SyncMode toggleMode(); //!< @return mode after call
	static void setMode(SyncMode mode); //!< directly set mode, or set Off, if "Last" is used
	static SyncMode getMode(); //!< @return current mode
	static bool toggleOnOff(); //!< @return true if ExternalSync became active
	static bool have(); //!< @return true if available
};


} // namespace lmms 


#endif // LMMS_HAVE_JACK


#endif // LMMS_JACKTRANSPORT_H
