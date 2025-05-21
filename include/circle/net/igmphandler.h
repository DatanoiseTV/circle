// include/circle/net/igmphandler.h
#ifndef _CIRCLE_NET_IGMPHANDLER_H
#define _CIRCLE_NET_IGMPHANDLER_H

#include <circle/net/ipaddress.h>
#include <circle/net/netconfig.h>
#include <circle/list.h>
#include <circle/timer.h> // For potential future timer use

// Forward declaration
class CNetworkLayer;

// IGMP Message Types (RFC 2236)
#define IGMP_MEMBERSHIP_QUERY       0x11 // General or Group-Specific
#define IGMP_V2_MEMBERSHIP_REPORT   0x16
#define IGMP_LEAVE_GROUP            0x17
#define IGMP_V3_MEMBERSHIP_REPORT   0x22 // For future, not in scope for now

// Max response delay for queries (in tenths of a second, as per RFC 2236)
#define IGMP_MAX_RESPONSE_DELAY_DEFAULT 100 // 10 seconds

struct TIGMPHeader // Basic IGMPv2 Header
{
	u8	nType;
	u8	nMaxRespTime; // Max Response Time in 1/10 sec (for Queries) or 0 (for Reports/Leave)
	u16	nChecksum;
	u8	GroupAddress[IP_ADDRESS_SIZE]; // Multicast Group Address, 0 for General Query
}
__attribute__((packed));


class CIGMPHandler : public CTimerListener // Inherit from CTimerListener for future timer needs
{
public:
	CIGMPHandler (CNetConfig *pNetConfig, CNetworkLayer *pNetworkLayer);
	~CIGMPHandler (void);

	boolean Initialize (void);

	// Called by CNetworkLayer when an IGMP packet (IP Protocol 2) is received
	void ProcessPacket (const void *pPacket, unsigned nLength, const CIPAddress &rSenderIP);

	// Called by CUDPConnection (via CNetworkLayer)
	void JoinGroup (const CIPAddress &rGroupAddress);
	void LeaveGroup (const CIPAddress &rGroupAddress);

	// CTimerListener method (for future use, e.g., query responses, periodic reports)
	void TimerHandler (void) override;

private:
	struct TGroupMembership
	{
		CIPAddress	Address;
		// Future: add state, timers for reporting, etc.
		// For IGMPv2, state is simpler: member or non-member.
		// Routers keep detailed state.
	};

	CNetConfig    *m_pNetConfig;
	CNetworkLayer *m_pNetworkLayer;
	CList<TGroupMembership *> m_JoinedGroups;
	CTimer m_Timer; // General purpose timer for IGMP actions

	// Members for scheduled report management
	boolean    m_bReportScheduled;      // True if a report is currently scheduled
	CIPAddress m_ScheduledGroupAddress; // Group for which report is scheduled

	TGroupMembership* FindGroup (const CIPAddress &rGroupAddress) const;

	void SendMembershipReport (const CIPAddress &rGroupAddress, boolean bUnsolicited);
	void SendLeaveGroup (const CIPAddress &rGroupAddress);
};

#endif // _CIRCLE_NET_IGMPHANDLER_H
