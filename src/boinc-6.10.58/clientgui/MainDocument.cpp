// This file is part of BOINC.
// http://boinc.berkeley.edu
// Copyright (C) 2008 University of California
//
// BOINC is free software; you can redistribute it and/or modify it
// under the terms of the GNU Lesser General Public License
// as published by the Free Software Foundation,
// either version 3 of the License, or (at your option) any later version.
//
// BOINC is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
// See the GNU Lesser General Public License for more details.
//
// You should have received a copy of the GNU Lesser General Public License
// along with BOINC.  If not, see <http://www.gnu.org/licenses/>.

#if defined(__GNUG__) && !defined(__APPLE__)
#pragma implementation "MainDocument.h"
#endif

#include "stdwx.h"

#include "error_numbers.h"
#include "util.h"
#ifdef _WIN32
#include "proc_control.h"
#endif

#include "BOINCGUIApp.h"
#include "MainDocument.h"
#include "BOINCBaseFrame.h"
#include "AdvancedFrame.h"
#include "sg_BoincSimpleGUI.h"
#include "BOINCClientManager.h"
#include "BOINCTaskBar.h"
#include "Events.h"

#ifndef _WIN32
#include <sys/wait.h>
#endif

#ifdef SANDBOX
#include <grp.h>
#endif

#define USE_CACHE_TIMEOUTS 0

// *** RPC update intervals in seconds ***
// m_dtCachedCCStatusTimestamp
#define CCSTATUS_RPC_INTERVAL 1

//m_dtCachedStateTimestamp
#define STATERPC_INTERVAL 3600

//m_dtProjecStatusTimestamp
#define PROJECTSTATUSRPC_INTERVAL 1

//m_dtResultsTimestamp
#define RESULTSRPC_INTERVAL 1

//m_dtFileTransfersTimestamp
#define FILETRANSFERSRPC_INTERVAL 1

// m_dtStatisticsStatusTimestamp
#define STATISTICSSTATUSRPC_INTERVAL 60

// m_dtDiskUsageTimestamp
#define DISKUSAGERPC_INTERVAL 60

// m_dtCachedSimpleGUITimestamp
#define CACHEDSIMPLEGUIRPC_INTERVAL 1

// m_dtCachedAcctMgrInfoTimestamp
#define CACHEDACCTMGRINFORPC_INTERVAL 600

// m_dtLasAsyncRPCDlgTime
#define DELAYAFTERASYNCRPC_DLG 1

bool g_use_sandbox = false;

using std::string;

CNetworkConnection::CNetworkConnection(CMainDocument* pDocument) :
    wxObject() {
    m_pDocument = pDocument;

    m_strConnectedComputerName = wxEmptyString;
    m_strConnectedComputerPassword = wxEmptyString;
    m_strNewComputerName = wxEmptyString;
    m_strNewComputerPassword = wxEmptyString;
    m_bFrameShutdownDetected = false;
    m_bConnectEvent = false;
    m_bConnected = false;
    m_bReconnecting = false;
    m_bForceReconnect = false;
    m_bReconnectOnError = false;
    m_bNewConnection = false;
    m_bUsedDefaultPassword = false;
    m_iPort = GUI_RPC_PORT,
    m_iReadGUIRPCAuthFailure = 0;
 }


CNetworkConnection::~CNetworkConnection() {
}


int CNetworkConnection::GetLocalPassword(wxString& strPassword){
    char buf[256];
    strcpy(buf, "");

    FILE* f = fopen("gui_rpc_auth.cfg", "r");
    if (!f) return errno;
    fgets(buf, 256, f);
    fclose(f);
    int n = (int)strlen(buf);
    if (n) {
        n--;
        if (buf[n]=='\n') {
            buf[n] = 0;
        }
    }

    strPassword = wxString(buf, wxConvUTF8);
    return 0;
}


void CNetworkConnection::Poll() {
    int retval;
    wxString strComputer = wxEmptyString;
    wxString strComputerPassword = wxEmptyString;

    if (IsReconnecting()) {
        wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - Reconnection Detected"));
        retval = m_pDocument->rpcClient.init_poll();
        if (!retval) {
            wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - init_poll() returned ERR_CONNECT, now authorizing..."));

            // Wait until we can establish a connection to the core client before reading
            //   the password so that the client has time to create one when it needs to.
            if (m_bUseDefaultPassword) {
                m_iReadGUIRPCAuthFailure = 0;
                m_bUseDefaultPassword = FALSE;
                m_bUsedDefaultPassword = true;

                m_iReadGUIRPCAuthFailure = GetLocalPassword(m_strNewComputerPassword);
            }

            retval = m_pDocument->rpc.authorize(m_strNewComputerPassword.mb_str());
            if (!retval) {
                wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - Connection Success"));
                SetStateSuccess(m_strNewComputerName, m_strNewComputerPassword);
            } else if (ERR_AUTHENTICATOR == retval) {
                wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - RPC Authorization - ERR_AUTHENTICATOR"));
                SetStateErrorAuthentication();
            } else {
                wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - RPC Authorization Failed '%d'"), retval);
                SetStateError();
            }
            m_bUsedDefaultPassword = false;
        } else if (ERR_RETRY != retval) {
            wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - RPC Connection Failed '%d'"), retval);
            SetStateError();
        }
    } else if (IsConnectEventSignaled() || m_bReconnectOnError) {
        if ((m_bForceReconnect) || (!IsConnected() && m_bReconnectOnError)) {
            wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - Resetting Document State"));
            m_pDocument->ResetState();
            wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - Setting connection state to reconnecting"));
            SetStateReconnecting();
        }

        if (!IsConnected()) {
            // determine computer name and password to use.
            // NOTE: Initial connection case.
            if (!m_strNewComputerName.empty()) {
                strComputer = m_strNewComputerName;
                strComputerPassword = m_strNewComputerPassword;
            } else {
                // NOTE: Reconnect after a disconnect case.
                //       Values are stored after the first successful connect to the host.
                //       See: SetStateSuccess()
                if (!m_strConnectedComputerName.empty()) {
                    strComputer = m_strConnectedComputerName;
                    strComputerPassword = m_strConnectedComputerPassword;
                }
            }

            // a host value of NULL is special cased as binding to the localhost and
            //   if we are connecting to the localhost we need to retry the connection
            //   for awhile so that the users can respond to firewall prompts.
            //
            // use a timeout of 60 seconds so that slow machines do not get a
            //   timeout event right after boot-up.
            //
            if (IsComputerNameLocal(strComputer)) {
                retval = m_pDocument->rpcClient.init_asynch(NULL, 60.0, true, m_iPort);
            } else {
                retval = m_pDocument->rpcClient.init_asynch(strComputer.mb_str(), 60.0, false, m_iPort);
            }

            if (!retval) {
                wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - RPC Initialization Called"));
            } else {
                wxLogTrace(wxT("Function Status"), wxT("CNetworkConnection::Poll - RPC Initialization Failed '%d'"), retval);
                SetStateError();
            }
        }
    }
}


int CNetworkConnection::FrameShutdownDetected() {
    m_bFrameShutdownDetected = true;
    return 0;
}

int CNetworkConnection::GetConnectedComputerName(wxString& strMachine) {
    strMachine = m_strConnectedComputerName;
    return 0;
}


int CNetworkConnection::GetConnectedComputerVersion(wxString& strVersion) {
    strVersion = m_strConnectedComputerVersion;
    return 0;
}


int CNetworkConnection::GetConnectingComputerName(wxString& strMachine) {
    strMachine = m_strNewComputerName;
    return 0;
}


bool CNetworkConnection::IsComputerNameLocal(const wxString& strMachine) {
    static wxString strHostName = wxEmptyString;
    static wxString strFullHostName = wxEmptyString;

    if (strHostName.empty()) {
        strHostName = ::wxGetHostName().Lower();
    }
    if (strFullHostName.empty()) {
        strFullHostName = ::wxGetFullHostName().Lower();
    }

    if (strMachine.empty()) {
        return true;
    } else if (wxT("localhost") == strMachine.Lower()) {
        return true;
    } else if (wxT("localhost.localdomain") == strMachine.Lower()) {
        return true;
    } else if (strHostName == strMachine.Lower()) {
        return true;
    } else if (strFullHostName == strMachine.Lower()) {
        return true;
    }
    return false;
}


