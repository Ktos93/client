/*
 * This file is part of the CitizenFX project - http://citizen.re/
 *
 * See LICENSE and MENTIONS in the root of the source tree for information
 * regarding licensing.
 */

#include "StdInc.h"
#include "NetLibrary.h"
#include <base64.h>
#include <mutex>
#include <mmsystem.h>
#include <yaml-cpp/yaml.h>

#include <ProfileManager.h>
#include <terminal.h>

uint16_t NetLibrary::GetServerNetID()
{
	return m_serverNetID;
}

uint16_t NetLibrary::GetHostNetID()
{
	return m_hostNetID;
}

void NetLibrary::ProcessPackets()
{
	ProcessPacketsInternal(NA_INET4);
	ProcessPacketsInternal(NA_INET6);
}

void NetLibrary::ProcessPacketsInternal(NetAddressType addrType)
{
	char buf[2048];
	memset(buf, 0, sizeof(buf));

	sockaddr_storage from;
	memset(&from, 0, sizeof(from));

	int fromlen = sizeof(from);

	auto socket = (addrType == NA_INET4) ? m_socket : m_socket6;

	while (true)
	{
		int len = recvfrom(socket, buf, 2048, 0, (sockaddr*)&from, &fromlen);

		NetAddress fromAddr((sockaddr*)&from);

		if (len == SOCKET_ERROR)
		{
			int error = WSAGetLastError();

			if (error != WSAEWOULDBLOCK)
			{
				trace("recv() failed - %d\n", error);
			}

			return;
		}

		if (*(int*)buf == -1)
		{
			ProcessOOB(fromAddr, &buf[4], len - 4);
		}
		else
		{
			if (fromAddr != m_currentServer)
			{
				trace("invalid from address for server msg\n");
				return;
			}

			NetBuffer* msg;

			if (m_netChannel.Process(buf, len, &msg))
			{
				ProcessServerMessage(*msg);

				delete msg;
			}
		}
	}
}

void NetLibrary::ProcessServerMessage(NetBuffer& msg)
{
	// update received-at time
	m_lastReceivedAt = GetTickCount();

	// metrics bits
	NetPacketMetrics metrics;

	// receive the message
	uint32_t msgType;

	uint32_t curReliableAck = msg.Read<uint32_t>();

	if (curReliableAck != m_outReliableAcknowledged)
	{
		for (auto it = m_outReliableCommands.begin(); it != m_outReliableCommands.end();)
		{
			if (it->id <= curReliableAck)
			{
				it = m_outReliableCommands.erase(it);
			}
			else
			{
				it++;
			}
		}

		m_outReliableAcknowledged = curReliableAck;
	}

	if (m_connectionState == CS_CONNECTED)
	{
		m_connectionState = CS_ACTIVE;
	}

	if (m_connectionState != CS_ACTIVE)
	{
		return;
	}

	do
	{
		if (msg.End())
		{
			break;
		}

		msgType = msg.Read<uint32_t>();

		if (msgType == 0xE938445B) // 'msgRoute'
		{
			uint16_t netID = msg.Read<uint16_t>();
			uint16_t rlength = msg.Read<uint16_t>();

			//trace("msgRoute from %d len %d\n", netID, rlength);

			char routeBuffer[65536];
			if (!msg.Read(routeBuffer, rlength))
			{
				break;
			}

			EnqueueRoutedPacket(netID, std::string(routeBuffer, rlength));

			// add to metrics
			metrics.AddElementSize(NET_PACKET_SUB_ROUTED_MESSAGES, 2 + rlength);
		}
		else if (msgType == 0x53FFFA3F) // msgFrame
		{
			// for now, frames are only an identifier - this will change once game features get moved to our code
			// (2014-10-15)

			uint32_t frameNum = msg.Read<uint32_t>();

			m_lastFrameNumber = frameNum;

			// handle ping status
			if (m_serverProtocol >= 3)
			{
				int currentPing = msg.Read<int>();

				if (m_metricSink.GetRef())
				{
					m_metricSink->OnPingResult(currentPing);
				}
			}
		}
		else if (msgType != 0xCA569E63) // reliable command
		{
			uint32_t id = msg.Read<uint32_t>();
			uint32_t size;

			if (id & 0x80000000)
			{
				size = msg.Read<uint32_t>();
				id &= ~0x80000000;

				metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 4);
			}
			else
			{
				size = msg.Read<uint16_t>();

				metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 2);
			}

			// test for bad scenarios
			if (id > (m_lastReceivedReliableCommand + 64))
			{
				return;
			}

			char* reliableBuf = new(std::nothrow) char[size];

			if (!reliableBuf)
			{
				return;
			}

			if (!msg.Read(reliableBuf, size))
			{
				break;
			}

			// check to prevent double execution
			if (id > m_lastReceivedReliableCommand)
			{
				HandleReliableCommand(msgType, reliableBuf, size);

				m_lastReceivedReliableCommand = id;
			}

			delete[] reliableBuf;

			// add to metrics
			metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 4 + size);
		}
	} while (msgType != 0xCA569E63); // 'msgEnd'

	if (m_metricSink.GetRef())
	{
		m_metricSink->OnIncomingPacket(metrics);
	}
}

