// lib/net/igmphandler.cpp
#include <circle/net/igmphandler.h>
#include <circle/net/networklayer.h> // For sending packets eventually
#include <circle/net/checksumcalculator.h>
#include <circle/logger.h>
#include <circle/util.h> // for memset, memcpy
#include <assert.h>

static const char LogTag[] = "IGMP";

CIGMPHandler::CIGMPHandler (CNetConfig *pNetConfig, CNetworkLayer *pNetworkLayer)
:	m_pNetConfig (pNetConfig),
	m_pNetworkLayer (pNetworkLayer),
	m_hKernelTimer (0), // Initialize kernel timer handle
	m_bReportScheduled (FALSE)
{
	assert (m_pNetConfig != 0);
	assert (m_pNetworkLayer != 0);
	// m_ScheduledGroupAddress is default constructed (invalid)
}

CIGMPHandler::~CIGMPHandler (void)
{
	if (m_hKernelTimer != 0)
	{
		CTimer::Get()->CancelKernelTimer(m_hKernelTimer);
		m_hKernelTimer = 0;
	}

	while (!m_JoinedGroups.IsEmpty ())
	{
		delete m_JoinedGroups.GetFirst ();
		m_JoinedGroups.RemoveFirst ();
	}

	m_pNetworkLayer = 0;
	m_pNetConfig = 0;
}

boolean CIGMPHandler::Initialize (void)
{
	// Future: Potentially send initial unsolicited reports for any pre-configured groups
	// For now, just ensure the state is clean.
	CLogger::Get ()->Write (LogTag, LogDebug, "Initialized");
	return TRUE;
}

CIGMPHandler::TGroupMembership* CIGMPHandler::FindGroup (const CIPAddress &rGroupAddress) const
{
	for (unsigned i = 0; i < m_JoinedGroups.GetCount (); i++)
	{
		TGroupMembership *pGroup = m_JoinedGroups[i];
		if (pGroup->Address == rGroupAddress)
		{
			return pGroup;
		}
	}
	return 0;
}

void CIGMPHandler::JoinGroup (const CIPAddress &rGroupAddress)
{
	if (!rGroupAddress.IsMulticast() /* || rGroupAddress.IsLinkLocalMulticast() */) // 224.0.0.x range is typically not reported
	{
		CLogger::Get()->Write(LogTag, LogWarning, "JoinGroup: Invalid or link-local group %s", rGroupAddress.GetText());
		return;
	}

	if (FindGroup (rGroupAddress))
	{
		CLogger::Get()->Write(LogTag, LogDebug, "JoinGroup: Already member of %s", rGroupAddress.GetText());
		// TODO: Handle re-joining logic if necessary (e.g., state refresh)
		return;
	}

	TGroupMembership *pNewGroup = new TGroupMembership;
	assert(pNewGroup != 0);
	pNewGroup->Address = rGroupAddress;
	// Initialize other state for pNewGroup if added later

	m_JoinedGroups.Append (pNewGroup);
	CLogger::Get()->Write(LogTag, LogInfo, "Joined group %s", rGroupAddress.GetText());

	// TODO: Send IGMP V2 Membership Report
	SendMembershipReport(rGroupAddress, TRUE); // TRUE for unsolicited
}

void CIGMPHandler::LeaveGroup (const CIPAddress &rGroupAddress)
{
	if (!rGroupAddress.IsMulticast())
	{
		CLogger::Get()->Write(LogTag, LogWarning, "LeaveGroup: Invalid group %s", rGroupAddress.GetText());
		return;
	}
	
	TGroupMembership *pGroup = FindGroup (rGroupAddress);
	if (!pGroup)
	{
		CLogger::Get()->Write(LogTag, LogDebug, "LeaveGroup: Not a member of %s", rGroupAddress.GetText());
		return;
	}

	SendLeaveGroup(rGroupAddress); // Send IGMP Leave

	if (m_bReportScheduled && m_ScheduledGroupAddress == rGroupAddress)
	{
		if (m_hKernelTimer != 0)
		{
			CLogger::Get()->Write(LogTag, LogDebug, "Leaving group %s, cancelling its scheduled report", rGroupAddress.GetText());
			CTimer::Get()->CancelKernelTimer(m_hKernelTimer);
			m_hKernelTimer = 0;
		}
		m_bReportScheduled = FALSE;
		m_ScheduledGroupAddress = CIPAddress(); // Invalidate
	}

	m_JoinedGroups.Remove(pGroup);
	delete pGroup;
	CLogger::Get()->Write(LogTag, LogInfo, "Left group %s", rGroupAddress.GetText());
}

