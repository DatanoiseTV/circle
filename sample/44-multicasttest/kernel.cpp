// sample/44-multicasttest/kernel.cpp
#include "kernel.h"
#include <circle/util.h> // for Sleep
#include <circle/net/socket.h> // For CSocket if we were to use it

#define NETWORK_INIT_RETRIES 10
#define NETWORK_INIT_DELAY   CTimeDuration(1, 0) // 1 second

CKernel::CKernel (void) :
	m_ActLED (m_Options.GetActLEDMode ()),
	m_Options (m_Logger), // Initialize m_Options before m_Interrupt and m_Timer
	m_Interrupt (m_Options.GetIrqChip (), m_Options.GetIrqUnhandled ()),
	m_Timer (&m_Interrupt), // Initialize m_Timer after m_Interrupt
	m_Logger (m_Options.GetLogLevel (), &m_Timer),
	m_Scheduler(), // Initialize Scheduler
	m_pNetSubSystem (0),
	m_pUDPConnection (0),
	m_Mode (MulticastModeReceiver), // Default to receiver
	m_bRunning (FALSE),
	m_pNetworkThread (0)
{
	// Constructor body
}

CKernel::~CKernel (void)
{
	if (m_pNetworkThread != 0)
	{
		m_bRunning = FALSE;
		m_pNetworkThread->WakeUp(); // Wake it up if it's sleeping/waiting
		// Thread should self-terminate. Consider CThread::Cancel and join if needed.
		// For simplicity, we let it exit its loop.
	}
	
	if (m_pUDPConnection != 0)
	{
		if (m_pUDPConnection->IsMulticastConnection())
		{
			m_Logger.Write("Kernel", LogNotice, "Leaving multicast group %s", m_MulticastGroupIP.GetText());
			m_pUDPConnection->LeaveMulticastGroup(m_MulticastGroupIP);
		}
		m_pUDPConnection->Close(); // Important to close the connection
		// CNetSubSystem should handle deletion of connections it creates,
		// or they should be deleted if created with 'new' outside of it.
		// For connections from NewUDPConnection, Close is usually enough, NetSubSystem cleans up.
	}

	if (m_pNetSubSystem != 0)
	{
		m_pNetSubSystem->Cleanup (); // Clean up network subsystem
	}
	
	m_pNetworkThread = 0;
	m_pUDPConnection = 0;
	m_pNetSubSystem = 0;
}

boolean CKernel::Initialize (void)
{
	if (!CKernelStub::Initialize ()) return FALSE;
	if (!m_Options.Initialize ()) return FALSE;
	if (!m_Logger.Initialize ()) return FALSE; // Initialize Logger first
	if (!m_Interrupt.Initialize ()) return FALSE;
	if (!m_Timer.Initialize ()) return FALSE;
	if (!m_DeviceNameService.Initialize ()) return FALSE;
	if (!m_Scheduler.Initialize ()) return FALSE; // Initialize Scheduler

	// Parse command line options
	CString ModeString = m_Options.Get()->GetOption("multicast.mode");
	if (!ModeString.IsEmpty() && ModeString.Compare("sender") == 0) {
		m_Mode = MulticastModeSender;
		m_Logger.Write("Kernel", LogNotice, "Mode: Sender");
	} else {
		m_Mode = MulticastModeReceiver;
		m_Logger.Write("Kernel", LogNotice, "Mode: Receiver");
	}

	if (!m_MulticastGroupIP.FromString(MULTICAST_IP_STR)) {
		m_Logger.Write("Kernel", LogError, "Failed to parse multicast IP string: %s", MULTICAST_IP_STR);
		return FALSE;
	}
	
	// Initialize Network Subsystem
	m_pNetSubSystem = new CNetSubSystem (&m_Interrupt, &m_Timer, &m_DeviceNameService, &m_Logger);
	if (!m_pNetSubSystem || !m_pNetSubSystem->Initialize (m_Options.Get())) {
		m_Logger.Write ("Kernel", LogError, "Cannot initialize network subsystem");
		return FALSE;
	}

	// Wait for network to be up (simplified)
	unsigned nRetries = 0;
	while (!m_pNetSubSystem->IsRunning() && nRetries < NETWORK_INIT_RETRIES) {
		m_Logger.Write("Kernel", LogNotice, "Waiting for network... (%u/%u)", nRetries + 1, NETWORK_INIT_RETRIES);
		m_Timer.MsDelay(NETWORK_INIT_DELAY.GetMilliSeconds());
		m_pNetSubSystem->Process(); // Give it time to process DHCP etc.
		nRetries++;
	}
	if (!m_pNetSubSystem->IsRunning()) {
		m_Logger.Write("Kernel", LogError, "Network subsystem did not start.");
		return FALSE;
	}
	m_Logger.Write("Kernel", LogNotice, "Network is up. IP: %s", m_pNetSubSystem->GetConfig()->GetIPAddress()->GetText());


	// Create UDP Connection
	// For receiving, bind to INADDR_ANY and the multicast port.
	// For sending, a specific bind isn't strictly necessary unless specifying source interface/port.
	m_pUDPConnection = m_pNetSubSystem->NewUDPConnection(MULTICAST_PORT);
	if (!m_pUDPConnection) {
		m_Logger.Write("Kernel", LogError, "Failed to create UDP connection on port %u", MULTICAST_PORT);
		return FALSE;
	}
	m_Logger.Write("Kernel", LogNotice, "UDP Connection created on port %u", MULTICAST_PORT);

	// Create and start the network thread
	m_pNetworkThread = new CThread(&m_Scheduler, NetworkThreadEntry, this, CThread::HIGHEST_PRIORITY - 10);
	if (!m_pNetworkThread) {
	    m_Logger.Write("Kernel", LogError, "Failed to create network thread.");
	    return FALSE;
	}
	
	m_bRunning = TRUE; // Set running flag before starting thread
	m_pNetworkThread->Run();

	return TRUE;
}