bool NetLibrary::WaitForRoutedPacket(uint32_t timeout)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		if (!m_incomingPackets.empty())
		{
			return true;
		}
	}

	WaitForSingleObject(m_receiveEvent, timeout);

	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		return (!m_incomingPackets.empty());
	}
}

void NetLibrary::EnqueueRoutedPacket(uint16_t netID, std::string packet)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		RoutingPacket routePacket;
		routePacket.netID = netID;
		routePacket.payload = packet;

		m_incomingPackets.push(routePacket);
	}

	SetEvent(m_receiveEvent);
}

bool NetLibrary::DequeueRoutedPacket(char* buffer, size_t* length, uint16_t* netID)
{
	{
		std::lock_guard<std::mutex> guard(m_incomingPacketMutex);

		if (m_incomingPackets.empty())
		{
			return false;
		}

		auto packet = m_incomingPackets.front();
		m_incomingPackets.pop();

		memcpy(buffer, packet.payload.c_str(), packet.payload.size());
		*netID = packet.netID;
		*length = packet.payload.size();
	}

	ResetEvent(m_receiveEvent);

	return true;
}

void NetLibrary::RoutePacket(const char* buffer, size_t length, uint16_t netID)
{
	RoutingPacket routePacket;
	routePacket.netID = netID;
	routePacket.payload = std::string(buffer, length);

	m_outgoingPackets.push(routePacket);
}

void NetLibrary::ProcessOOB(NetAddress& from, char* oob, size_t length)
{
	if (from == m_currentServer)
	{
		if (!_strnicmp(oob, "connectOK", 9))
		{
			char* clientNetIDStr = &oob[10];
			char* hostIDStr = strchr(clientNetIDStr, ' ');

			hostIDStr[0] = '\0';
			hostIDStr++;

			char* hostBaseStr = strchr(hostIDStr, ' ');

			hostBaseStr[0] = '\0';
			hostBaseStr++;

			m_serverNetID = atoi(clientNetIDStr);
			m_hostNetID = atoi(hostIDStr);
			m_hostBase = atoi(hostBaseStr);

			m_lastReceivedReliableCommand = 0;

			trace("connectOK, our id %d, host id %d\n", m_serverNetID, m_hostNetID);

			OnConnectOKReceived(m_currentServer);

			m_netChannel.Reset(m_currentServer, this);
			m_connectionState = CS_CONNECTED;
		}
		else if (!_strnicmp(oob, "error", 5))
		{
			if (from != m_currentServer)
			{
				trace("Received 'error' request was not from the host\n");
				return;
			}

			char* errorStr = &oob[6];

			GlobalError("%s", errorStr);
		}
	}
}

void NetLibrary::SetHost(uint16_t netID, uint32_t base)
{
	m_hostNetID = netID;
	m_hostBase = base;
}

void NetLibrary::SetBase(uint32_t base)
{
	m_serverBase = base;
}

uint32_t NetLibrary::GetHostBase()
{
	return m_hostBase;
}

void NetLibrary::SetMetricSink(fwRefContainer<INetMetricSink>& sink)
{
	m_metricSink = sink;
}