int CNetworkConnection::SetComputer(
    const wxChar* szComputer, const int iPort, const wxChar* szPassword,
    const bool bUseDefaultPassword
) {
    m_strNewComputerName.Empty();
    m_strNewComputerPassword.Empty();
    m_bUseDefaultPassword = FALSE;

    m_bNewConnection = true;
    m_strNewComputerName = szComputer;
    m_iPort = iPort;
    m_strNewComputerPassword = szPassword;
    m_bUseDefaultPassword = bUseDefaultPassword;
    return 0;
}


void CNetworkConnection::SetStateErrorAuthentication() {
    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (pFrame && !m_bFrameShutdownDetected) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
        m_bConnected = false;
        m_bReconnecting = false;
        m_bReconnectOnError = false;

        m_bConnectEvent = false;

        pFrame->ShowConnectionBadPasswordAlert(m_bUsedDefaultPassword, m_iReadGUIRPCAuthFailure);
    }
}


void CNetworkConnection::SetStateError() {
    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (pFrame && !m_bFrameShutdownDetected) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
        m_bConnected = false;
        m_bReconnecting = false;
        m_bReconnectOnError = false;

        m_bConnectEvent = false;

        pFrame->ShowConnectionFailedAlert();
    }
}


void CNetworkConnection::SetStateReconnecting() {
    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (pFrame && !m_bFrameShutdownDetected) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
        m_bConnected = false;
        m_bReconnectOnError = false;
        m_bForceReconnect = false;
        m_bReconnecting = true;
        if (!m_bNewConnection) {
            m_strNewComputerName = m_strConnectedComputerName;
            m_strNewComputerPassword = m_strConnectedComputerPassword;
        }
        pFrame->FireRefreshView();
    }
}


void CNetworkConnection::SetStateSuccess(wxString& strComputer, wxString& strComputerPassword) {
    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (pFrame && !m_bFrameShutdownDetected) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
        m_bConnected = true;
        m_bReconnecting = false;
        m_bReconnectOnError = true;
        m_strConnectedComputerName = strComputer;
        m_strConnectedComputerPassword = strComputerPassword;
        m_strNewComputerName = wxEmptyString;
        m_strNewComputerPassword = wxEmptyString;
        m_bNewConnection = false;

        // Get the version of the client and cache it
        VERSION_INFO vi;
        m_pDocument->rpc.exchange_versions(vi);
        m_strConnectedComputerVersion.Printf(
            wxT("%d.%d.%d"),
            vi.major, vi.minor, vi.release
        );

        m_bConnectEvent = false;
        m_pDocument->ResetMessageState();

        pFrame->FireConnect();
    }
}


void CNetworkConnection::SetStateDisconnected() {
    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (pFrame && !m_bFrameShutdownDetected) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
        m_bConnected = false;
        m_bReconnecting = false;
        m_pDocument->results.clear();
    }
}


IMPLEMENT_DYNAMIC_CLASS(CMainDocument, wxObject)


CMainDocument::CMainDocument() : rpc(this) {

#ifdef __WIN32__
    int retval;
    WSADATA wsdata;

    retval = WSAStartup(MAKEWORD(1, 1), &wsdata);
    if (retval) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CMainDocument - Winsock Initialization Failure '%d'"), retval);
    }
#endif

    m_bClientStartCheckCompleted = false;

    m_ActiveTasksOnly = false;

    m_fProjectTotalResourceShare = 0.0;

    m_iMessageSequenceNumber = 0;

    m_dtCachedStateTimestamp = wxDateTime((time_t)0);
    m_iGet_state_rpc_result = 0;
    m_iGet_host_info_rpc_result = 0;
    
    m_dtCachedCCStatusTimestamp = wxDateTime((time_t)0);
    m_iGet_status_rpc_result = 0;
    
    m_dtProjecStatusTimestamp = wxDateTime((time_t)0);
    m_iGet_project_status1_rpc_result = -1;
    
    m_dtResultsTimestamp = wxDateTime((time_t)0);
    m_iGet_results_rpc_result = -1;
    
    m_dtKillInactiveGfxTimestamp = wxDateTime((time_t)0);
    m_dtFileTransfersTimestamp = wxDateTime((time_t)0);
    m_iGet_file_transfers_rpc_result = 0;
    
    m_iGet_messages_rpc_result = -1;
    
    m_dtDiskUsageTimestamp = wxDateTime((time_t)0);
    m_iGet_dsk_usage_rpc_result = -1;

    m_dtStatisticsStatusTimestamp = wxDateTime((time_t)0);
    m_iGet_statistics_rpc_result = -1;
    
    m_dtCachedSimpleGUITimestamp = wxDateTime((time_t)0);
    m_iGet_simple_gui2_rpc_result = -1;
    
    m_dtCachedAcctMgrInfoTimestamp = wxDateTime((time_t)0);
    m_iAcct_mgr_info_rpc_result = -1;
    
    m_dtLasAsyncRPCDlgTime = wxDateTime((time_t)0);
}


CMainDocument::~CMainDocument() {
    KillAllRunningGraphicsApps();
#ifdef __WIN32__
    WSACleanup();
#endif
}


int CMainDocument::OnInit() {
    int iRetVal = -1;

    m_pNetworkConnection = new CNetworkConnection(this);
    wxASSERT(m_pNetworkConnection);

    m_pClientManager = new CBOINCClientManager();
    wxASSERT(m_pClientManager);

    m_RPCWaitDlg = NULL;
    m_bWaitingForRPC = false;
    m_bNeedRefresh = false;
    m_bNeedTaskBarRefresh = false;
    m_bRPCThreadIsReady = false;
    m_bShutDownRPCThread = false;
    current_rpc_request.clear();


    m_pRPC_Thread_Mutex = new BOINC_Mutex();
    wxASSERT(m_pRPC_Thread_Mutex);

    m_pRPC_Thread_Condition = new BOINC_Condition(*m_pRPC_Thread_Mutex);
     wxASSERT(m_pRPC_Thread_Condition);
  
    m_pRPC_Request_Mutex = new BOINC_Mutex();
    wxASSERT(m_pRPC_Request_Mutex);

    m_pRPC_Request_Condition = new BOINC_Condition(*m_pRPC_Request_Mutex);
     wxASSERT(m_pRPC_Request_Condition);
  
    m_RPCThread = new RPCThread(this, 
                                m_pRPC_Thread_Mutex, 
                                m_pRPC_Thread_Condition, 
                                m_pRPC_Request_Mutex, 
                                m_pRPC_Request_Condition
                    );
    wxASSERT(m_RPCThread);

    iRetVal = m_RPCThread->Create();
    wxASSERT(!iRetVal);
    
    m_RPCThread->Run();
    for (int i=0; i<100; i++) {
        if (!m_bRPCThreadIsReady) {
            boinc_sleep(0.01);  // Allow RPC thread to initialize itself
        }
    }

    return iRetVal;
}


int CMainDocument::OnExit() {
    int iRetVal = 0;

    if (m_pClientManager) {
        m_pClientManager->ShutdownBOINCCore();

        delete m_pClientManager;
        m_pClientManager = NULL;
    }

    if (m_RPCThread) {
        KillRPCThread();
        m_RPCThread = NULL;
    }
    
    delete m_pRPC_Thread_Mutex;
    m_pRPC_Thread_Mutex = NULL;
    
    delete m_pRPC_Thread_Condition;
    m_pRPC_Thread_Condition = NULL;
    
    delete m_pRPC_Request_Mutex;
    m_pRPC_Request_Mutex = NULL;
    
    delete m_pRPC_Request_Condition;
    m_pRPC_Request_Condition = NULL;
    
    rpcClient.close();

    if (m_pNetworkConnection) {
        delete m_pNetworkConnection;
        m_pNetworkConnection = NULL;
    }

    return iRetVal;
}