TShutdownMode CKernel::Run (void)
{
	m_Logger.Write ("Kernel", LogNotice, "Circle Multicast Test Sample (Build %s %s)", __DATE__, __TIME__);
	
	// The main work is done in the NetworkThread and by CNetSubSystem::Process()
	// We just keep the kernel alive and process the network stack periodically.
	while (m_bRunning) {
		m_Scheduler.Yield(); // Allow other threads to run
		if (m_pNetSubSystem) {
			m_pNetSubSystem->Process(); // Process network stack events
		}
		// Minimal delay or specific event wait could be here if Yield isn't enough
		m_Timer.MsSleep(10); // Sleep for a short duration
	}
	
	return ShutdownHalt;
}

void CKernel::NetworkThreadEntry(void *pParam)
{
    CKernel *pKernel = (CKernel *) pParam;
    pKernel->NetworkThread();
}

void CKernel::NetworkThread(void)
{
    m_Logger.Write("Kernel", LogNotice, "NetworkThread started.");

    if (m_Mode == MulticastModeReceiver) {
        StartReceiver();
    } else {
        StartSender();
    }

    m_Logger.Write("Kernel", LogNotice, "NetworkThread finished.");
    m_bRunning = FALSE; // Signal main loop to exit if thread finishes
}

void CKernel::StartReceiver(void)
{
    m_Logger.Write("Kernel", LogNotice, "Receiver mode started. Joining group %s on port %u.", 
                   m_MulticastGroupIP.GetText(), MULTICAST_PORT);

    if (m_pUDPConnection->JoinMulticastGroup(m_MulticastGroupIP) != 0) {
        m_Logger.Write("Kernel", LogError, "Failed to join multicast group %s", m_MulticastGroupIP.GetText());
        return;
    }
    m_Logger.Write("Kernel", LogNotice, "Successfully joined multicast group %s", m_MulticastGroupIP.GetText());

    char buffer[128];
    CIPAddress senderIP;
    u16 senderPort;
    int nBytesReceived;

    while (m_bRunning) {
        // MSG_DONTWAIT is not implemented in this CUDPConnection, it blocks.
        // We need a way to make ReceiveFrom non-blocking or use with timeout,
        // or handle m_bRunning termination differently.
        // For simplicity, this example will block. To stop, you might need to reset.
        // Or add a check to m_bRunning inside CUDPConnection's event wait.
        
        nBytesReceived = m_pUDPConnection->ReceiveFrom(buffer, sizeof(buffer) - 1, 0, &senderIP, &senderPort);
        if (nBytesReceived > 0) {
            buffer[nBytesReceived] = '\0'; // Null-terminate
            m_Logger.Write("Kernel", LogNotice, "Received %d bytes from %s:%u : '%s'",
                           nBytesReceived, senderIP.GetText(), senderPort, buffer);
            m_ActLED.Blink(1); // Blink LED on receive
        } else if (nBytesReceived == 0) {
             // This means MSG_DONTWAIT was used and no data, but our ReceiveFrom blocks.
             // So this case won't be hit with current CUDPConnection.
        } else {
            m_Logger.Write("Kernel", LogError, "ReceiveFrom error: %d", nBytesReceived);
            // Could mean connection closed or other error.
            // If connection closed by peer (not applicable for UDP multicast listen)
            // or error, we might want to break.
            if (m_pUDPConnection->IsTerminated()) {
                 m_Logger.Write("Kernel", LogError, "UDP Connection terminated.");
                 break; 
            }
        }
        // Yield to allow other processing, especially if ReceiveFrom had a timeout (not currently)
        // m_Scheduler.Yield(); // Not strictly needed if ReceiveFrom blocks and is the main loop activity
    }
}

void CKernel::StartSender(void)
{
    m_Logger.Write("Kernel", LogNotice, "Sender mode started. Sending to %s on port %u.",
                   m_MulticastGroupIP.GetText(), MULTICAST_PORT);
    
    CString sMessage;
    unsigned nCount = 0;

    while (m_bRunning) {
        sMessage.Format("Multicast message #%u from Circle", ++nCount);
        
        int nSent = m_pUDPConnection->SendTo(sMessage.Get(), sMessage.GetLength(), 0,
                                           m_MulticastGroupIP, MULTICAST_PORT);
        if (nSent == sMessage.GetLength()) {
            m_Logger.Write("Kernel", LogDebug, "Sent: %s", sMessage.Get());
            m_ActLED.Blink(1); // Blink LED on send
        } else {
            m_Logger.Write("Kernel", LogError, "Failed to send multicast message. Result: %d", nSent);
        }
        
        // Delay for a second
        for (int i=0; i<10 && m_bRunning; ++i) { // Check m_bRunning for faster exit
             m_Timer.MsSleep(100);
        }
    }
}