void NetLibrary::HandleReliableCommand(uint32_t msgType, const char* buf, size_t length)
{
	auto range = m_reliableHandlers.equal_range(msgType);

	std::for_each(range.first, range.second, [&] (std::pair<uint32_t, ReliableHandlerType> handler)
	{
		handler.second(buf, length);
	});
}

NetLibrary::RoutingPacket::RoutingPacket()
{
	//genTime = timeGetTime();
	genTime = 0;
}

void NetLibrary::ProcessSend()
{
	// is it time to send a packet yet?
	bool continueSend = false;

/*	if (GameFlags::GetFlag(GameFlag::InstantSendPackets))
	{
		if (!m_outgoingPackets.empty())
		{
			continueSend = true;
		}
	}*/

	if (!continueSend)
	{
		uint32_t diff = timeGetTime() - m_lastSend;

		if (diff >= (1000 / 60))
		{
			continueSend = true;
		}
	}

	if (!continueSend)
	{
		return;
	}

	// do we have data to send?
	if (m_connectionState != CS_ACTIVE)
	{
		return;
	}

	// metrics
	NetPacketMetrics metrics;

	// build a nice packet
	NetBuffer msg(24000);

	msg.Write(m_lastReceivedReliableCommand);

	if (m_serverProtocol >= 2)
	{
		msg.Write(m_lastFrameNumber);
	}

	RoutingPacket packet;

	while (m_outgoingPackets.try_pop(packet))
	{
		msg.Write(0xE938445B); // msgRoute
		msg.Write(packet.netID);
		msg.Write<uint16_t>(packet.payload.size());

		//trace("sending msgRoute to %d len %d\n", packet.netID, packet.payload.size());

		msg.Write(packet.payload.c_str(), packet.payload.size());

		metrics.AddElementSize(NET_PACKET_SUB_ROUTED_MESSAGES, packet.payload.size() + 2 + 2 + 4);
	}

	// send pending reliable commands
	for (auto& command : m_outReliableCommands)
	{
		msg.Write(command.type);

		if (command.command.size() > UINT16_MAX)
		{
			msg.Write(command.id | 0x80000000);

			msg.Write<uint32_t>(command.command.size());

			metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 4);
		}
		else
		{
			msg.Write(command.id);

			msg.Write<uint16_t>(command.command.size());

			metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, 2);
		}

		msg.Write(command.command.c_str(), command.command.size());

		metrics.AddElementSize(NET_PACKET_SUB_RELIABLES, command.command.size() + 8);
	}

	// FIXME: REPLACE HARDCODED STUFF
	/*if (*(BYTE*)0x18A82FD) // is server running
	{
		msg.Write(0xB3EA30DE); // msgIHost
		msg.Write(m_serverBase);
	}*/

	OnBuildMessage(msg);

	msg.Write(0xCA569E63); // msgEnd

	m_netChannel.Send(msg);

	m_lastSend = timeGetTime();

	if (m_metricSink.GetRef())
	{
		m_metricSink->OnOutgoingPacket(metrics);
	}
}

void NetLibrary::SendReliableCommand(const char* type, const char* buffer, size_t length)
{
	uint32_t unacknowledged = m_outReliableSequence - m_outReliableAcknowledged;

	if (unacknowledged > MAX_RELIABLE_COMMANDS)
	{
		GlobalError("Reliable client command overflow.");
	}

	m_outReliableSequence++;

	OutReliableCommand cmd;
	cmd.type = HashRageString(type);
	cmd.id = m_outReliableSequence;
	cmd.command = std::string(buffer, length);

	m_outReliableCommands.push_back(cmd);
}

static std::string g_disconnectReason;

static std::mutex g_netFrameMutex;

void NetLibrary::PreProcessNativeNet()
{
	if (!g_netFrameMutex.try_lock())
	{
		return;
	}

	ProcessPackets();

	g_netFrameMutex.unlock();
}

void NetLibrary::PostProcessNativeNet()
{
	if (!g_netFrameMutex.try_lock())
	{
		return;
	}

	ProcessSend();

	g_netFrameMutex.unlock();
}