int CMainDocument::OnPoll() {
    int iRetVal = 0;

    wxASSERT(wxDynamicCast(m_pClientManager, CBOINCClientManager));
    wxASSERT(wxDynamicCast(m_pNetworkConnection, CNetworkConnection));

    if (!m_bClientStartCheckCompleted) {
        m_bClientStartCheckCompleted = true;

        CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));

        pFrame->UpdateStatusText(_("Starting client services; please wait..."));

        if (m_pClientManager->StartupBOINCCore()) {
            Connect(wxT("localhost"), GUI_RPC_PORT, wxEmptyString, TRUE, TRUE);
        } else {
            m_pNetworkConnection->ForceDisconnect();
            pFrame->ShowDaemonStartFailedAlert();
        }

        pFrame->UpdateStatusText(wxEmptyString);
    }

    // Check connection state, connect if needed.
    m_pNetworkConnection->Poll();

    // Every 10 seconds, kill any running graphics apps 
    // whose associated worker tasks are no longer running
    wxTimeSpan ts(wxDateTime::Now() - m_dtKillInactiveGfxTimestamp);
    if (ts.GetSeconds() > 10) {
        m_dtKillInactiveGfxTimestamp = wxDateTime::Now();
        KillInactiveGraphicsApps();
    }

    return iRetVal;
}


int CMainDocument::OnRefreshState() {
    if (IsConnected()) {
        CachedStateUpdate();
    }

    return 0;
}


int CMainDocument::CachedStateUpdate() {
    // Most of this is now handled by RunPeriodicRPCs() and ForceCacheUpdate()
    int     retval = 0;

    if (m_iGet_state_rpc_result) retval = m_iGet_state_rpc_result;
    if (m_iGet_host_info_rpc_result) retval = m_iGet_host_info_rpc_result;
            
    if (retval) m_pNetworkConnection->SetStateDisconnected();

    return retval;
}


int CMainDocument::ResetState() {
    rpcClient.close();
    state.clear();
    host.clear_host_info();
    results.clear();
    ft.clear();
    statistics_status.clear();
    disk_usage.clear();
    proxy_info.clear();

    ForceCacheUpdate();
    return 0;
}


int CMainDocument::Connect(const wxChar* szComputer, int iPort, const wxChar* szComputerPassword, const bool bDisconnect, const bool bUseDefaultPassword) {
    if (IsComputerNameLocal(szComputer)) {
        // Restart client if not already running
        m_pClientManager->AutoRestart();
    }
   
   if (bDisconnect) {
        m_pNetworkConnection->ForceReconnect();
    }

    m_pNetworkConnection->SetComputer(szComputer, iPort, szComputerPassword, bUseDefaultPassword);
    m_pNetworkConnection->FireReconnectEvent();

    ResetMessageState();
    return 0;
}


int CMainDocument::Reconnect() {
    m_pNetworkConnection->ForceReconnect();
    m_pNetworkConnection->FireReconnectEvent();
    ResetMessageState();
    return 0;
}


int CMainDocument::GetConnectedComputerName(wxString& strMachine) {
    m_pNetworkConnection->GetConnectedComputerName(strMachine);
    return 0;
}


int CMainDocument::GetConnectedComputerVersion(wxString& strVersion) {
    m_pNetworkConnection->GetConnectedComputerVersion(strVersion);
    return 0;
}


int CMainDocument::GetConnectingComputerName(wxString& strMachine) {
    m_pNetworkConnection->GetConnectingComputerName(strMachine);
    return 0;
}


bool CMainDocument::IsComputerNameLocal(const wxString strMachine) {
    return m_pNetworkConnection->IsComputerNameLocal(strMachine);
}


bool CMainDocument::IsConnected() {
    return m_pNetworkConnection->IsConnected();
}


bool CMainDocument::IsReconnecting() {
    return m_pNetworkConnection->IsReconnecting();
}


void CMainDocument::ForceDisconnect() {
    return m_pNetworkConnection->ForceDisconnect();
}


int CMainDocument::FrameShutdownDetected() {
    return m_pNetworkConnection->FrameShutdownDetected();
}


// It is _not_ enough to just reset m_dtCachedCCStatusTimestamp 
// and let RunPeriodicRPCs() update the state for these routines
// (which need immediate results):
//      CMainDocument::SetActivityRunMode()
//      CMainDocument::SetNetworkRunMode()
// Otherwise the Snooze task bar menu item and SimpleGUI Pause 
// button do not work properly.
//
int CMainDocument::GetCoreClientStatus(CC_STATUS& ccs, bool bForce) {
    wxString         strMachine = wxEmptyString;
    int              iRetVal = 0;

    if (IsConnected()) {
        if (!m_bWaitingForRPC) {    // Prevent recursive entry of RequestRPC() 
#if USE_CACHE_TIMEOUTS
            wxTimeSpan ts(wxDateTime::Now() - m_dtCachedCCStatusTimestamp);
            if (ts.GetSeconds() >= (10 * CCSTATUS_RPC_INTERVAL)) bForce = true;
#endif
            if (m_dtCachedCCStatusTimestamp.IsEqualTo(wxDateTime((time_t)0))) bForce = true;
        }
        if (bForce) {
            m_dtCachedCCStatusTimestamp = wxDateTime::Now();

            m_iGet_status_rpc_result = rpc.get_cc_status(ccs);
            if (0 == iRetVal) {
                status = ccs;
            } else {
                iRetVal = m_iGet_status_rpc_result;
            }
        } else {
            ccs = status;
            iRetVal = m_iGet_status_rpc_result;
        }
        
        if (m_iGet_status_rpc_result) {
            m_pNetworkConnection->SetStateDisconnected();
        } else {
            if (ccs.manager_must_quit) {
                GetConnectedComputerName(strMachine);
                if (IsComputerNameLocal(strMachine)) {
                    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
                    wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
                    pFrame->Close(true);
                }
            }
        }
    } else {
        iRetVal = -1;
    }

    return iRetVal;
}


int CMainDocument::SetActivityRunMode(int iMode, int iTimeout) {
    int       iRetVal = 0;
    CC_STATUS ccs;

    if (IsConnected()) {
        iRetVal = rpc.set_run_mode(iMode, iTimeout);
        if (0 == iRetVal) {
            if (RUN_MODE_RESTORE == iMode) {
                GetCoreClientStatus(ccs, true);
            } else {
                status.task_mode = iMode;
            }
        }
    }

    return iRetVal;
}


int CMainDocument::SetGPURunMode(int iMode, int iTimeout) {
    CC_STATUS ccs;

    if (IsConnected()) {
        int retval = rpc.set_gpu_mode(iMode, iTimeout);
        if (retval) return retval;
        if (iMode == RUN_MODE_RESTORE) {
            GetCoreClientStatus(ccs, true);
        } else {
            status.network_mode = iMode;
        }
    }
    return 0;
}

int CMainDocument::SetNetworkRunMode(int iMode, int iTimeout) {
    int       iRetVal = 0;
    CC_STATUS ccs;

    if (IsConnected()) {
        iRetVal = rpc.set_network_mode(iMode, iTimeout);
        if (0 == iRetVal) {
            if (RUN_MODE_RESTORE == iMode) {
                GetCoreClientStatus(ccs, true);
            } else {
                status.network_mode = iMode;
            }
        }
    }

    return iRetVal;
}


// We use 0 to indicate that the RPC has never been called yet, so 
// set last update time to (time_t)1 here rather than to (time_t)0, 
// and only if it is currently not zero.
void CMainDocument::RefreshRPCs() {

    if (!m_dtCachedCCStatusTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtCachedCCStatusTimestamp = wxDateTime((time_t)1);
//      m_iGet_status_rpc_result = -1;
    }
    
    if (!m_dtProjecStatusTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtProjecStatusTimestamp = wxDateTime((time_t)1);
//      m_iGet_project_status1_rpc_result = -1;
    }
        
    if (!m_dtResultsTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtResultsTimestamp = wxDateTime((time_t)1);
//      m_iGet_results_rpc_result = -1;
    }
        
    if (!m_dtFileTransfersTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtFileTransfersTimestamp = wxDateTime((time_t)1);
//      m_iGet_file_transfers_rpc_result = 0;
    }
        
//  m_iGet_messages_rpc_result = -1;
        
    if (!m_dtDiskUsageTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtDiskUsageTimestamp = wxDateTime((time_t)1);
//      m_iGet_dsk_usage_rpc_result = -1;
    }

    if (!m_dtStatisticsStatusTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtStatisticsStatusTimestamp = wxDateTime((time_t)1);
//      m_iGet_statistics_rpc_result = -1;
    }
        
    if (!m_dtCachedSimpleGUITimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtCachedSimpleGUITimestamp = wxDateTime((time_t)1);
//      m_iGet_simple_gui2_rpc_result = -1;
    }

    if (!m_dtCachedAcctMgrInfoTimestamp.IsEqualTo(wxDateTime((time_t)0))) {
        m_dtCachedAcctMgrInfoTimestamp = wxDateTime((time_t)1);
        m_iAcct_mgr_info_rpc_result = -1;
    }

//  m_iGet_state_rpc_result = -1;
}


