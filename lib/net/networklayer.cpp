//
// networklayer.cpp
//
// Circle - A C++ bare metal environment for Raspberry Pi
// Copyright (C) 2015-2024  R. Stange <rsta2@o2online.de>
// 
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//
// You should have received a copy of the GNU General Public License
// along with this program.  If not, see <http://www.gnu.org/licenses/>.
//
#include <circle/net/networklayer.h>
#include <circle/net/checksumcalculator.h>
#include <circle/net/in.h>
#include <circle/util.h>
#include <assert.h>

CNetworkLayer::CNetworkLayer (CNetConfig *pNetConfig, CLinkLayer *pLinkLayer)
:	m_pNetConfig (pNetConfig),
	m_pLinkLayer (pLinkLayer),
	m_pICMPHandler (0),
	m_pICMPRxQueue2 (0),
	m_pIGMPHandler (0) // Initialize here
{
	assert (m_pNetConfig != 0);
	assert (m_pLinkLayer != 0);
}

CNetworkLayer::~CNetworkLayer (void)
{
	delete m_pICMPRxQueue2;
	m_pICMPRxQueue2 = 0;

	delete m_pICMPHandler;
	m_pICMPHandler = 0;

	delete m_pIGMPHandler; // Add this
	m_pIGMPHandler = 0;    // Add this

	m_pLinkLayer = 0;
	m_pNetConfig = 0;
}

boolean CNetworkLayer::Initialize (void)
{
	assert (m_pICMPHandler == 0);
	m_pICMPHandler = new CICMPHandler (m_pNetConfig, this, &m_ICMPRxQueue, &m_ICMPNotificationQueue);
	assert (m_pICMPHandler != 0);

	assert (m_pIGMPHandler == 0); // Add this block
	m_pIGMPHandler = new CIGMPHandler (m_pNetConfig, this);
	assert (m_pIGMPHandler != 0);
	if (!m_pIGMPHandler->Initialize())
	{
		// CLogger::Get()->Write(LogTagNetwork, LogError, "IGMP Handler initialization failed"); // Assuming LogTagNetwork exists
		// Using a generic log tag for now, as CNetworkLayer doesn't have its own LogTag field
		CLogger::Get()->Write("network", LogError, "IGMP Handler initialization failed");
		return FALSE;
	}

	return TRUE;
}

void CNetworkLayer::Process (void)
{
	assert (m_pNetConfig != 0);
	const CIPAddress *pOwnIPAddress = m_pNetConfig->GetIPAddress ();
	assert (pOwnIPAddress != 0);

	u8 Buffer[FRAME_BUFFER_SIZE];
	unsigned nResultLength;
	assert (m_pLinkLayer != 0);
	while (m_pLinkLayer->Receive (Buffer, &nResultLength))
	{
		if (nResultLength <= sizeof (TIPHeader))
		{
			continue;
		}
		TIPHeader *pHeader = (TIPHeader *) Buffer;

		unsigned nHeaderLength = pHeader->nVersionIHL & 0xF;
		if (   nHeaderLength < IP_HEADER_LENGTH_DWORD_MIN
		    || nHeaderLength > IP_HEADER_LENGTH_DWORD_MAX)
		{
			continue;
		}
		nHeaderLength *= 4;
		if (nResultLength <= nHeaderLength)
		{
			continue;
		}

		if (   CChecksumCalculator::SimpleCalculate (pHeader, nHeaderLength) != CHECKSUM_OK
		    || (pHeader->nVersionIHL >> 4) != IP_VERSION)
		{
			continue;
		}

		CIPAddress IPAddressDestination (pHeader->DestinationAddress);
		if (!pOwnIPAddress->IsNull ()) // If this device has an IP address
		{
			if (   *pOwnIPAddress != IPAddressDestination              // Not our unicast IP
			    && !IPAddressDestination.IsBroadcast ()             // Not a broadcast IP
			    && !IPAddressDestination.IsMulticast ()             // AND Not a multicast IP
			    && (*m_pNetConfig->GetBroadcastAddress () != IPAddressDestination)) // Also not the subnet broadcast
			{
				continue; // Then drop
			}
		}
		else // If this device does not have an IP address yet (e.g., during DHCP)
		{
			// Only allow broadcast or multicast packets through, as we can't match a unicast IP.
			// This is important for DHCP which uses broadcast.
			// Multicast might be less relevant here but allowing it is harmless.
			if (   !IPAddressDestination.IsBroadcast ()
			    && !IPAddressDestination.IsMulticast ())
			{
				continue; // Drop if not broadcast or multicast
			}
		}

		if (   (pHeader->nFlagsFragmentOffset & IP_FLAGS_MF)
		    ||    IP_FRAGMENT_OFFSET (le2be16 (pHeader->nFlagsFragmentOffset))
		       != IP_FRAGMENT_OFFSET_FIRST)
		{
			continue;
		}
		
		unsigned nTotalLength = le2be16 (pHeader->nTotalLength);
		if (nResultLength < nTotalLength)
		{
			continue;
		}
		nResultLength = nTotalLength;		// ignore padding

		TNetworkPrivateData *pParam = new TNetworkPrivateData;
		assert (pParam != 0);
		pParam->nProtocol = pHeader->nProtocol;
		memcpy (pParam->SourceAddress, pHeader->SourceAddress, IP_ADDRESS_SIZE);
		memcpy (pParam->DestinationAddress, pHeader->DestinationAddress, IP_ADDRESS_SIZE);

		nResultLength -= nHeaderLength;

		if (pHeader->nProtocol == IPPROTO_ICMP)
		{
			if (m_pICMPRxQueue2 != 0)
			{
				TNetworkPrivateData *pParam2 = new TNetworkPrivateData;
				assert (pParam2 != 0);
				memcpy (pParam2, pParam, sizeof *pParam);

				m_pICMPRxQueue2->Enqueue (Buffer+nHeaderLength, nResultLength,
							  pParam2);
			}

			m_ICMPRxQueue.Enqueue (Buffer+nHeaderLength, nResultLength, pParam);
		}
		else if (pHeader->nProtocol == IPPROTO_IGMP) // Check for IPPROTO_IGMP (value is 2)
		{
			if (m_pIGMPHandler != 0)
			{
				CIPAddress senderIP(pParam->SourceAddress); // pParam is TNetworkPrivateData
				m_pIGMPHandler->ProcessPacket(Buffer+nHeaderLength, nResultLength, senderIP);
			}
			delete pParam; // Consume the pParam for IGMP packets here
		}
		else // Other protocols (UDP, TCP)
		{
			m_RxQueue.Enqueue (Buffer+nHeaderLength, nResultLength, pParam);
		}
	}

	assert (m_pICMPHandler != 0);
	m_pICMPHandler->Process ();
	
	// TODO: Add m_pIGMPHandler->ProcessTimer() or similar if IGMP needs periodic processing
	// For now, IGMP processing is purely reactive to received packets or join/leave calls.
	// If timers are added in CIGMPHandler for query responses, then a Process() or TimerTick()
	// method would be needed here, called from CNetTask or similar.
}

