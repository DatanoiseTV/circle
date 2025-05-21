// sample/44-multicasttest/kernel.h
#ifndef _CIRCLE_SAMPLE_44_KERNEL_H
#define _CIRCLE_SAMPLE_44_KERNEL_H

#include <circle/kernel.h>
#include <circle/actled.h>
#include <circle/koptions.h>
#include <circle/devicenameservice.h>
#include <circle/exceptionhandler.h>
#include <circle/interrupt.h>
#include <circle/timer.h>
#include <circle/logger.h>
#include <circle/net/netsubsystem.h>
#include <circle/net/ipaddress.h>
#include <circle/net/udpconnection.h>
#include <circle/string.h>
#include <circle/sched/scheduler.h> // For CScheduler
#include <circle/sched/thread.h>    // For CThread

#define LOG_LEVEL LOG_DEBUG // Or LOG_INFO

// Define a multicast group and port for testing
const char MULTICAST_IP_STR[] = "239.1.2.3";
const u16 MULTICAST_PORT = 7777;

// Mode selection via kernel command line (e.g., "multicast.mode=sender")
enum TMulticastMode
{
	MulticastModeReceiver,
	MulticastModeSender
};

class CKernel : public CKernelStub
{
public:
	CKernel (void);
	~CKernel (void);

	boolean Initialize (void);
	TShutdownMode Run (void);

private:
	// Kernel components
	CActLED			m_ActLED;
	CKOptions		m_Options;
	CDeviceNameService	m_DeviceNameService;
	CInterruptSystem	m_Interrupt;
	CTimer			m_Timer;
	CLogger			m_Logger;
	CScheduler		m_Scheduler; // Required for CNetSubSystem and CThread

	// Network components
	CNetSubSystem *m_pNetSubSystem;
	CUDPConnection *m_pUDPConnection;
	CIPAddress m_MulticastGroupIP;

	TMulticastMode m_Mode;
	volatile boolean m_bRunning;

	// Threads for sender/receiver
	CThread *m_pNetworkThread;
	static void NetworkThreadEntry (void *pParam);
	void NetworkThread (void);

	void StartReceiver(void);
	void StartSender(void);
};

#endif // _CIRCLE_SAMPLE_44_KERNEL_H