void NetLibrary::RunFrame()
{
	if (!g_netFrameMutex.try_lock())
	{
		return;
	}

	ProcessPackets();

	ProcessSend();

	switch (m_connectionState)
	{
		case CS_INITRECEIVED:
			// change connection state to CS_DOWNLOADING
			m_connectionState = CS_DOWNLOADING;

			// trigger task event
			OnInitReceived(m_currentServer);

			break;

		case CS_DOWNLOADCOMPLETE:
			m_connectionState = CS_CONNECTING;
			m_lastConnect = 0;
			m_connectAttempts = 0;

			break;

		case CS_CONNECTING:
			if ((GetTickCount() - m_lastConnect) > 5000)
			{
				TerminalClient* clientContainer = Instance<TerminalClient>::Get();
				auto client = clientContainer->GetClient();
				auto user = static_cast<terminal::IUser1*>(client->GetUserService(terminal::IUser1::InterfaceID).GetDetail());

				SendOutOfBand(m_currentServer, "connect token=%s&guid=%llu", m_token.c_str(), user->GetNPID());

				m_lastConnect = GetTickCount();

				m_connectAttempts++;
			}

			if (m_connectAttempts > 3)
			{
				g_disconnectReason = "Connection timed out.";
				FinalizeDisconnect();

				OnConnectionTimedOut();

				GlobalError("Failed to connect to server after 3 attempts.");
			}

			break;

		case CS_ACTIVE:
			if ((GetTickCount() - m_lastReceivedAt) > 15000)
			{
				g_disconnectReason = "Connection timed out.";
				FinalizeDisconnect();

				OnConnectionTimedOut();

				GlobalError("Server connection timed out after 15 seconds.");
			}

			break;
	}

	g_netFrameMutex.unlock();
}

void NetLibrary::Death()
{
	g_netFrameMutex.unlock();
}

void NetLibrary::Resurrection()
{
	g_netFrameMutex.lock();
}

void NetLibrary::ConnectToServer(const char* hostname, uint16_t port)
{
	if (m_connectionState != CS_IDLE)
	{
		Disconnect("Bye!");
	}

	m_connectionState = CS_INITING;
	m_currentServer = NetAddress(hostname, port);

	m_outReliableAcknowledged = 0;
	m_outSequence = 0;
	m_lastReceivedReliableCommand = 0;
	m_outReliableCommands.clear();

	m_lastFrameNumber = 0;

	wchar_t wideHostname[256];
	mbstowcs(wideHostname, hostname, _countof(wideHostname) - 1);

	wideHostname[255] = L'\0';

	fwWString wideHostnameStr = wideHostname;

	static fwMap<fwString, fwString> postMap;
	postMap["method"] = "initConnect";
	postMap["name"] = GetPlayerName();
	postMap["protocol"] = va("%d", NETWORK_PROTOCOL);

	TerminalClient* clientContainer = Instance<TerminalClient>::Get();
	auto client = clientContainer->GetClient();
	auto user = static_cast<terminal::IUser1*>(client->GetUserService(terminal::IUser1::InterfaceID).GetDetail());

	postMap["guid"] = va("%llu", user->GetNPID());

	uint16_t capturePort = port;

	static fwAction<bool, const char*, size_t> handleAuthResult;
	handleAuthResult = [=] (bool result, const char* connDataStr, size_t size) mutable
	{
		std::string connData(connDataStr, size);

		if (!result)
		{
			// TODO: add UI output
			m_connectionState = CS_IDLE;

			//nui::ExecuteRootScript("citFrames[\"mpMenu\"].contentWindow.postMessage({ type: 'connectFailed', message: 'General handshake failure.' }, '*');");
			OnConnectionError("General handshake failure.");

			return;
		}

		try
		{
			auto node = YAML::Load(connData);

			// ha-ha, you need to authenticate first!
			if (node["authID"].IsDefined())
			{
				if (postMap.find("authTicket") == postMap.end())
				{
					std::vector<uint8_t> authTicket = user->GetUserTicket(node["authID"].as<uint64_t>());

					size_t ticketLen;
					char* ticketEncoded = base64_encode(&authTicket[0], authTicket.size(), &ticketLen);

					postMap["authTicket"] = fwString(ticketEncoded, ticketLen);

					free(ticketEncoded);

					m_httpClient->DoPostRequest(wideHostnameStr, capturePort, L"/client", postMap, handleAuthResult);
				}
				else
				{
					postMap.erase("authTicket");

					GlobalError("you're so screwed, the server still asked for an auth ticket even though we gave them one");
				}

				return;
			}

			postMap.erase("authTicket");

			if (node["error"].IsDefined())
			{
				// FIXME: single quotes
				//nui::ExecuteRootScript(va("citFrames[\"mpMenu\"].contentWindow.postMessage({ type: 'connectFailed', message: '%s' }, '*');", node["error"].as<std::string>().c_str()));
				OnConnectionError(node["error"].as<std::string>().c_str());

				m_connectionState = CS_IDLE;

				return;
			}

			m_token = node["token"].as<std::string>();

			m_serverProtocol = node["protocol"].as<uint32_t>();

			m_connectionState = CS_INITRECEIVED;
		}
		catch (YAML::Exception&)
		{
			m_connectionState = CS_IDLE;
		}
	};

	m_httpClient->DoPostRequest(wideHostname, port, L"/client", postMap, handleAuthResult);
}

