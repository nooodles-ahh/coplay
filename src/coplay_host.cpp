#include "cbase.h"
#include "coplay.h"
#include "coplay_host.h"
#include <inetchannel.h>
#include <inetchannelinfo.h>
#include "coplay_connection.h"

CCoplayHost::CCoplayHost() :
	m_hSocket(k_HSteamListenSocket_Invalid),
	m_usingPassword(false),
	m_lobby(k_steamIDNil)
{
}

CCoplayHost::~CCoplayHost()
{
}

extern ConVar coplay_use_lobbies;
extern ConVar coplay_joinfilter;
void CCoplayHost::StartHosting()
{
    // ensure we're in a game
    if (!engine->IsConnected())
    {
        ConColorMsg(COPLAY_MSG_COLOR, "You're not currently in a local game.");
        return;
    }

	// ensure we're in a local game
    INetChannelInfo* netinfo = engine->GetNetChannelInfo();
    std::string ip = netinfo->GetAddress();
    if (!(netinfo->IsLoopback() || ip.find("127") == 0))
    {
        ConColorMsg(COPLAY_MSG_COLOR, "You're not currently in a local game.%s\n", netinfo->GetAddress());
        return;
    }

	// if we currently have an active connection, close it
    if (IsHosting())
		StopHosting();

    ConVarRef sv_lan("sv_lan");
    ConVarRef engine_no_focus_sleep("engine_no_focus_sleep");
    //sv_lan off will heartbeat the server and allow clients to see our ip
    sv_lan.SetValue("1");
    // stops everyone lagging out when the host unfocuses the game
    engine_no_focus_sleep.SetValue("0"); 

	// create a listen socket
    m_hSocket = SteamNetworkingSockets()->CreateListenSocketP2P(0, 0, NULL);

    if (coplay_use_lobbies.GetBool())
    {
		// open a lobby with the appropriate settings
        SteamMatchmaking()->LeaveLobby(m_lobby);
        int filter = coplay_joinfilter.GetInt();
        ELobbyType lobbytype = (ELobbyType)(filter > -1 ? filter : 0);
        SteamMatchmaking()->CreateLobby(lobbytype, gpGlobals->maxClients);
    }
    else
    {
		// we're not using lobbies, so just generate a random passcode the user will need to provide
        RandomizePasscode();
    }
}

void CCoplayHost::StopHosting()
{
	if (IsHosting())
	{
		// close all connections
        for (int i = 0; i < m_connections.Count(); i++)
            m_connections[i]->QueueForDeletion();

        for (int i = 0; i < m_connections.Count(); i++)
            m_connections[i]->Join();

		m_connections.PurgeAndDeleteElements();

		SteamNetworkingSockets()->CloseListenSocket(m_hSocket);
		m_hSocket = k_HSteamListenSocket_Invalid;
	}

	// shutdown the lobby
	if (coplay_use_lobbies.GetBool() && m_lobby != k_steamIDNil)
	{
		SteamMatchmaking()->LeaveLobby(m_lobby);
		m_lobby.Clear();
	}

	// reset convars
    ConVarRef engine_no_focus_sleep("engine_no_focus_sleep");
    engine_no_focus_sleep.SetValue(engine_no_focus_sleep.GetDefault());
}

void CCoplayHost::Update()
{
	if (!IsHosting())
		return;
    
	// check our threads for deletion
	FOR_EACH_VEC_BACK(m_connections, i)
	{
		if (m_connections[i]->IsAlive())
		{
			m_connections[i]->Join();
			delete m_connections[i];
			m_connections.Remove(i);
		}
	}
}

bool CCoplayHost::ConnectionStatusUpdated(SteamNetConnectionStatusChangedCallback_t* pParam)
{
	bool stateFailed = false;
    // Somehow left without us catching it, map transistion load error or cancelation probably
    if (!engine->IsConnected() || !IsHosting())
    {
        SteamNetworkingSockets()->CloseConnection(pParam->m_hConn, k_ESteamNetConnectionEnd_App_NotOpen, "", false);
        return true;
    }

    switch (pParam->m_info.m_eState)
    {
    case k_ESteamNetworkingConnectionState_Connecting:
		// lobbies filter for us already, so we can just accept the connection
        if (coplay_use_lobbies.GetBool())
        {
            if (IsUserInLobby(m_lobby, pParam->m_info.m_identityRemote.GetSteamID()))
                SteamNetworkingSockets()->AcceptConnection(pParam->m_hConn);
        }
		// if we're not using lobbies, we need to the join filter
        else
        {
            switch (coplay_joinfilter.GetInt())
            {
            case eP2PFilter_EVERYONE:
                SteamNetworkingSockets()->AcceptConnection(pParam->m_hConn);
                break;

            case eP2PFilter_FRIENDS:
                if (SteamFriends()->HasFriend(pParam->m_info.m_identityRemote.GetSteamID(), k_EFriendFlagImmediate))
                    SteamNetworkingSockets()->AcceptConnection(pParam->m_hConn);
                else
                    SteamNetworkingSockets()->CloseConnection(pParam->m_hConn, k_ESteamNetConnectionEnd_App_NotFriend, "", true);
                break;

            // This is for passwords, we cant get the password before making a connection so dont make a CCoplayConnection until we get it
            // Connections in PendingConnections are run in CCoplaySystem::Update() waiting to recieve it
            case eP2PFilter_CONTROLLED:
				// Create pending connection
                break;

			// sent something that we dont know how to handle
            default:
                SteamNetworkingSockets()->CloseConnection(pParam->m_hConn, k_ESteamNetConnectionEnd_App_NotOpen, "", true);
                break;
            }
        }
        break;

    case k_ESteamNetworkingConnectionState_Connected:
		// add the connection to our list
        if (!AddConnection(pParam->m_hConn))
            SteamNetworkingSockets()->CloseConnection(pParam->m_hConn, k_ESteamNetConnectionEnd_App_RemoteIssue, "", true);
        break;

    case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
		// tell the client to disconnect
        SteamNetworkingSockets()->CloseConnection(pParam->m_hConn, k_ESteamNetConnectionEnd_Misc_Timeout, "", true);
		// todo check if this is a valid path for the host
    case k_ESteamNetworkingConnectionState_ClosedByPeer:
        StopHosting();
		stateFailed = true;
        break;
    }

	return stateFailed;
}

void CCoplayHost::RandomizePasscode()
{
    static const std::string validchars = "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789";
    m_password.clear();
    for (int i = 0; i < 32; i++)
        m_password += validchars[rand() % validchars.length()];
}

bool CCoplayHost::AddConnection(HSteamNetConnection hConnection)
{
    SteamNetConnectionInfo_t newinfo;
    if (!SteamNetworkingSockets()->GetConnectionInfo(hConnection, &newinfo))
        return false;

	// delete any existing connections from the same user
	FOR_EACH_VEC_BACK(m_connections, i)
	{
		SteamNetConnectionInfo_t info;
		if (SteamNetworkingSockets()->GetConnectionInfo(m_connections[i]->m_hSteamConnection, &info))
		{
			if (info.m_identityRemote.GetSteamID64() == newinfo.m_identityRemote.GetSteamID64())
			{
				m_connections[i]->QueueForDeletion();
				break;
			}
		}
	}

	// create a new connection
    CCoplayConnection* connection = new CCoplayConnection(hConnection);
    connection->Start();
    m_connections.AddToTail(connection);
    return true;
}