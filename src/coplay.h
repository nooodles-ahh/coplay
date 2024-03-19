/*
 * This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this
 * file, You can obtain one at https://mozilla.org/MPL/2.0/.
 */

//================================================
// CoaXioN Source SDK p2p networking: "CoaXioN Coplay"
// Author : Tholp / Jackson S
// File Last Modified : Feb 24 2024
//================================================

//Shared defines

// Ok, here the rundown on the concept behind this
// After socket opening and all the handshake stuff:
// The client has a UDP listener on port 27015 that relays all packets -
// through the Steam datagram, the server machine recieves them and -
// sends the UDP packets localy to the game server which has a similar mechanism to send back to the client.
// This allows us to make use of the P2P features steam offers within the Source SDK without any networking code changes
#pragma once
#ifndef COPLAY_H
#define COPLAY_H

#define COPLAY_MSG_COLOR Color(170, 255, 0, 255)
#define COPLAY_DEBUG_MSG_COLOR Color(255, 170, 0, 255)

#include <cbase.h>
#include "SDL2/SDL_net.h"
#include "steam/isteamnetworkingsockets.h"
#include "steam/isteamnetworkingutils.h"
#include "steam/isteamfriends.h"
#include "steam/steam_api.h"



struct CoplaySteamSocketTuple
{
    UDPsocket LocalSocket = NULL;
    uint16     Port = 0;
    HSteamNetConnection    SteamConnection = 0;


    bool DeletionQueued = false;
};

enum JoinFilter
{
    eP2PFilter_NONE = -1,
    eP2PFilter_EVERYONE = 0,
    eP2PFilter_FRIENDS,
    eP2PFilter_INVITEONLY,
};

enum ConnectionRole
{
    eConnectionRole_UNAVAILABLE = 0,
    eConnectionRole_HOST,
    eConnectionRole_CLIENT
};

enum ConnectionEndReason //see the enum ESteamNetConnectionEnd
{
    k_ESteamNetConnectionEnd_App_NotOpen = 1001,
    k_ESteamNetConnectionEnd_App_ServerFull = 1002,
    k_ESteamNetConnectionEnd_App_PortsFilled = 1003, //every port we tried is already bound, somehow
};

class CCoplayPacketHandler : public CThread
{
    int Run();
public:
    CCoplayPacketHandler()
    {
        SetName("coplay");
    }
    bool ShouldRun = true;
};

class CCoplayConnectionHandler : public CAutoGameSystemPerFrame
{
public:
    virtual bool Init();
    virtual void Update(float frametime);

    virtual void Shutdown()
    {
        if (packethandler)
        {
            packethandler->ShouldRun = false;
            packethandler->Join();//dont segfault on every exit
        }
    }

    void        OpenP2PSocket();
    void        CloseP2PSocket();
    bool        CreateSteamConnectionTuple(HSteamNetConnection hConn);

    ConnectionRole  GetConnectionRole(){return Role;}

private:
    HSteamListenSocket  HP2PSocket;
    ConnectionRole Role = eConnectionRole_UNAVAILABLE;
public:
    CUtlVector<CoplaySteamSocketTuple*> SteamConnections;

private:
    STEAM_CALLBACK(CCoplayConnectionHandler, ConnectionStatusUpdated, SteamNetConnectionStatusChangedCallback_t);
#ifdef CLIENT_DLL
    STEAM_CALLBACK(CCoplayConnectionHandler, JoinGame, GameRichPresenceJoinRequested_t);
#endif
    CCoplayPacketHandler *packethandler;
};
extern CCoplayConnectionHandler *g_pCoplayConnectionHandler;

#endif