void NetLibrary::Disconnect(const char* reason)
{
	g_disconnectReason = reason;

	OnAttemptDisconnect(reason);
	//GameInit::KillNetwork((const wchar_t*)1);
}

void NetLibrary::FinalizeDisconnect()
{
	if (m_connectionState == CS_CONNECTING || m_connectionState == CS_ACTIVE)
	{
		SendReliableCommand("msgIQuit", g_disconnectReason.c_str(), g_disconnectReason.length() + 1);

		m_lastSend = 0;
		ProcessSend();

		m_lastSend = 0;
		ProcessSend();

		OnFinalizeDisconnect(m_currentServer);
		//GameFlags::ResetFlags();

		//TheResources.CleanUp();
		//TheDownloads.ReleaseLastServer();

		m_connectionState = CS_IDLE;
		m_currentServer = NetAddress();

		//GameInit::MurderGame();
	}
}

void NetLibrary::CreateResources()
{
	WSADATA wsaData;
	if (WSAStartup(MAKEWORD(2, 2), &wsaData) != 0)
	{
		GlobalError("WSAStartup did not succeed.");
	}

	// create IPv4 socket
	m_socket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);

	if (m_socket == INVALID_SOCKET)
	{
		GlobalError("only one sock in pair");
	}

	// explicit bind
	sockaddr_in localAddr = { 0 };
	localAddr.sin_family = AF_INET;
	localAddr.sin_addr.s_addr = ADDR_ANY;
	localAddr.sin_port = 0;

	bind(m_socket, (sockaddr*)&localAddr, sizeof(localAddr));

	// non-blocking
	u_long arg = true;
	ioctlsocket(m_socket, FIONBIO, &arg);

	// create IPv6 socket
	m_socket6 = socket(AF_INET6, SOCK_DGRAM, IPPROTO_UDP);

	if (m_socket6 != INVALID_SOCKET)
	{
		// bind the socket
		sockaddr_in6 ip6Addr = { 0 };
		ip6Addr.sin6_family = AF_INET6;
		ip6Addr.sin6_addr = in6addr_any;
		ip6Addr.sin6_port = 0;

		bind(m_socket6, (sockaddr*)&ip6Addr, sizeof(ip6Addr));

		// make non-blocking
		ioctlsocket(m_socket6, FIONBIO, &arg);
	}

	m_httpClient = new HttpClient();
	//m_httpClient = new HttpClient();

	// TEMPTEMP
	/*uint8_t out[1024];
	uint8_t in[] = { 0x19, 0x00, 0xF7, 0x03, 0xC7, 0x40, 0x00, 0x02, 0x00, 0x01, 0xB4, 0x8D, 0xFD, 0x94, 0x8D, 0xAD, 0x03, 0xC5, 0xC0, 0xE4, 0x00, 0xB0, 0xF0, 0xDA, 0x30, 0xDA, 0x4D, 0x03, 0xC7, 0xC0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0xB0, 0x53, 0xF0, 0x54, 0x0D };

	__asm
	{
		push edx
		mov ecx, 785B70h
		mov edx, 1bh

		lea eax, in
		push eax
		push 1021

		lea eax, out

		call ecx

		add esp, 8

		pop edx
	}*/
}