void CIGMPHandler::ProcessPacket (const void *pPacket, unsigned nLength, const CIPAddress &rSenderIP)
{
	if (nLength < sizeof(TIGMPHeader))
	{
		CLogger::Get()->Write(LogTag, LogWarning, "ProcessPacket: Packet too short from %s", rSenderIP.GetText());
		return;
	}

	const TIGMPHeader *pHeader = (const TIGMPHeader *) pPacket;

	// Verify checksum
	TIGMPHeader testHeader = *pHeader; // Make a copy to zero out checksum field for calculation
	testHeader.nChecksum = 0;
	// The nLength here is the length of the IGMP payload from IP layer's perspective
	u16 calculated_checksum = CChecksumCalculator::SimpleCalculate(&testHeader, nLength); 
	if (pHeader->nChecksum != calculated_checksum) {
		CLogger::Get()->Write(LogTag, LogWarning, "ProcessPacket: Invalid checksum from %s. Expected 0x%04X, got 0x%04X", 
								rSenderIP.GetText(), calculated_checksum, pHeader->nChecksum);
		return; // Drop packet
	}

	switch (pHeader->nType)
	{
		case IGMP_MEMBERSHIP_QUERY:
		{
			CIPAddress groupQueryAddress(pHeader->GroupAddress);
			u16 maxRespTimeTenths = pHeader->nMaxRespTime;
			if (maxRespTimeTenths == 0) { // As per RFC 2236, if 0, use default 10 seconds
				maxRespTimeTenths = IGMP_MAX_RESPONSE_DELAY_DEFAULT;
			}
			u32 maxRespTimeMs = maxRespTimeTenths * 100; // Convert tenths of a second to milliseconds

			CLogger::Get()->Write(LogTag, LogDebug, "Query for %s, MaxRespTime: %ums (raw value: %u)",
								groupQueryAddress.IsNull() ? "ALL" : groupQueryAddress.GetText(), maxRespTimeMs, pHeader->nMaxRespTime);

			if (m_bReportScheduled) {
				 CLogger::Get()->Write(LogTag, LogDebug, "Report already scheduled, ignoring new query for now.");
				 // TODO: More sophisticated logic could update the schedule or queue reports.
				 // For example, if this query is for the same group and MaxRespTime is shorter, update timer.
				 // If for a different group, might need to queue or make a choice.
				 break; 
			}

			if (groupQueryAddress.IsNull()) // General Query
			{
				if (!m_JoinedGroups.IsEmpty()) {
					// Schedule report for the first group in our list (simplification)
					TGroupMembership *pGroupToReport = m_JoinedGroups.GetFirst();
					if (pGroupToReport->Address.IsLinkLocalMulticast()) { // 224.0.0.x range is not reported
						CLogger::Get()->Write(LogTag, LogDebug, "General Query: Skipping link-local group %s for report.", pGroupToReport->Address.GetText());
						// Try to find a non-link-local group
						boolean foundReportable = FALSE;
						for (unsigned i = 0; i < m_JoinedGroups.GetCount(); ++i) {
							if (!m_JoinedGroups[i]->Address.IsLinkLocalMulticast()) {
								pGroupToReport = m_JoinedGroups[i];
								foundReportable = TRUE;
								break;
							}
						}
						if (!foundReportable) break; // No reportable groups
					}
					m_ScheduledGroupAddress = pGroupToReport->Address;
				} else {
					CLogger::Get()->Write(LogTag, LogDebug, "General Query: No groups joined, nothing to report.");
					break; // No groups joined, nothing to report
				}
			}
			else // Group-Specific Query
			{
				if (groupQueryAddress.IsLinkLocalMulticast()) { // 224.0.0.x range is not reported
					CLogger::Get()->Write(LogTag, LogDebug, "Group-Specific Query: Skipping link-local group %s.", groupQueryAddress.GetText());
					break;
				}
				TGroupMembership *pGroup = FindGroup(groupQueryAddress);
				if (pGroup != 0) {
					m_ScheduledGroupAddress = pGroup->Address;
				} else {
					CLogger::Get()->Write(LogTag, LogDebug, "Group-Specific Query: Not a member of %s.", groupQueryAddress.GetText());
					break; // Not a member of the queried group
				}
			}

			if (m_ScheduledGroupAddress.IsSet() && m_ScheduledGroupAddress.IsMulticast()) {
				// Cancel any previously scheduled timer for reporting.
				if (m_hKernelTimer != 0) {
					CTimer::Get()->CancelKernelTimer(m_hKernelTimer);
					m_hKernelTimer = 0; // Important: clear handle after cancelling
				}

				u32 randomDelayMs = 0;
				if (maxRespTimeMs > 0) {
					 randomDelayMs = CScheduler::GetRandomNumber() % maxRespTimeMs;
					 // Ensure a minimum delay if maxRespTimeMs is very small but non-zero,
					 // or if random resulted in 0 but we want some delay.
					 // HZ is 100. Smallest tick delay is 1 (10ms).
					 if (randomDelayMs < 10 && maxRespTimeMs >=10 ) randomDelayMs = 10; // Min 10ms if possible
					 else if (randomDelayMs == 0 && maxRespTimeMs > 0) randomDelayMs = 1; // Or ensure at least 1ms for calc
				}
				
				unsigned nDelayTicks = MSEC2HZ(randomDelayMs);
				if (nDelayTicks == 0 && randomDelayMs > 0) { // Ensure at least 1 tick if there's any delay
					nDelayTicks = 1;
				}
				// If maxRespTimeMs was 0, nDelayTicks will be 0, meaning report ASAP.
				// StartKernelTimer with nDelay=0 should be immediate or next tick.

				CLogger::Get()->Write(LogTag, LogDebug, "Scheduling report for %s in %u ms (%u ticks)", 
									  m_ScheduledGroupAddress.GetText(), randomDelayMs, nDelayTicks);
				
				m_hKernelTimer = CTimer::Get()->StartKernelTimer(nDelayTicks, 
																 StaticTimerHandler, 
																 this,  // pParam for StaticTimerHandler
																 0);    // pContext (unused here)
				if (m_hKernelTimer == 0) { // Check if timer start failed (should not happen if params are valid)
					 CLogger::Get()->Write(LogTag, LogError, "Failed to start kernel timer for %s", m_ScheduledGroupAddress.GetText());
					 m_bReportScheduled = FALSE; // Don't consider it scheduled
				} else {
					 m_bReportScheduled = TRUE;
					 // m_ScheduledGroupAddress is already set prior to this block
				}
			}
			break;
		}

		case IGMP_V2_MEMBERSHIP_REPORT:
			CLogger::Get()->Write(LogTag, LogDebug, "Received Membership Report from %s (ignoring)", rSenderIP.GetText());
			// Hosts generally don't act on other hosts' reports, routers do.
			break;

		case IGMP_LEAVE_GROUP:
			CLogger::Get()->Write(LogTag, LogDebug, "Received Leave Group from %s (ignoring)", rSenderIP.GetText());
			// Hosts generally don't act on other hosts' leave messages.
			break;
			
		default:
			CLogger::Get()->Write(LogTag, LogWarning, "Received unknown IGMP type 0x%02X from %s", pHeader->nType, rSenderIP.GetText());
			break;
	}
}