void CMainDocument::RunPeriodicRPCs() {
    ASYNC_RPC_REQUEST request;
    wxTimeSpan ts;

    // Timer events are handled while the RPC Wait dialog is shown 
    // which may cause unintended recursion and repeatedly posting 
    // the same RPC requests from timer routines.
    if (WaitingForRPC()) return;

    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (!pFrame) return;

    int currentTabView = pFrame->GetCurrentViewPage();
        
    // If the client is heavily loaded (e.g, very many tasks), the 
    // RPC Wait dialog could appear continuously.  To prevent this, 
    // delay periodic RPCs for 1 second after the dialog closes.
    wxDateTime dtNow(wxDateTime::Now());
    if ((currentTabView & (VW_STAT | VW_DISK)) == 0) {
        ts = dtNow - m_dtLasAsyncRPCDlgTime;
        if (ts.GetSeconds()<= DELAYAFTERASYNCRPC_DLG) {
            return;
        }
    }
    
    wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));

    if (!IsConnected()) {
        CFrameEvent event(wxEVT_FRAME_REFRESHVIEW, pFrame);
        pFrame->AddPendingEvent(event);

#if defined(__WXMSW__) || defined(__WXMAC__)
        CTaskBarIcon* pTaskbar = wxGetApp().GetTaskBarIcon();
        if (pTaskbar) {
            CTaskbarEvent event(wxEVT_TASKBAR_REFRESH, pTaskbar);
            pTaskbar->AddPendingEvent(event);
        }
#endif

        return;
    }
    
    // Several functions (such as Abort, Reset, Detach) display an 
    // "Are you sure?" dialog before passing a pointer to a result 
    // or project in a demand RPC call.  If Periodic RPCs continue 
    // to run during these dialogs, that pointer may no longer be 
    // valid by the time the demand RPC is executed.  So we suspend 
    // periodic RPCs during certain modal dialogs.
    //
    // Note that this depends on using wxGetApp().SafeMessageBox()
    // instead of wxMessageBox in all tab views and anywhere else 
    // where a periodic RPC could cause a similar problem.
    if (wxGetApp().IsSafeMesageBoxDisplayed()) {
        return;
    }

    // *********** RPC_GET_CC_STATUS **************
    
    ts = dtNow - m_dtCachedCCStatusTimestamp;
    if (ts.GetSeconds() >= CCSTATUS_RPC_INTERVAL) {
        request.clear();
        request.which_rpc = RPC_GET_CC_STATUS;
        request.arg1 = &async_status_buf;
        request.exchangeBuf = &status;
        request.rpcType = RPC_TYPE_ASYNC_WITH_UPDATE_TASKBAR_ICON_AFTER;
        request.completionTime = &m_dtCachedCCStatusTimestamp;
        request.resultPtr = &m_iGet_status_rpc_result;
       
        RequestRPC(request);
    }

    // *********** RPC_GET_MESSAGES **************

    request.clear();
    request.which_rpc = RPC_GET_MESSAGES;
    // m_iMessageSequenceNumber could change between request and execution
    // of RPC, so pass in a pointer rather than its value
    request.arg1 = &m_iMessageSequenceNumber;
    request.arg2 = &messages;
//        request.arg2 = &async_messages_buf;
//        request.exchangeBuf = &messages;
    request.rpcType = (currentTabView & VW_MSGS) ? 
            RPC_TYPE_ASYNC_WITH_REFRESH_AFTER : RPC_TYPE_ASYNC_WITH_UPDATE_MESSAGE_LIST_AFTER;
    request.completionTime = NULL;
    request.resultPtr = &m_iGet_messages_rpc_result;
   
    RequestRPC(request);

    ts = dtNow - m_dtCachedStateTimestamp;
    if (ts.GetSeconds() >= STATERPC_INTERVAL) {

    // *********** RPC_GET_STATE **************

        request.clear();
        request.which_rpc = RPC_GET_STATE;
        request.arg1 = &async_state_buf;
        request.exchangeBuf = &state;
        request.rpcType = RPC_TYPE_ASYNC_NO_REFRESH;
        request.resultPtr = &m_iGet_state_rpc_result;
       
        RequestRPC(request);

    // *********** RPC_GET_HOST_INFO **************

        request.clear();
        request.which_rpc = RPC_GET_HOST_INFO;
        request.arg1 = &async_host_buf;
        request.exchangeBuf = &host;
        request.rpcType = RPC_TYPE_ASYNC_NO_REFRESH;
        request.completionTime = &m_dtCachedStateTimestamp;
        request.resultPtr = &m_iGet_host_info_rpc_result;
       
        RequestRPC(request);
    }
     
    // TODO: modify SimpleGUI to not do direct RPC calls when hidden / minimized
    if (! ((currentTabView & VW_SGUI) || pFrame->IsShown()) ) return;
   
    // *********** RPC_GET_PROJECT_STATUS1 **************

    if (currentTabView & VW_PROJ) {
        wxTimeSpan ts(dtNow - m_dtProjecStatusTimestamp);
        if (ts.GetSeconds() >= PROJECTSTATUSRPC_INTERVAL) {
            request.clear();
            request.which_rpc = RPC_GET_PROJECT_STATUS1;
            request.arg1 = &async_projects_update_buf;
            request.arg2 = &state;
            request.rpcType = RPC_TYPE_ASYNC_WITH_REFRESH_AFTER;
            request.completionTime = &m_dtProjecStatusTimestamp;
            request.resultPtr = &m_iGet_project_status1_rpc_result;
           
            RequestRPC(request);
        }
    }

    // *********** RPC_GET_RESULTS **************

    if (currentTabView & VW_TASK) {
        wxTimeSpan ts(dtNow - m_dtResultsTimestamp);
        if (ts.GetSeconds() >= RESULTSRPC_INTERVAL) {
	            request.clear();
	            request.which_rpc = RPC_GET_RESULTS;
	            request.arg1 = &async_results_buf;
	            request.arg2 = &m_ActiveTasksOnly;
	            request.exchangeBuf = &results;
	            request.rpcType = RPC_TYPE_ASYNC_WITH_REFRESH_AFTER;
	            request.completionTime = &m_dtResultsTimestamp;
	            request.resultPtr = &m_iGet_results_rpc_result;
	           
	            RequestRPC(request);
        }
    }
    
    // *********** RPC_GET_FILE_TRANSFERS **************

    if (currentTabView & VW_XFER) {
        wxTimeSpan ts(dtNow - m_dtFileTransfersTimestamp);
        if (ts.GetSeconds() >= FILETRANSFERSRPC_INTERVAL) {
            request.clear();
            request.which_rpc = RPC_GET_FILE_TRANSFERS;
            request.arg1 = &async_ft_buf;
            request.exchangeBuf = &ft;
            request.rpcType = RPC_TYPE_ASYNC_WITH_REFRESH_AFTER;
            request.completionTime = &m_dtFileTransfersTimestamp;
            request.resultPtr = &m_iGet_file_transfers_rpc_result;
           
            RequestRPC(request);
        }
    }
    
    // *********** RPC_GET_STATISTICS **************

    if (currentTabView & VW_STAT) {
        wxTimeSpan ts(dtNow - m_dtStatisticsStatusTimestamp);
        if (ts.GetSeconds() >= STATISTICSSTATUSRPC_INTERVAL) {
            request.clear();
            request.which_rpc = RPC_GET_STATISTICS;
            request.arg1 = &async_statistics_status_buf;
            request.exchangeBuf = &statistics_status;
            request.rpcType = RPC_TYPE_ASYNC_WITH_REFRESH_AFTER;
            request.completionTime = &m_dtStatisticsStatusTimestamp;
            request.resultPtr = &m_iGet_statistics_rpc_result;
           
            RequestRPC(request);
        }
    }
    
    // *********** RPC_GET_DISK_USAGE **************

    if (currentTabView & VW_DISK) {
        wxTimeSpan ts(dtNow - m_dtDiskUsageTimestamp);
        if ((ts.GetSeconds() >= DISKUSAGERPC_INTERVAL) || disk_usage.projects.empty()) {
            request.clear();
            request.which_rpc = RPC_GET_DISK_USAGE;
            request.arg1 = &async_disk_usage_buf;
            request.exchangeBuf = &disk_usage;
            request.rpcType = RPC_TYPE_ASYNC_WITH_REFRESH_AFTER;
            request.completionTime = &m_dtDiskUsageTimestamp;
            request.resultPtr = &m_iGet_dsk_usage_rpc_result;
           
            RequestRPC(request);
        }
    }
    
    // *********** GET_SIMPLE_GUI_INFO2 **************
    if (currentTabView & VW_SGUI) {
        wxTimeSpan ts(dtNow - m_dtCachedSimpleGUITimestamp);
        if (ts.GetSeconds() >= CACHEDSIMPLEGUIRPC_INTERVAL) {
            request.clear();
            request.which_rpc = RPC_GET_SIMPLE_GUI_INFO2;
            request.arg1 = &async_projects_update_buf;
            request.arg2 = &state;
            request.arg3 = &async_results_buf;
            request.exchangeBuf = &results;
            request.rpcType = RPC_TYPE_ASYNC_WITH_REFRESH_AFTER;
            request.completionTime = &m_dtCachedSimpleGUITimestamp;
            request.resultPtr = &m_iGet_simple_gui2_rpc_result;
           
            RequestRPC(request);
        }
    }
    // *********** RPC_ACCT_MGR_INFO **************

    if (currentTabView & VW_SGUI) {
        wxTimeSpan ts(dtNow - m_dtCachedAcctMgrInfoTimestamp);
        if (ts.GetSeconds() >= CACHEDACCTMGRINFORPC_INTERVAL) {
            request.clear();
            request.which_rpc = RPC_ACCT_MGR_INFO;
            request.arg1 = &async_ami_buf;
            request.exchangeBuf = &ami;
            request.rpcType = RPC_TYPE_ASYNC_NO_REFRESH;
            request.completionTime = &m_dtCachedAcctMgrInfoTimestamp;
            request.resultPtr = &m_iAcct_mgr_info_rpc_result;
           
            RequestRPC(request);
        }
    }
}