void NetLibrary::SendOutOfBand(NetAddress& address, const char* format, ...)
{
	static char buffer[32768];

	*(int*)buffer = -1;

	va_list ap;
	va_start(ap, format);
	int length = _vsnprintf(&buffer[4], 32764, format, ap);
	va_end(ap);

	if (length >= 32764)
	{
		GlobalError("Attempted to overrun string in call to SendOutOfBand()!");
	}

	buffer[32767] = '\0';

	SendData(address, buffer, strlen(buffer));
}

const char* NetLibrary::GetPlayerName()
{
	ProfileManager* profileManager = Instance<ProfileManager>::Get();
	fwRefContainer<Profile> profile = profileManager->GetPrimaryProfile();

	const char* returnName = nullptr;

	if (profile.GetRef())
	{
		returnName = profile->GetDisplayName();
	}
	else
	{
		static char computerName[64];
		DWORD nameSize = sizeof(computerName);
		GetComputerNameA(computerName, &nameSize);

		returnName = computerName;
	}

	return returnName;
}

void NetLibrary::SetPlayerName(const char* name)
{
	m_playerName = name;
}

void NetLibrary::SendData(NetAddress& address, const char* data, size_t length)
{
	sockaddr_storage addr;
	int addrLen;
	address.GetSockAddr(&addr, &addrLen);

	if (addr.ss_family == AF_INET)
	{
		sendto(m_socket, data, length, 0, (sockaddr*)&addr, addrLen);
	}
	else if (addr.ss_family == AF_INET6)
	{
		sendto(m_socket6, data, length, 0, (sockaddr*)&addr, addrLen);
	}
}

void NetLibrary::AddReliableHandler(const char* type, ReliableHandlerType function)
{
	uint32_t hash = HashRageString(type);

	m_reliableHandlers.insert(std::make_pair(hash, function));
}

void NetLibrary::DownloadsComplete()
{
	if (m_connectionState == CS_DOWNLOADING)
	{
		m_connectionState = CS_DOWNLOADCOMPLETE;
	}
}

bool NetLibrary::ProcessPreGameTick()
{
	if (m_connectionState != CS_ACTIVE && m_connectionState != CS_CONNECTED && m_connectionState != CS_IDLE)
	{
		RunFrame();

		return false;
	}

	return true;
}

void NetLibrary::SendNetEvent(fwString eventName, fwString jsonString, int i)
{
	const char* cmdType = "msgNetEvent";

	if (i == -1)
	{
		i = UINT16_MAX;
	}
	else if (i == -2)
	{
		cmdType = "msgServerEvent";
	}

	size_t eventNameLength = eventName.length();

	NetBuffer buffer(100000);

	if (i >= 0)
	{
		buffer.Write<uint16_t>(i);
	}

	buffer.Write<uint16_t>(eventNameLength + 1);
	buffer.Write(eventName.c_str(), eventNameLength + 1);

	buffer.Write(jsonString.c_str(), jsonString.size());
	
	SendReliableCommand(cmdType, buffer.GetBuffer(), buffer.GetCurLength());
}

/*void NetLibrary::AddReliableHandler(const char* type, ReliableHandlerType function)
{
	netLibrary.AddReliableHandlerImpl(type, function);
}*/

NetLibrary::NetLibrary()
	: m_serverNetID(0), m_serverBase(0), m_hostBase(0), m_hostNetID(0), m_connectionState(CS_IDLE),
	  m_tempGuid(0), m_lastConnect(0), m_lastSend(0), m_outSequence(0), m_lastReceivedReliableCommand(0), m_outReliableAcknowledged(0), m_outReliableSequence(0),
	  m_lastReceivedAt(0)

{
	m_receiveEvent = CreateEvent(nullptr, TRUE, FALSE, nullptr);
}

__declspec(dllexport) fwEvent<NetLibrary*> NetLibrary::OnNetLibraryCreate;

NetLibrary* NetLibrary::Create()
{
	auto lib = new NetLibrary();

	lib->CreateResources();

	OnNetLibraryCreate(lib);

	return lib;
}