boolean CNetworkLayer::Send (const CIPAddress &rReceiver, const void *pPacket, unsigned nLength, int nProtocol)
{
	unsigned nPacketLength = sizeof (TIPHeader) + nLength;		// may wrap
	if (   nPacketLength <= sizeof (TIPHeader)
	    || nPacketLength > FRAME_BUFFER_SIZE)
	{
		return FALSE;
	}

	u8 PacketBuffer[nPacketLength];
	TIPHeader *pHeader = (TIPHeader *) PacketBuffer;

	pHeader->nVersionIHL          = IP_VERSION << 4 | IP_HEADER_LENGTH_DWORD_MIN;
	pHeader->nTypeOfService       = IP_TOS_ROUTINE;
	pHeader->nTotalLength         = le2be16 ((u16) nPacketLength);
	pHeader->nIdentification      = BE (IP_IDENTIFICATION_DEFAULT);
	pHeader->nFlagsFragmentOffset = IP_FLAGS_DF | BE (IP_FRAGMENT_OFFSET_FIRST);
	pHeader->nTTL                 = rReceiver.IsMulticast () ? IP_TTL_MULTICAST : IP_TTL_DEFAULT;
	pHeader->nProtocol            = (u8) nProtocol;

	assert (m_pNetConfig != 0);
	const CIPAddress *pOwnIPAddress = m_pNetConfig->GetIPAddress ();
	assert (pOwnIPAddress != 0);

	pOwnIPAddress->CopyTo (pHeader->SourceAddress);

	rReceiver.CopyTo (pHeader->DestinationAddress);

	pHeader->nHeaderChecksum = 0;
	pHeader->nHeaderChecksum = CChecksumCalculator::SimpleCalculate (pHeader, sizeof (TIPHeader));

	assert (pPacket != 0);
	assert (nLength > 0);
	memcpy (PacketBuffer+sizeof (TIPHeader), pPacket, nLength);

	if (   pOwnIPAddress->IsNull ()
	    && !rReceiver.IsBroadcast ())
	{
		SendFailed (ICMP_CODE_DEST_NET_UNREACH, PacketBuffer, nPacketLength);

		return FALSE;
	}

	CIPAddress GatewayIP;
	const CIPAddress *pNextHop = &rReceiver;
	if (   !rReceiver.IsMulticast ()
	    && !pOwnIPAddress->OnSameNetwork (rReceiver, m_pNetConfig->GetNetMask ()))
	{
		const u8 *pGateway = m_RouteCache.GetRoute (rReceiver.Get ());
		if (pGateway != 0)
		{
			GatewayIP.Set (pGateway);

			pNextHop = &GatewayIP;
		}
		else
		{
			pNextHop = m_pNetConfig->GetDefaultGateway ();
			if (pNextHop->IsNull ())
			{
				SendFailed (ICMP_CODE_DEST_NET_UNREACH, PacketBuffer, nPacketLength);

				return FALSE;
			}
		}
	}
	
	assert (m_pLinkLayer != 0);
	assert (pNextHop != 0);
	return m_pLinkLayer->Send (*pNextHop, PacketBuffer, nPacketLength);
}