// TODO: Is it enough to just reset m_dtCachedStateTimestamp 
// and let RunPeriodicRPCs() update the state?  This would avoid 
// displaying the "Please wait" dialog on multi-processor computers.  
// Possible exceptions might be when ForceCacheUpdate() is called 
// from these routines (which may need immediate results):
//      CAdvancedFrame::OnConnect()
//      CDlgItemProperties::FormatApplicationName()
//      WorkunitNotebook::AddTab()
//      CMainDocument::CachedProjectStatusUpdate()
//      CMainDocument::CachedSimpleGUIUpdate()
//
// Currently, no calls to ForceCacheUpdate pass false as the arg.
//
int CMainDocument::ForceCacheUpdate(bool immediate) {
    wxLogTrace(wxT("Function Start/End"), wxT("CMainDocument::ForceCacheUpdate - Function Begin"));

    if (!immediate) {
        m_dtCachedStateTimestamp = wxDateTime((time_t)0);
        return m_iGet_state_rpc_result;
    }
    
    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    int     retval = 0;

    if (IsConnected()) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));
        pFrame->UpdateStatusText(_("Retrieving system state; please wait..."));

        m_dtCachedStateTimestamp = wxDateTime::Now();
        m_iGet_state_rpc_result = rpc.get_state(state);
        if (m_iGet_state_rpc_result) {
            retval = m_iGet_state_rpc_result;
            wxLogTrace(wxT("Function Status"), wxT("CMainDocument::ForceCacheUpdate - Get State Failed '%d'"), retval);
            m_pNetworkConnection->SetStateDisconnected();
        }
        pFrame->UpdateStatusText(_("Retrieving host information; please wait..."));

        m_iGet_host_info_rpc_result = rpc.get_host_info(host);
        if (m_iGet_host_info_rpc_result) {
             retval = m_iGet_host_info_rpc_result;
            wxLogTrace(wxT("Function Status"), wxT("CMainDocument::ForceCacheUpdate - Get Host Information Failed '%d'"), retval);
            m_pNetworkConnection->SetStateDisconnected();
        }

        pFrame->UpdateStatusText(wxEmptyString);
    } else {
        retval = -1;
    }

    wxLogTrace(wxT("Function Start/End"), wxT("CMainDocument::ForceCacheUpdate - Function End"));
    return retval;
}


int CMainDocument::RunBenchmarks() {
    return rpc.run_benchmarks();
}


int CMainDocument::CoreClientQuit() {
    return rpc.quit();
}


bool CMainDocument::IsUserAuthorized() {
#ifndef _WIN32
#ifdef SANDBOX
    static bool         sIsAuthorized = false;
    group               *grp;
    gid_t               rgid, boinc_master_gid;
    char                *userName, *groupMember;
    int                 i;

    if (g_use_sandbox) {
        if (sIsAuthorized)
            return true;            // We already checked and OK'd current user

        grp = getgrnam(BOINC_MASTER_GROUP_NAME);
        if (grp) {
            boinc_master_gid = grp->gr_gid;

            rgid = getgid();
            if (rgid == boinc_master_gid) {
                sIsAuthorized = true;           // User's primary group is boinc_master
                return true;
            }

            userName = getlogin();
            if (userName) {
                for (i=0; ; i++) {              // Step through all users in group boinc_master
                    groupMember = grp->gr_mem[i];
                    if (groupMember == NULL)
                        break;                  // User is not a member of group boinc_master
                    if (strcmp(userName, groupMember) == 0) {
                        sIsAuthorized = true;   // User is a member of group boinc_master
                        return true;
                    }
                }       // for (i)
            }           // if (userName)
        }               // if grp

#ifdef __WXMAC__
        if (Mac_Authorize()) {          // Run Mac Authentication dialog
            sIsAuthorized = true;       // Authenticated by password
            return true;
        }
#endif      // __WXMAC__

        return false;

    }       // if (g_use_sandbox)
#endif      // SANDBOX
#endif      // #ifndef _WIN32

    return true;
}


int CMainDocument::CachedProjectStatusUpdate(bool bForce) {
    int     i = 0;

    if (! IsConnected()) return -1;

#if USE_CACHE_TIMEOUTS
    wxTimeSpan ts(wxDateTime::Now() - m_dtProjecStatusTimestamp);
    if (ts.GetSeconds() >= (2 * PROJECTSTATUSRPC_INTERVAL)) bForce = true;
#endif
    if (m_dtProjecStatusTimestamp.IsEqualTo(wxDateTime((time_t)0))) bForce = true;
    
    if (bForce) {
        m_dtProjecStatusTimestamp = wxDateTime::Now();
        m_iGet_project_status1_rpc_result = rpc.get_project_status(async_projects_update_buf, state);
    }

    if (m_iGet_project_status1_rpc_result) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CachedProjectStatusUpdate - Get Project Status Failed '%d'"), m_iGet_project_status1_rpc_result);
        ForceCacheUpdate();
//        return m_iGet_project_status1_rpc_result;
    }

    m_fProjectTotalResourceShare = 0.0;
    for (i=0; i < (long)state.projects.size(); i++) {
        m_fProjectTotalResourceShare += state.projects.at(i)->resource_share;
    }
    if (!m_fProjectTotalResourceShare) m_fProjectTotalResourceShare = 1;

    return m_iGet_project_status1_rpc_result;
}


PROJECT* CMainDocument::project(unsigned int i) {
    PROJECT* pProject = NULL;

    try {
        if (!state.projects.empty())
            pProject = state.projects.at(i);
    }
    catch (std::out_of_range e) {
        pProject = NULL;
    }

    return pProject;
}