void CIGMPHandler::SendMembershipReport (const CIPAddress &rGroupAddress, boolean bUnsolicited)
{
	if (!m_pNetConfig->GetIPAddress()->IsSet()) // Cannot send if we don't have an IP
	{
		CLogger::Get()->Write(LogTag, LogWarning, "Cannot send report for %s, no local IP", rGroupAddress.GetText());
		return;
	}

	TIGMPHeader igmp_packet;
	memset(&igmp_packet, 0, sizeof(TIGMPHeader)); // Important to zero out, esp. for checksum
	igmp_packet.nType = IGMP_V2_MEMBERSHIP_REPORT;
	igmp_packet.nMaxRespTime = 0; // Must be 0 for reports
	rGroupAddress.CopyTo(igmp_packet.GroupAddress);
	// igmp_packet.nChecksum is already 0 due to memset

	// IGMP checksum is calculated over the IGMP message only
	igmp_packet.nChecksum = CChecksumCalculator::SimpleCalculate(&igmp_packet, sizeof(TIGMPHeader));

	// Destination IP for report is the group address itself
	// CNetworkLayer::Send handles setting TTL to 1 if destination is multicast
	if (m_pNetworkLayer->Send(rGroupAddress, &igmp_packet, sizeof(TIGMPHeader), IPPROTO_IGMP))
	{
		CLogger::Get()->Write(LogTag, LogInfo, "Sent V2 Membership Report for %s (Unsolicited: %d)", rGroupAddress.GetText(), bUnsolicited);
	}
	else
	{
		CLogger::Get()->Write(LogTag, LogError, "Failed to send V2 Membership Report for %s", rGroupAddress.GetText());
	}
}