boolean CNetworkLayer::Receive (void *pBuffer, unsigned *pResultLength,
				CIPAddress *pSender, CIPAddress *pReceiver, int *pProtocol)
{
	void *pParam;
	assert (pBuffer != 0);
	assert (pResultLength != 0);
	*pResultLength = m_RxQueue.Dequeue (pBuffer, &pParam);
	if (*pResultLength == 0)
	{
		return FALSE;
	}
	
	TNetworkPrivateData *pData = (TNetworkPrivateData *) pParam;
	assert (pData != 0);

	assert (pProtocol != 0);
	*pProtocol = pData->nProtocol;

	assert (pSender != 0);
	pSender->Set (pData->SourceAddress);

	assert (pReceiver != 0);
	pReceiver->Set (pData->DestinationAddress);

	delete pData;
	pData = 0;
	
	return TRUE;
}

boolean CNetworkLayer::ReceiveNotification (TICMPNotificationType *pType,
					    CIPAddress *pSender, CIPAddress *pReceiver,
					    u16 *pSendPort, u16 *pReceivePort,
					    int *pProtocol)
{
	TICMPNotification Notification;
	unsigned nLength = m_ICMPNotificationQueue.Dequeue (&Notification);
	if (nLength == 0)
	{
		return FALSE;
	}
	assert (nLength == sizeof Notification);

	assert (pType != 0);
	*pType = Notification.Type;

	assert (pProtocol != 0);
	*pProtocol = Notification.nProtocol;

	assert (pSender != 0);
	pSender->Set (Notification.SourceAddress);

	assert (pReceiver != 0);
	pReceiver->Set (Notification.DestinationAddress);

	assert (pSendPort != 0);
	*pSendPort = Notification.nSourcePort;

	assert (pReceivePort != 0);
	*pReceivePort = Notification.nDestinationPort;

	return TRUE;
}

void CNetworkLayer::EnableReceiveICMP (boolean bEnable)
{
	if (bEnable)
	{
		if (m_pICMPRxQueue2 == 0)
		{
			m_pICMPRxQueue2 = new CNetQueue;
			assert (m_pICMPRxQueue2 != 0);
		}
	}
	else
	{
		if (m_pICMPRxQueue2 != 0)
		{
			void *pParam;
			u8 Buffer[FRAME_BUFFER_SIZE];
			while (m_pICMPRxQueue2->Dequeue (Buffer, &pParam))
			{
				TNetworkPrivateData *pData = (TNetworkPrivateData *) pParam;
				delete pData;
			}

			delete m_pICMPRxQueue2;
			m_pICMPRxQueue2 = 0;
		}
	}
}

boolean CNetworkLayer::ReceiveICMP (void *pBuffer, unsigned *pResultLength,
				    CIPAddress *pSender, CIPAddress *pReceiver)
{
	if (m_pICMPRxQueue2 == 0)
	{
		return FALSE;
	}

	void *pParam;
	assert (pBuffer != 0);
	assert (pResultLength != 0);
	*pResultLength = m_pICMPRxQueue2->Dequeue (pBuffer, &pParam);
	if (*pResultLength == 0)
	{
		return FALSE;
	}

	TNetworkPrivateData *pData = (TNetworkPrivateData *) pParam;
	assert (pData != 0);

	assert (pData->nProtocol == IPPROTO_ICMP);

	assert (pSender != 0);
	pSender->Set (pData->SourceAddress);

	assert (pReceiver != 0);
	pReceiver->Set (pData->DestinationAddress);

	delete pData;
	pData = 0;

	return TRUE;
}

void CNetworkLayer::AddRoute (const u8 *pDestIP, const u8 *pGatewayIP)
{
	m_RouteCache.AddRoute (pDestIP, pGatewayIP);
}

const u8 *CNetworkLayer::GetGateway (const u8 *pDestIP) const
{
	const u8 *pGateway = m_RouteCache.GetRoute (pDestIP);
	if (pGateway != 0)
	{
		return pGateway;
	}

	assert (m_pNetConfig != 0);
	const CIPAddress *pDefaultGateway = m_pNetConfig->GetDefaultGateway ();
	assert (pDefaultGateway != 0);

	return pDefaultGateway->Get ();
}

void CNetworkLayer::SendFailed (unsigned nICMPCode, const void *pReturnedPacket, unsigned nLength)
{
	assert (m_pICMPHandler != 0);
	m_pICMPHandler->DestinationUnreachable (nICMPCode, pReturnedPacket, nLength);
}

void CNetworkLayer::NotifyJoinGroup(const CIPAddress &rGroupAddress)
{
	if (m_pIGMPHandler != 0)
	{
		m_pIGMPHandler->JoinGroup(rGroupAddress);
	}
}

void CNetworkLayer::NotifyLeaveGroup(const CIPAddress &rGroupAddress)
{
	if (m_pIGMPHandler != 0)
	{
		m_pIGMPHandler->LeaveGroup(rGroupAddress);
	}
}