PROJECT* CMainDocument::project(const wxString& projectname) {
	for (unsigned int i=0; i< state.projects.size(); i++) {
		PROJECT* tp = state.projects[i];
		wxString t1(tp->project_name.c_str(), wxConvUTF8);
		if(t1.IsSameAs(projectname)) return tp;
		wxString t2(tp->master_url.c_str(), wxConvUTF8);
		if(t2.IsSameAs(projectname)) return tp;
	}
    return NULL;
}


int CMainDocument::GetProjectCount() {
    int iCount = -1;

    CachedProjectStatusUpdate();
    CachedStateUpdate();

    if (!state.projects.empty())
        iCount = (int)state.projects.size();

    return iCount;
}


int CMainDocument::ProjectDetach(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "detach");

    return iRetVal;
}


int CMainDocument::ProjectDetach(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "detach");

    return iRetVal;
}


int CMainDocument::ProjectUpdate(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "update");

    return iRetVal;
}

int CMainDocument::ProjectUpdate(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "update");

    return iRetVal;
}


int CMainDocument::ProjectReset(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "reset");

    return iRetVal;
}

int CMainDocument::ProjectReset(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "reset");

    return iRetVal;
}


int CMainDocument::ProjectSuspend(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "suspend");

    return iRetVal;
}

int CMainDocument::ProjectSuspend(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "suspend");

    return iRetVal;
}

int CMainDocument::ProjectResume(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "resume");

    return iRetVal;
}

int CMainDocument::ProjectResume(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "resume");

    return iRetVal;
}

int CMainDocument::ProjectNoMoreWork(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "nomorework");

    return iRetVal;
}

int CMainDocument::ProjectNoMoreWork(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "nomorework");

    return iRetVal;
}

int CMainDocument::ProjectAllowMoreWork(int iIndex) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(iIndex);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "allowmorework");

    return iRetVal;
}

int CMainDocument::ProjectAllowMoreWork(const wxString& projectname) {
    PROJECT* pProject = NULL;
    int iRetVal = -1;

    pProject = project(projectname);

    if (pProject)
        iRetVal = rpc.project_op((*pProject), "allowmorework");

    return iRetVal;
}


int CMainDocument::CachedResultsStatusUpdate() {
    if (! IsConnected()) return -1;
    bool active_tasks_only = false;
    bool immediate = false;

    CBOINCBaseFrame* pFrame = wxGetApp().GetFrame();
    if (pFrame) {
        wxASSERT(wxDynamicCast(pFrame, CBOINCBaseFrame));

        if (pFrame->GetCurrentViewPage() & VW_TASK) {
            active_tasks_only = m_ActiveTasksOnly;
        }
    }

#if USE_CACHE_TIMEOUTS
    wxTimeSpan ts(wxDateTime::Now() - m_dtResultsTimestamp);
    if (ts.GetSeconds() >= (2 * RESULTSRPC_INTERVAL)) immediate = true;
#endif
    if (m_dtResultsTimestamp.IsEqualTo(wxDateTime((time_t)0))) immediate = true;
    
    if (immediate) {
        m_dtResultsTimestamp = wxDateTime::Now();
        m_iGet_results_rpc_result = rpc.get_results(results, active_tasks_only);
    }
    
    if (m_iGet_results_rpc_result) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CachedResultsStatusUpdate - Get Result Status Failed '%d'"), m_iGet_results_rpc_result);
        ForceCacheUpdate();
    }

    return m_iGet_results_rpc_result;
}


RESULT* CMainDocument::result(unsigned int i) {
    RESULT* pResult = NULL;

    try {
        if (!results.results.empty())
            pResult = results.results.at(i);
    }
    catch (std::out_of_range e) {
        pResult = NULL;
    }

    return pResult;
}

/* get the result not by index, but by name */
RESULT* CMainDocument::result(const wxString& name, const wxString& project_url) {
    RESULT* pResult = NULL;

    try {
        if (!results.results.empty()) {
            //iterating over the vector and find the right result
            for(unsigned int i=0; i< results.results.size();i++) {
                RESULT* tResult = results.results.at(i);
                wxString resname(tResult->name.c_str(),wxConvUTF8);
                if(resname.IsSameAs(name)){
                    wxString resurl(tResult->project_url.c_str(),wxConvUTF8);
                    if(resurl.IsSameAs(project_url)){
                        pResult = tResult;
                        break;
                    }
                }
            }
        }
    }
    catch (std::out_of_range e) {
        pResult = NULL;
    }

    return pResult;
}

int CMainDocument::GetWorkCount() {
    int iCount = -1;
    
    CachedResultsStatusUpdate();
    CachedStateUpdate();

    if (!results.results.empty())
        iCount = (int)results.results.size();

    return iCount;
}


int CMainDocument::WorkSuspend(std::string& strProjectURL, std::string& strName) {
    int iRetVal = 0;

    RESULT* pStateResult = state.lookup_result(strProjectURL, strName);
    if (pStateResult) {
        iRetVal = rpc.result_op((*pStateResult), "suspend");
    } else {
        ForceCacheUpdate();
    }

    return iRetVal;
}


int CMainDocument::WorkResume(std::string& strProjectURL, std::string& strName) {
    int iRetVal = 0;

    RESULT* pStateResult = state.lookup_result(strProjectURL, strName);
    if (pStateResult) {
        iRetVal = rpc.result_op((*pStateResult), "resume");
    } else {
        ForceCacheUpdate();
    }

    return iRetVal;
}


// If the graphics application for the current task is already 
// running, return a pointer to its RUNNING_GFX_APP struct.
RUNNING_GFX_APP* CMainDocument::GetRunningGraphicsApp(RESULT* result, int slot)
{
    bool exited = false;
    std::vector<RUNNING_GFX_APP>::iterator gfx_app_iter;
    
    for( gfx_app_iter = m_running_gfx_apps.begin(); 
        gfx_app_iter != m_running_gfx_apps.end(); 
        gfx_app_iter++
    ) {
         if ( (slot >= 0) && ((*gfx_app_iter).slot != slot) ) continue;

#ifdef _WIN32
        unsigned long exit_code;
        if (GetExitCodeProcess((*gfx_app_iter).pid, &exit_code)) {
            if (exit_code != STILL_ACTIVE) {
                exited = true;
            }
        }
#else
        if (waitpid((*gfx_app_iter).pid, 0, WNOHANG) == (*gfx_app_iter).pid) {
            exited = true;
        }
#endif
        if (! exited) {
            if ( (result->name == (*gfx_app_iter).name) &&
                (result->project_url == (*gfx_app_iter).project_url) ) {
                return &(*gfx_app_iter);
            }
    
            // Graphics app is still running but the slot now has a different task
            KillGraphicsApp((*gfx_app_iter).pid);
        }

        // Either the graphics app had already exited or we just killed it
        (*gfx_app_iter).name.clear();
        (*gfx_app_iter).project_url.clear();
        m_running_gfx_apps.erase(gfx_app_iter);
        return NULL;
    }
    return NULL;
}


// Kill any running graphics apps whose worker tasks aren't running
void CMainDocument::KillInactiveGraphicsApps()
{
/*
    std::vector<RUNNING_GFX_APP>::iterator gfx_app_iter;
    unsigned int i;
    bool bStillRunning;

    if (m_running_gfx_apps.size() <= 0) return;
    
    // If none of the Tasks displays are visible, we need to update 
    // the results vector.  This call does nothing if recently updated
    // by a call from CViewWork or CViewTabPage.
    CachedResultsStatusUpdate();
    
    gfx_app_iter = m_running_gfx_apps.begin();
    while (gfx_app_iter != m_running_gfx_apps.end()) {
        bStillRunning = false;
        
        for (i=0; i<results.results.size(); i++) {
            if ((results.results.at(i))->state != RESULT_FILES_DOWNLOADED) continue;
            if (!(results.results.at(i))->active_task) continue;
            if ((results.results.at(i))->scheduler_state != CPU_SCHED_SCHEDULED) continue;
            if ((results.results.at(i))->name != (*gfx_app_iter).name) continue;
            if ((results.results.at(i))->project_url != (*gfx_app_iter).project_url) continue;    
            bStillRunning =  true;
            break;
        }
        
        if (!bStillRunning) {
            KillGraphicsApp((*gfx_app_iter).pid);
            gfx_app_iter = m_running_gfx_apps.erase(gfx_app_iter);
        } else {
            gfx_app_iter++;
        }
    }
*/
}