void CIGMPHandler::SendLeaveGroup (const CIPAddress &rGroupAddress)
{
	if (!m_pNetConfig->GetIPAddress()->IsSet())
	{
		CLogger::Get()->Write(LogTag, LogWarning, "Cannot send leave for %s, no local IP", rGroupAddress.GetText());
		return;
	}

	TIGMPHeader igmp_packet;
	memset(&igmp_packet, 0, sizeof(TIGMPHeader)); // Important to zero out
	igmp_packet.nType = IGMP_LEAVE_GROUP;
	igmp_packet.nMaxRespTime = 0; // Must be 0 for leave
	rGroupAddress.CopyTo(igmp_packet.GroupAddress); // Group being left
	// igmp_packet.nChecksum is already 0

	igmp_packet.nChecksum = CChecksumCalculator::SimpleCalculate(&igmp_packet, sizeof(TIGMPHeader));

	// Destination IP for leave is ALL-ROUTERS (224.0.0.2)
	// CIPAddress constructor (u32 nAddress) expects nAddress = A | (B<<8) | (C<<16) | (D<<24)
	// For 224.0.0.2 (A.B.C.D): A=224 (0xE0), B=0, C=0, D=2 (0x02)
	// u32 val = 0xE0 | (0<<8) | (0<<16) | (0x02<<24) = 0x020000E0
	CIPAddress AllRoutersIP(0x020000E0); 

	if (m_pNetworkLayer->Send(AllRoutersIP, &igmp_packet, sizeof(TIGMPHeader), IPPROTO_IGMP))
	{
		CLogger::Get()->Write(LogTag, LogInfo, "Sent V2 Leave Group for %s", rGroupAddress.GetText());
	}
	else
	{
		CLogger::Get()->Write(LogTag, LogError, "Failed to send V2 Leave Group for %s", rGroupAddress.GetText());
	}
}

void CIGMPHandler::TimerHandler(void)
{
	if (m_bReportScheduled && m_ScheduledGroupAddress.IsSet() && m_ScheduledGroupAddress.IsMulticast())
	{
		CLogger::Get()->Write(LogTag, LogDebug, "Timer fired: Sending scheduled report for %s", m_ScheduledGroupAddress.GetText());
		SendMembershipReport(m_ScheduledGroupAddress, FALSE); // FALSE because it's a solicited report (response to query)
	}
	m_bReportScheduled = FALSE;
	m_ScheduledGroupAddress = CIPAddress(); // Invalidate to show it's handled or no longer scheduled
	// To handle more groups for general queries: 
	// Here one could re-scan m_JoinedGroups, find the next group that needs reporting (if any),
	// calculate a new random delay for it, and set m_Timer again.
	// For this simplified version, we only handle one report per query.
}

void CIGMPHandler::StaticTimerHandler(TKernelTimerHandle hTimer, void *pParam, void *pContext)
{
	CIGMPHandler *pThis = (CIGMPHandler *) pParam;
	assert(pThis != 0);

	// Optional: Check if hTimer matches pThis->m_hKernelTimer if needed,
	// though for a single pending timer per instance, it should.
	// if (hTimer != pThis->m_hKernelTimer) { return; }

	pThis->InstanceTimerHandler();
	pThis->m_hKernelTimer = 0; // Timer has fired and is done (kernel timers are one-shot unless re-added)
}

void CIGMPHandler::InstanceTimerHandler(void)
{
	if (m_bReportScheduled && m_ScheduledGroupAddress.IsSet() && m_ScheduledGroupAddress.IsMulticast())
	{
		CLogger::Get()->Write(LogTag, LogDebug, "Timer fired: Sending scheduled report for %s", m_ScheduledGroupAddress.GetText());
		SendMembershipReport(m_ScheduledGroupAddress, FALSE); // FALSE because it's a solicited report (response to query)
	}
	m_bReportScheduled = FALSE;
	m_ScheduledGroupAddress = CIPAddress(); // Invalidate to show it's handled or no longer scheduled
	// To handle more groups for general queries: 
	// Here one could re-scan m_JoinedGroups, find the next group that needs reporting (if any),
	// calculate a new random delay for it, and set m_Timer again.
	// For this simplified version, we only handle one report per query.
	// m_hKernelTimer is cleared by StaticTimerHandler after this returns
}