void CMainDocument::KillAllRunningGraphicsApps()
{
    size_t i, n;
    std::vector<RUNNING_GFX_APP>::iterator gfx_app_iter;

    n = m_running_gfx_apps.size();
    for (i=0; i<n; i++) {
        gfx_app_iter = m_running_gfx_apps.begin(); 
        KillGraphicsApp((*gfx_app_iter).pid);
        (*gfx_app_iter).name.clear();
        (*gfx_app_iter).project_url.clear();
        m_running_gfx_apps.erase(gfx_app_iter);
    }
}


#ifdef _WIN32
void CMainDocument::KillGraphicsApp(HANDLE pid) {
    kill_program(pid);
}
#else
void CMainDocument::KillGraphicsApp(int pid) {
    char* argv[6];
    char currentDir[1024];
    char thePIDbuf[10];
    int id, iRetVal;
    

    if (g_use_sandbox) {
        snprintf(thePIDbuf, sizeof(thePIDbuf), "%d", pid);
        argv[0] = "switcher";
        argv[1] = "/bin/kill";
        argv[2] =  "kill";
        argv[3] = "-KILL";
        argv[4] = thePIDbuf;
        argv[5] = 0;
    
        iRetVal = run_program(
            getcwd(currentDir, sizeof(currentDir)),
            "./switcher/switcher",
            5,
            argv,
            0,
            id
        );
    } else {
        kill_program(pid);
    }
}
#endif

int CMainDocument::WorkShowGraphics(RESULT* result)
{
    int iRetVal = 0;
    
    if (!result->graphics_exec_path.empty()) {
        // V6 Graphics
        RUNNING_GFX_APP gfx_app;
        RUNNING_GFX_APP* previous_gfx_app;
        char *p;
        int slot;
#ifdef __WXMSW__
        HANDLE   id;
#else
        int      id;
#endif

        p = strrchr((char*)result->slot_path.c_str(), '/');
        if (!p) return ERR_INVALID_PARAM;
        slot = atoi(p+1);
        
        // See if we are already running the graphics application for this task
        previous_gfx_app = GetRunningGraphicsApp(result, slot);

#ifndef __WXMSW__
        char* argv[4];

        if (previous_gfx_app) {
#ifdef __WXMAC__
        ProcessSerialNumber gfx_app_psn;
            // If this graphics app is already running,
            // just bring it to the front
            //
            if (!GetProcessForPID(previous_gfx_app->pid, &gfx_app_psn)) {
                SetFrontProcess(&gfx_app_psn);
            }
#endif
            // If graphics app is already running, don't launch a second instance
            //
            return 0;
        }
        argv[0] = "switcher";
        // For unknown reasons on Macs, the graphics application 
        // exits with "RegisterProcess failed (error = -50)" unless 
        // we pass its full path twice in the argument list to execv.
        //
        argv[1] = (char *)result->graphics_exec_path.c_str();
        argv[2] = (char *)result->graphics_exec_path.c_str();
        argv[3] = 0;
    
         if (g_use_sandbox) {
            iRetVal = run_program(
                result->slot_path.c_str(),
               "../../switcher/switcher",
                3,
                argv,
                0,
                id
            );
        } else {        
            iRetVal = run_program(
                result->slot_path.c_str(),
                result->graphics_exec_path.c_str(),
                1,
                &argv[2],
                0,
                id
            );
        }
#else
        char* argv[2];

        // If graphics app is already running, don't launch a second instance
        //
        if (previous_gfx_app) return 0;
        argv[0] =0;
        
        iRetVal = run_program(
            result->slot_path.c_str(),
            result->graphics_exec_path.c_str(),
            0,
            argv,
            0,
            id
        );
#endif

        if (!iRetVal) {
            gfx_app.slot = slot;
            gfx_app.project_url = result->project_url;
            gfx_app.name = result->name;
            gfx_app.pid = id;
            m_running_gfx_apps.push_back(gfx_app);
        }

    } else {
        // V5 and Older
        DISPLAY_INFO di;

        strcpy(di.window_station, (const char*)wxGetApp().m_strDefaultWindowStation.mb_str());
        strcpy(di.desktop, (const char*)wxGetApp().m_strDefaultDesktop.mb_str());
        strcpy(di.display, (const char*)wxGetApp().m_strDefaultDisplay.mb_str());

        iRetVal = rpc.show_graphics(
            result->project_url.c_str(),
            result->name.c_str(),
            MODE_WINDOW,
            di
        );
    }

    return iRetVal;
}


int CMainDocument::WorkAbort(std::string& strProjectURL, std::string& strName) {
    int iRetVal = 0;

    RESULT* pStateResult = state.lookup_result(strProjectURL, strName);
    if (pStateResult) {
        iRetVal = rpc.result_op((*pStateResult), "abort");
    } else {
        ForceCacheUpdate();
    }

    return iRetVal;
}


// Call this only when message buffer is stable
// Note: This must not call any rpcs.
int CMainDocument::CachedMessageUpdate() {
    static bool in_this_func = false;

    if (in_this_func) return 0;
    in_this_func = true;

    if (IsConnected()) {
        // rpc.get_messages is now called from RunPeriodicRPCs()
//        retval = rpc.get_messages(m_iMessageSequenceNumber, messages);
        if (m_iGet_messages_rpc_result) {
            wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CachedMessageUpdate - Get Messages Failed '%d'"), m_iGet_messages_rpc_result);
            m_pNetworkConnection->SetStateDisconnected();
            goto done;
        }
        if (messages.messages.size() != 0) {
            size_t last_ind = messages.messages.size()-1;
            m_iMessageSequenceNumber = messages.messages[last_ind]->seqno;
        }
    }
done:
    in_this_func = false;
    return 0;
}


MESSAGE* CMainDocument::message(unsigned int i) {
    MESSAGE* pMessage = NULL;

    try {
        if (!messages.messages.empty())
            pMessage = messages.messages.at(i);
    }
    catch (std::out_of_range e) {
        pMessage = NULL;
    }

    return pMessage;
}


int CMainDocument::GetMessageCount() {
    int iCount = -1;

    // CachedMessageUpdate() is now called from CMainDocument::OnRPCComplete() 
    // only after a get_messages RPC completes so messages buffer is stable.
    CachedStateUpdate();

    if (!messages.messages.empty()) {
        iCount = (int)messages.messages.size();
    }
    
    return iCount;
}


int CMainDocument::ResetMessageState() {
    messages.clear();
    m_iMessageSequenceNumber = 0;
    return 0;
}


int CMainDocument::CachedFileTransfersUpdate() {
    bool immediate = false;

    if (! IsConnected()) return -1;

#if USE_CACHE_TIMEOUTS
    wxTimeSpan ts(wxDateTime::Now() - m_dtFileTransfersTimestamp);
    if (ts.GetSeconds() >= (2* FILETRANSFERSRPC_INTERVAL)) immediate = true;
#endif
    if (m_dtFileTransfersTimestamp.IsEqualTo(wxDateTime((time_t)0))) immediate = true;
    
    if (immediate) {
        m_dtFileTransfersTimestamp = wxDateTime::Now();
        m_iGet_file_transfers_rpc_result = rpc.get_file_transfers(ft);
    }
 
     if (m_iGet_file_transfers_rpc_result) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CachedFileTransfersUpdate - Get File Transfers Failed '%d'"), m_iGet_file_transfers_rpc_result);
        ForceCacheUpdate();
    }

    return m_iGet_file_transfers_rpc_result;
}


FILE_TRANSFER* CMainDocument::file_transfer(unsigned int i) {
    FILE_TRANSFER* pFT = NULL;

    try {
        if (!ft.file_transfers.empty())
            pFT = ft.file_transfers.at(i);
    }
    catch (std::out_of_range e) {
        pFT = NULL;
    }

    return pFT;
}

FILE_TRANSFER* CMainDocument::file_transfer(const wxString& fileName, const wxString& project_url) {
    FILE_TRANSFER* pFT = NULL;

    try {
        if (!ft.file_transfers.empty()) {
            for(unsigned int i=0; i< ft.file_transfers.size();i++) {
                FILE_TRANSFER* tFT = ft.file_transfers.at(i);
                wxString fname(tFT->name.c_str(),wxConvUTF8);
                if(fname.IsSameAs(fileName)) {
                    wxString furl(tFT->project_url.c_str(),wxConvUTF8);
                    if(furl.IsSameAs(project_url)){
                        pFT = tFT;
                        break;
                    }
                }
            }
        }
    }
    catch (std::out_of_range e) {
        pFT = NULL;
    }

    return pFT;
}


int CMainDocument::GetTransferCount() {
    int iCount = 0;

    CachedFileTransfersUpdate();
    CachedStateUpdate();

    if (!ft.file_transfers.empty())
        iCount = (int)ft.file_transfers.size();

    return iCount;
}


int CMainDocument::TransferRetryNow(int iIndex) {
    FILE_TRANSFER* pFT = NULL;
    int iRetVal = 0;

    pFT = file_transfer(iIndex);

    if (pFT)
        iRetVal = rpc.file_transfer_op((*pFT), "retry");

    return iRetVal;
}

int CMainDocument::TransferRetryNow(const wxString& fileName, const wxString& project_url) {
    FILE_TRANSFER* pFT = NULL;
    int iRetVal = 0;

    pFT = file_transfer(fileName, project_url);

    if (pFT)
        iRetVal = rpc.file_transfer_op((*pFT), "retry");

    return iRetVal;
}


int CMainDocument::TransferAbort(int iIndex) {
    FILE_TRANSFER* pFT = NULL;
    int iRetVal = 0;

    pFT = file_transfer(iIndex);

    if (pFT)
        iRetVal = rpc.file_transfer_op((*pFT), "abort");

    return iRetVal;
}

int CMainDocument::TransferAbort(const wxString& fileName, const wxString& project_url) {
    FILE_TRANSFER* pFT = NULL;
    int iRetVal = 0;

    pFT = file_transfer(fileName, project_url);

    if (pFT)
        iRetVal = rpc.file_transfer_op((*pFT), "abort");

    return iRetVal;
}


int CMainDocument::CachedDiskUsageUpdate() {
    bool immediate = false;

    if (! IsConnected()) return -1;

    // don't get disk usage more than once per minute
            // unless we just connected to a client
    //
#if USE_CACHE_TIMEOUTS
    wxTimeSpan ts(wxDateTime::Now() - m_dtDiskUsageTimestamp);
    if (ts.GetSeconds() >= (2 * DISKUSAGERPC_INTERVAL)) immediate = true;
#endif    
    if (disk_usage.projects.empty()) immediate = true;
    if (m_dtDiskUsageTimestamp.IsEqualTo(wxDateTime((time_t)0))) immediate = true;
    
    if (immediate) {
        m_dtDiskUsageTimestamp = wxDateTime::Now();
        m_iGet_dsk_usage_rpc_result = rpc.get_disk_usage(disk_usage);
    }
    
    if (m_iGet_dsk_usage_rpc_result) {
        wxLogTrace(wxT("Function Status"), wxT("Get Disk Usage Failed '%d'"), m_iGet_dsk_usage_rpc_result);
        ForceCacheUpdate();
    }

    return m_iGet_dsk_usage_rpc_result;
}


PROJECT* CMainDocument::DiskUsageProject(unsigned int i) {
    PROJECT* pProject = NULL;

    try {
        if (!disk_usage.projects.empty()) {
            pProject = disk_usage.projects.at(i);
        }
    }
    catch (std::out_of_range e) {
        pProject = NULL;
    }

    return pProject;
}

int CMainDocument::CachedStatisticsStatusUpdate() {
    bool immediate = false;

    if (! IsConnected()) return -1;

#if USE_CACHE_TIMEOUTS
    wxTimeSpan ts(wxDateTime::Now() - m_dtStatisticsStatusTimestamp);
    if (ts.GetSeconds() >= (2 * STATISTICSSTATUSRPC_INTERVAL)) immediate = true;
#endif
    if (statistics_status.projects.empty()) immediate = true;
    if (m_dtStatisticsStatusTimestamp.IsEqualTo(wxDateTime((time_t)0))) immediate = true;
    
    if (immediate) {
        m_dtStatisticsStatusTimestamp = wxDateTime::Now();
        m_dtStatisticsStatusTimestamp = rpc.get_statistics(statistics_status);
    }

    if (m_iGet_statistics_rpc_result) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CachedStatisticsStatusUpdate - Get Statistics Failed '%d'"), m_iGet_statistics_rpc_result);
        ForceCacheUpdate();
    }

    return m_iGet_state_rpc_result;
}


PROJECT* CMainDocument::statistic(unsigned int i) {
    PROJECT* pProject = NULL;


    try {
        if (!statistics_status.projects.empty())
            pProject = statistics_status.projects.at(i);
    }
    catch (std::out_of_range e) {
        pProject = NULL;
    }


    return pProject;
}


int CMainDocument::GetStatisticsCount() {
    int iCount = -1;

    CachedStatisticsStatusUpdate();
    CachedStateUpdate();

    if (!statistics_status.projects.empty())
        iCount = (int)statistics_status.projects.size();

    return iCount;
}


int CMainDocument::GetProxyConfiguration() {
    int     iRetVal = 0;
    wxString    strEmpty = wxEmptyString;

    iRetVal = rpc.get_proxy_settings(proxy_info);
    if (iRetVal) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::GetProxyInfo - Get Proxy Info Failed '%d'"), iRetVal);
    }

    return iRetVal;
}


int CMainDocument::SetProxyConfiguration() {
    int     iRetVal = 0;

    if (!proxy_info.http_user_name.empty() || !proxy_info.http_user_passwd.empty())
        proxy_info.use_http_authentication = true;

    proxy_info.socks_version = 4;
    if (!proxy_info.socks5_user_name.empty() || !proxy_info.socks5_user_passwd.empty())
        proxy_info.socks_version = 5;

    iRetVal = rpc.set_proxy_settings(proxy_info);
    if (iRetVal) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::SetProxyInfo - Set Proxy Info Failed '%d'"), iRetVal);
    }

    return iRetVal;
}


int CMainDocument::CachedSimpleGUIUpdate(bool bForce) {
    int     i = 0;

    if (! IsConnected()) return -1;

#if USE_CACHE_TIMEOUTS
    wxTimeSpan ts(wxDateTime::Now() - m_dtCachedSimpleGUITimestamp);
    if (ts.GetSeconds() >= (2 * CACHEDSIMPLEGUIRPC_INTERVAL)) bForce = true;
#endif
    if (m_dtCachedSimpleGUITimestamp.IsEqualTo(wxDateTime((time_t)0))) bForce = true;
    if (bForce) {
        m_dtCachedSimpleGUITimestamp = wxDateTime::Now();
        m_iGet_simple_gui2_rpc_result = rpc.get_simple_gui_info(async_projects_update_buf, state, results);
    }

    if (m_iGet_simple_gui2_rpc_result) {
        wxLogTrace(wxT("Function Status"), wxT("CMainDocument::CachedSimpleGUIUpdate - Get Simple GUI Failed '%d'"), m_iGet_simple_gui2_rpc_result);
        ForceCacheUpdate();
    }

    m_fProjectTotalResourceShare = 0.0;
    for (i=0; i < (long)state.projects.size(); i++) {
        m_fProjectTotalResourceShare += state.projects.at(i)->resource_share;
    }
    if (!m_fProjectTotalResourceShare) m_fProjectTotalResourceShare = 1;

    return m_iGet_simple_gui2_rpc_result;
}


int CMainDocument::GetSimpleProjectCount() {
    int iCount = -1;

    CachedSimpleGUIUpdate();
    CachedStateUpdate();

    if (!state.projects.empty())
        iCount = (int)state.projects.size();

    return iCount;
}


int CMainDocument::GetSimpleGUIWorkCount() {
    int iCount = 0;
    unsigned int i = 0;

    CachedSimpleGUIUpdate();
    CachedStateUpdate();

	for(i=0; i<results.results.size(); i++) {
		if (results.results[i]->active_task) {
			iCount++;
		}
	}
    return iCount;
}

