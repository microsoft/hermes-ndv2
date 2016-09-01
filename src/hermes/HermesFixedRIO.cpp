// HermesFixedRIO.cpp : A implementation of Hermes interface
//
// It's a module that binds to an address with NDv2 supported adptor, 
// This implementation uses a set of fixed size registered I/O buffers.
//

#pragma once
#include "stdafx.h"

#include <vector>

#include "SchedulerIOCP.hpp"
#include "Control.hpp"
#include "Mem.hpp"

#include "HermesFixedRIO.hpp"

namespace hinternal
{
    using namespace Ctrl;
    using namespace mbuf;

    // Startup terminal cause the messages are coming.
    //
    void HermesFixedRIO::StartTerminal(uint32_t termId, const HandShake& handShake)
    {
        // TODO If we need to use send/receive verbs
        // prepost receive buffers here
        //
        auto pTerm = GetTerminal(termId);
        pTerm->SaveRemoteKeys(handShake);
        pTerm->m_sndRunning = false;
        pTerm->m_sndQue.clear();

        InitializeSRWLock(&pTerm->m_sndLock);

        wchar_t name[128];
        swprintf_s(name, L"Sock:%u", termId);
        auto termThread = ActivityFactory(*m_pScheduler, name);

        auto pArena = termThread->GetArena();
        pTerm->m_pRcvCont = pArena->allocate<Conveyor>
            (*termThread, *this, termId);
        pTerm->m_pDisconnectCont = pArena->allocate<TerminalCloser>
            (*termThread, *this, termId);
        pTerm->m_pVerbCompletion = pArena->allocate<VerbCompletion>
            (*this, termId);

        auto pOv = m_overlapPool.GetObj(pTerm->m_pVerbCompletion, pTerm->m_pCQ);
        auto hr = pTerm->m_pCQ->Notify(ND_CQ_NOTIFY_ANY, &pOv->OV);

        // TODO!! for a client socket, this is called after we received server
        // ack and before we call CompleteConnect. On the event that the
        // server sends the first greeting we may have an immediate return
        // with ND_SUCCESS here.
        Audit::Assert(hr == ND_PENDING,
            L"Notification error not handled! We don't expect immediate return either!");

    }

    // Slice large page buffers into array of buffer pairs Terminal
    // Must be called from the constructor
    //
    void HermesFixedRIO::SliceNregister()
    {
        // two buffers per connection
        auto totalBufSize = m_maxConnection * m_bufSize * 2;

        m_buffers = m_pMemory->GetData().subspan(0, totalBufSize);

        // can't get gsl::as_span to work here ?!
        auto connectionRegion = m_pMemory->GetData().subspan(totalBufSize);
        auto maxConn = connectionRegion.size_bytes() / sizeof(Terminal);
        Audit::Assert(maxConn > m_maxConnection,
            L"Not enough memory for connections.");
        m_connections = gsl::span<Terminal>(
            reinterpret_cast<Terminal*>(connectionRegion.data()),
            m_maxConnection);

        auto sockRegion = connectionRegion.subspan(m_connections.size_bytes());
        auto numSockets = m_sockPool.GetCapacity();
        Audit::Assert(numSockets == (m_maxConnection << 1),
            L"number of socket should be double number of connections");
        Audit::Assert(sockRegion.size_bytes() / sizeof(HSocket) >= numSockets,
            L"Not enough memory for sockets.");

        m_sockTbl = gsl::span<HSocket>(
            reinterpret_cast<HSocket*>(sockRegion.data()), numSockets);

        // VirtualAlloc should zero out all memory already

        // Initialize and Register, note that index start from 1
        for (uint32_t conId = 1; conId <= m_maxConnection; conId++)
        {
            auto buf = GetBufPair(conId);
            auto conInfo = GetTerminal(conId);

            // need to call contructor just to initialize the std containers.
            new(conInfo) Terminal();

            Audit::Assert(m_connPool.Add(conId),
                L"Connection pool size error!");

            // no need to call constructor of socket yet.
            Audit::Assert(m_sockPool.Add(conId),
                L"Socket pool size error!");
            GetSocket(conId)->m_handle = (1ull << 32) | conId;

            Audit::Assert(m_sockPool.Add(conId + m_maxConnection),
                L"Socket pool size error!");
            GetSocket(conId + m_maxConnection)->m_handle = (1ull << 32)
                | (conId + m_maxConnection);
        }
    }


    // Called before setting up connection to create memory regions.
    //
    // Memory regions control tokens, which gives remote server access
    // to our memory. We don't want tokens to be shared by any two
    // remote servers. So we must destroy and recreate them every time
    //
    void HermesFixedRIO::CreateMemRegions(uint32_t conId)
    {
        auto pTerm = GetTerminal(conId);
        if (pTerm->m_pMemRegion != nullptr)
        {
            // we may inherite memory region from previous failed 
            // connection attempts
            Audit::Assert(pTerm->m_pTagRegion != nullptr, 
                L"Memory region state mismatch!");
            return;
        }

        auto buf = GetBufPair(conId);
        auto pArena = m_pScheduler->GetNewArena();

        // Register memory region for ring edges
        auto pTagRegion = (IND2MemoryRegion*)nullptr;
        auto er = m_pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion,
            m_hAdapterFile,
            reinterpret_cast<void**>(&pTagRegion));
        if (er != S_OK)
        {
            Audit::OutOfLine::Fail(er, L"Failed creating memory region.");
        }

        auto pOnTagRegistered = asyncLambda([pTerm, pTagRegion]
        (HRESULT hr, ActionArena* /*pArena*/) {
            if (hr != ND_SUCCESS) {
                Audit::OutOfLine::Fail(StatusCode(hr), 
                    L"Failed to register tag region.");
            }
            Audit::Assert(pTerm->m_pTagRegion == nullptr,
                L"Duplicate init of tag mem region.");
            pTerm->m_pTagRegion = pTagRegion;
        }, pArena);

        auto pTagOv = m_overlapPool.GetObj(pOnTagRegistered, pTagRegion);
        // Register circular edge info
        auto hrTag = pTagRegion->Register(
            pTerm, sizeof(Terminal),
            ND_MR_FLAG_ALLOW_LOCAL_WRITE |
            ND_MR_FLAG_ALLOW_REMOTE_READ |
            ND_MR_FLAG_RDMA_READ_SINK |
            ND_MR_FLAG_ALLOW_REMOTE_WRITE |
            ND_MR_FLAG_DO_NOT_SECURE_VM,
            &(pTagOv->OV));

        if (hrTag != ND_PENDING)
        {
            // Async call returned immediately, inline processing
            (*pOnTagRegistered)(hrTag, pArena);
            m_overlapPool.FreeObj(pTagOv);
        }

        // Register memory region for ring buffers
        auto pMemRegion = (IND2MemoryRegion*)nullptr;
        auto erm = m_pAdapter->CreateMemoryRegion(IID_IND2MemoryRegion,
            m_hAdapterFile,
            reinterpret_cast<void**>(&pMemRegion));
        if (erm != S_OK)
        {
            Audit::OutOfLine::Fail(er, L"Failed creating memory region.");
        }

        auto pOnMemRegistered = asyncLambda([pTerm, pMemRegion]
        (HRESULT hr, ActionArena* pArena) {
            if (hr != ND_SUCCESS) {
                Audit::OutOfLine::Fail(StatusCode(hr),
                    L"Failed to register buffer region.");
            }
            // busy wait for tag region init, crappy but simple
            while (pTerm->m_pTagRegion == nullptr) std::this_thread::yield();

            Audit::Assert(pTerm->m_pMemRegion == nullptr,
                L"Duplicate init of buffer mem region.");
            pTerm->m_pMemRegion = pMemRegion;

            pArena->Retire();
        }, pArena);

        auto pMemOv = m_overlapPool.GetObj(pOnMemRegistered, pMemRegion);
        // Register buffers
        auto hrMem = pMemRegion->Register(buf.data(),
            buf.size_bytes(),
            ND_MR_FLAG_ALLOW_LOCAL_WRITE |
            ND_MR_FLAG_ALLOW_REMOTE_READ |
            ND_MR_FLAG_RDMA_READ_SINK |
            ND_MR_FLAG_ALLOW_REMOTE_WRITE |
            ND_MR_FLAG_DO_NOT_SECURE_VM,
            &(pMemOv->OV));
        if (hrMem != ND_PENDING)
        {
            (*pOnMemRegistered)(hrMem, pArena);
            m_overlapPool.FreeObj(pMemOv);
        }
    }

    // Begin Listen on a certain port. automatically assigned if 0
    //
    StatusCode HermesFixedRIO::Listen(HSocketHandle handle, uint16_t port)
    {
        auto err = H_unexpected;
        auto pSock = GetSocket(HandleToId(handle));
        if (pSock == nullptr || pSock->m_handle != handle)
        {
            return StatusCode(E_HANDLE);
        }

        if (m_closing || pSock->m_state != HSocket::State::Disconnected)
        {
            err = StatusCode(E_NOT_VALID_STATE);
            wchar_t msg[128];
            swprintf_s(msg, L"Socket Listen issued on state %d", pSock->m_state);
            Tracer::LogError(err, msg);
            return err;
        }
        pSock->m_state = HSocket::State::StartToListen;

        auto cleanup = scopeGuard([pSock, &err] {
            pSock->m_state = HSocket::State::Disconnected;
            pSock->m_port = 0;
            if (pSock->m_pListen != nullptr)
            {
                pSock->m_pListen->Release();
                pSock->m_pListen = nullptr;
            }
            Tracer::LogError(err, L"Socket Listen Failed");
        });

        if (m_closing || pSock->m_state == HSocket::State::Stopping)
        {
            err = StatusCode(ND_CANCELED);
            return err;
        }

        err = (StatusCode)m_pAdapter->CreateListener(
            IID_IND2Listener,
            m_hAdapterFile,
            reinterpret_cast<void**>(&(pSock->m_pListen)));

        if (err != H_ok)  return err;

        sockaddr_in listenaddr;
        memset(&listenaddr, 0, sizeof(listenaddr));

        listenaddr.sin_family = AF_INET;
        listenaddr.sin_addr.S_un.S_addr = htonl(GetHomeIp());
        listenaddr.sin_port = htons(port);

        err = (StatusCode)pSock->m_pListen->Bind(
            reinterpret_cast<const sockaddr*>(&listenaddr),
            sizeof(listenaddr));
        if (err != H_ok)  return err;

        pSock->m_port = port;

        err = (StatusCode)pSock->m_pListen->Listen(0);
        if (err != H_ok)  return err;

        Audit::Assert(pSock->m_state == HSocket::State::StartToListen,
            L"Unexpect race condition detected in Listen.");
        pSock->m_state = HSocket::State::Listening;

        cleanup.dismiss();
        {
            wchar_t msg[128];
            swprintf_s(msg, L"Socket:[%p] Listening on Port:%u",
                this, pSock->m_port);
            Tracer::LogInfo(H_ok, msg);
        }

        return H_ok;
    }

    // Signal that one connection can be accepted. new socket
    // of that connection is provided async by connectionCallback
    //
    StatusCode HermesFixedRIO::Accept(
        HSocketHandle handle,

        // hook for Async return of the connected socket
        _In_ ConnectionNotifier& connectCallback,

        // A receive call back, will be assigned to the connected
        // socket, and triggered whenever a new message received
        // on that connected socket.
        // 
        _In_ RcvNotifier& rcvCallback
    ) 
    {
        auto err = StatusCode(E_HANDLE);
        auto pSock = GetSocket(HandleToId(handle));
        if (pSock == nullptr || pSock->m_handle != handle)  return err;

        if (m_closing || pSock->m_state != HSocket::State::Listening)
        {
            err = StatusCode(E_NOT_VALID_STATE);
            wchar_t msg[128];
            swprintf_s(msg, L"Socket Accept issued on state %d", pSock->m_state);
            Tracer::LogError(err, msg);
            return err;
        }

        // create a socket for the new connection
        auto pConnect = AllocateSocket(err);
        auto catchBlock = scopeGuard([this, &pConnect, &err] {
            if (pConnect != nullptr) {
                if (pConnect->m_term != INVALID_TERM_ID) {
                    FreeTerminal(pConnect->m_term);
                    pConnect->m_term = INVALID_TERM_ID;
                }
                FreeSocket(HandleToId(pConnect->m_handle));
                pConnect = nullptr;
            }
            Tracer::LogError(err, L"Socket Accept Failed.");
        });
        if (pConnect == nullptr) return err;
        pConnect->m_state = HSocket::State::Accept1;

        auto pListen = pSock->m_pListen;
        if (pListen == nullptr)
        {
            err = StatusCode(ND_CANCELED);
            return err;
        }
        pListen->AddRef();

        // Allocate connection block from root. The block has all the data
        // structure needed for send/receive
        //
        Audit::Assert(pConnect->m_term == INVALID_TERM_ID,
            L"Double allocation of connection block.");
        pConnect->m_term = AllocTerminal(err, pConnect->m_handle);
        if (pConnect->m_term == INVALID_TERM_ID) return err;

        auto pTerm = GetTerminal(pConnect->m_term);
        pTerm->m_pConnFunc = &connectCallback;
        pTerm->m_pRcvFunc = &rcvCallback;

        // Prepare for async call, completion redirect to ProcessConnReq
        // clean up delegate to the completion handler
        catchBlock.dismiss();

        auto pArena = m_pScheduler->GetNewArena();
        auto pOnConn = asyncLambda([this, pConnect, pListen]
        (HRESULT hr, ActionArena* pArena) {
            pListen->Release();
            this->ProcessConnReq(*pConnect, hr, pArena);
        }, pArena);

        auto pAsync = m_overlapPool.GetObj(pOnConn, pListen);
        auto hr = pListen->GetConnectionRequest
            (pTerm->m_pConnector, &(pAsync->OV));

        if (hr != ND_PENDING)
        {
            // Async call returned immediately, inline processing
            (*pOnConn)(hr, pArena);
            m_overlapPool.FreeObj(pAsync);
        }
        return H_ok;
    }


    // Called when we have a connection request arrived, i.e.
    // completion of Listen->GetConnectionRequest(connector);
    //
    void HermesFixedRIO::ProcessConnReq(HSocket& connSock, HRESULT hr, ActionArena* pArena)
    {
        auto err = StatusCode(hr);
        auto finalBlock = scopeGuard([this, &err, &connSock, &pArena]() {
            if (err != H_ok) {
                auto pT = GetTerminal(connSock.m_term);
                if (pT->m_pDisconnectCont != nullptr)
                {
                    pT->m_pDisconnectCont->Post(intptr_t(err), 0);
                }
                else {
                    auto notifier = pT->m_pConnFunc;
                    if (notifier != nullptr) {
                        (*notifier)(pT->m_handle, StatusCode(err));
                    }

                    // we have not sent out the token yet. so it is ok
                    // to not free the memory regions and reuse them
                    FreeTerminal(connSock.m_term);
                    connSock.m_term = INVALID_TERM_ID;
                    FreeSocket(HandleToId(connSock.m_handle));
                }
                Tracer::LogError(err,
                    L"Query Connection req failed!");
            }
            if (pArena != nullptr) {
                auto pa = pArena;	pArena = nullptr;
                pa->Retire(); // take the floor away under your foot
            }
        });

        if (err != H_ok) return;
        if (m_closing)
        {
            err = StatusCode(ND_CANCELED);
            return;
        }

        Audit::Assert(connSock.m_state == HSocket::State::Accept1,
            L"Unexpected state found in connection sock while querying connection req.");
        auto pTerm = GetTerminal(connSock.m_term);

        // Read the hand shake data from the connection request
        auto pHandShake = pArena->allocate<HandShake>();
        auto dlen = ULONG(sizeof(HandShake));
        err = (StatusCode)pTerm->m_pConnector->GetPrivateData(
            pHandShake, &dlen);
        if (err != H_ok && err != StatusCode(ND_BUFFER_OVERFLOW))
        {
            Tracer::LogError(err, L"Failed to retrieve connection data.");
            return;
        }

        // TODO!! should not crash due to possible remote attack.
        Audit::Assert(pHandShake->Tag.WriteEdge == 0
            && pHandShake->Tag.TrailingEdge == m_bufSize,
            L"Rmote protocol mismatch!");

        StartTerminal(connSock.m_term, *pHandShake);

        PrepareHandShakeData(connSock.m_term, *pHandShake);

        finalBlock.dismiss();
        // async processing redirect to OnConnAccepted
        // AND wait for connection request
        HSocket* pConnSock = &connSock;
        auto pOnAccepted = asyncLambda([this, pConnSock]
        (HRESULT hr, ActionArena* pArena) {
            this->OnConnAccepted(*pConnSock, hr, pArena);
        }, pArena);

        // Async acknowledge connection
        auto pAccOv = m_overlapPool.GetObj(pOnAccepted, pTerm->m_pConnector);
        auto result = pTerm->m_pConnector->Accept(
            pTerm->m_pQuePair,
            MAX_SGE,
            MAX_SGE,
            pHandShake,
            sizeof(*pHandShake),
            &pAccOv->OV);

        if (result != ND_PENDING)
        {
            // Async call returned immediately, inline processing
            (*pOnAccepted)(result, pArena);
            m_overlapPool.FreeObj(pAccOv);
        }

        wchar_t msg[256];
        swprintf_s(msg, L"Socket:%I64u ACCEPTING CONNECT.", connSock.m_handle);
        Tracer::LogInfo(err, msg);
    }


    // Called upon completion of the following two:
    // connector->Accept, i.e. remote client received our handshake data
    // connector->CompleteConnect, i.e. client connection request accepted by server
    //
    void HermesFixedRIO::OnConnAccepted(HSocket& connsock, HRESULT hr, ActionArena* pArena)
    {
        auto err = StatusCode(hr);
        auto finalBlock = scopeGuard([this, &err, &connsock, &pArena]() {
            if (err != H_ok) {
                auto pT = GetTerminal(connsock.m_term);
                Audit::Assert(pT != nullptr, L"Invalid terminal!");
                Audit::Assert(pT->m_pDisconnectCont != nullptr, 
                    L"Terminal not started when connection is ready.");
                pT->m_pDisconnectCont->Post(intptr_t(err), 0);
                Tracer::LogError(err,
                    L"Last step of Accept/Connect failed!");
            }
            if (pArena != nullptr) {
                auto pa = pArena;	pArena = nullptr;
                pa->Retire(); // take the floor away under your foot
            }
        });
        if (err != H_ok) return;

        Audit::Assert(connsock.m_state == HSocket::State::Accept1
            || connsock.m_state == HSocket::State::ConnAck,
            L"Unexpected state found when accepting connections");
        connsock.m_state = HSocket::State::ConnAgreed;
        auto pTerm = GetTerminal(connsock.m_term);

        sockaddr_in addr;
        auto uSize = ULONG(sizeof(addr));
        memset(&addr, 0, sizeof(addr));
        err = (StatusCode)
            pTerm->m_pConnector->GetPeerAddress((sockaddr*)&addr, &uSize);
        if (err != H_ok) return;

        pTerm->m_rmtIpAddr = ntohl(addr.sin_addr.s_addr);
        pTerm->m_rmtPort = ntohs(addr.sin_port);

        memset(&addr, 0, sizeof(addr));
        err = (StatusCode)
            pTerm->m_pConnector->GetLocalAddress((sockaddr*)&addr, &uSize);
        if (err != H_ok) return;

        connsock.m_port = ntohs(addr.sin_port);
        auto ip = ntohl(addr.sin_addr.S_un.S_addr);
        Audit::Assert(ip == m_homeIp, L"Local address mismatch!");

        // Register disconnect listener, allocated on the terminal
        // activity arena, so do not reclaim this arena, it would be
        // recycled when the activity stops.
        //
        auto pOnDisconNotified = asyncLambda([this, pTerm]
        (HRESULT /*hr*/, ActionArena* /*pAre*/) {
            pTerm->m_pDisconnectCont->Post((intptr_t)ERROR_GRACEFUL_DISCONNECT, 0);
        }, pTerm->m_pDisconnectCont->GetArena());

        auto pAccOv = m_overlapPool.GetObj
            (pOnDisconNotified, pTerm->m_pConnector); 
        auto disconnect = pTerm->m_pConnector->NotifyDisconnect(&pAccOv->OV);
        if (disconnect != ND_PENDING)
        {
            (*pOnDisconNotified)(disconnect, pTerm->m_pDisconnectCont->GetArena());
            m_overlapPool.FreeObj(pAccOv);
        }

        Audit::Assert(connsock.m_state == HSocket::State::ConnAgreed,
            L"Race condition in OnConnAccepted");

        connsock.m_state = HSocket::State::Connected;
        {
            wchar_t msg[256];
            swprintf_s(msg, L"Socket:[%I64u:%u] connected to %u:%u",
                connsock.m_handle, connsock.m_port,
                pTerm->m_rmtIpAddr, pTerm->m_rmtPort);
            Tracer::LogInfo(err, msg);
        }

        // Indicate connected to upper layer
        (*pTerm->m_pConnFunc)(pTerm->m_handle, H_ok);

        auto tmp = pArena; pArena = nullptr; tmp->Retire();
    }


    // Async call to connecting to a remote end point ==> WSA connect
    // new socket of the newly established connection is provided async
    // by connectionCallback
    StatusCode HermesFixedRIO::Connect(
        HSocketHandle handle,

        uint32_t remoteIp,
        uint16_t remotePort,

        // hook for async notification of connection success or failure
        // socket parameter should be this socket.
        _In_ ConnectionNotifier& connectCallback,

        // A receive call back, once connected, new messages received
        // will trigger this
        // 
        _In_ RcvNotifier& rcvCallback
    ) 
    {
        auto err = H_unexpected;
        auto pSock = GetSocket(HandleToId(handle));
        // Verify handle and socket state
        //
        if (pSock == nullptr || pSock->m_handle != handle)
        {
            return StatusCode(E_HANDLE);
        }

        if (m_closing || pSock->m_state != HSocket::State::Disconnected)
        {
            err = StatusCode(E_NOT_VALID_STATE);
            wchar_t msg[128];
            swprintf_s(msg, L"Socket Connect issued on state %d", pSock->m_state);
            Tracer::LogError(err, msg);
            return err;
        }

        pSock->m_state = HSocket::State::Connecting;
        auto pArena = m_pScheduler->GetNewArena();

        // Setup cleanup code.
        //
        auto cleanup = scopeGuard([this, pSock, &pArena, &err] {
            pSock->m_state = HSocket::State::Disconnected;
            pSock->m_port = 0;
            if (pSock->m_term != INVALID_TERM_ID)
            {
                FreeTerminal(pSock->m_term);
                pSock->m_term = INVALID_TERM_ID;
            }
            if (pArena != nullptr) {
                auto ptr = pArena; pArena = nullptr;
                ptr->Retire();
            }
            Tracer::LogError(err, L"Socket Connection Failed");
        });
        if (m_closing || pSock->m_state != HSocket::State::Connecting)
        {
            err = StatusCode(ND_CANCELED);
            return err;
        }

        // Allocate terminal
        //
        Audit::Assert(pSock->m_term == INVALID_TERM_ID,
            L"Double allocation of connection block.");
        pSock->m_term = AllocTerminal(err, pSock->m_handle);
        if (pSock->m_term == INVALID_TERM_ID) return err;

        auto pTerm = GetTerminal(pSock->m_term);
        pTerm->m_pConnFunc = &connectCallback;
        pTerm->m_pRcvFunc = &rcvCallback;

        // Bind
        //
        sockaddr_in bindaddr;

        memset(&bindaddr, 0, sizeof(bindaddr));
        bindaddr.sin_family = AF_INET;
        bindaddr.sin_addr.s_addr = htonl(m_homeIp);
        bindaddr.sin_port = htons(0);

        err = (StatusCode)pTerm->m_pConnector->Bind(
            (const struct sockaddr*)&bindaddr,
            sizeof(bindaddr));
        if (err != H_ok) return err;

        // Prepare connect parameters
        //
        sockaddr_in remoteAddr;
        memset(&remoteAddr, 0, sizeof(remoteAddr));

        remoteAddr.sin_family = AF_INET;
        remoteAddr.sin_addr.s_addr = htonl(remoteIp);
        remoteAddr.sin_port = htons(remotePort);

        // Register OnConnAck function to handle completion 
        // cleanup delegated to the completion handler
        cleanup.dismiss();
        auto pOnConnAck = asyncLambda([this, pSock]
        (HRESULT hr, ActionArena* pArena) {
            this->OnConnAck(*pSock, hr, pArena);
        }, pArena);

        auto pConnOv = m_overlapPool.GetObj(pOnConnAck, pTerm->m_pConnector);
        auto pHandShake = pArena->allocate<HandShake>();
        PrepareHandShakeData(pSock->m_term, *pHandShake);
        auto hr = pTerm->m_pConnector->Connect(
            pTerm->m_pQuePair,                  // Endpoint to use
            (const struct sockaddr*)&remoteAddr,// Remote address
            sizeof(remoteAddr),
            MAX_SGE,
            MAX_SGE,
            pHandShake,
            sizeof(*pHandShake),
            &pConnOv->OV); 

        if (hr != ND_PENDING) {
            // Async call returned immediately, inline processing
            (*pOnConnAck)(hr, pArena);
            m_overlapPool.FreeObj(pConnOv);
        }

        return err;
    }

    // Called on completion of async Connect call.
    // server accepted, need to complete connect
    //
    void HermesFixedRIO::OnConnAck(HSocket& sock, HRESULT hr, ActionArena* pArena)
    {
        // Setup cleanup routine
        auto err = StatusCode(hr);
        auto catchBlk = scopeGuard([this, &err, &sock, &pArena]() {
            if (err != H_ok) {
                CloseSocket(sock.m_handle, err);
                Audit::OutOfLine::Fail(err,
                    L"Query Connection req failed!");
            }
            if (pArena != nullptr) {
                auto pa = pArena;	pArena = nullptr;
                pa->Retire(); // take the floor away under your foot
            }
        });

        // verify states
        if (err != H_ok) return;
        Audit::Assert(sock.m_state == HSocket::State::Connecting,
            L"Unexpected socket state detected when complete connect.");
        sock.m_state = HSocket::State::ConnAck;

        auto pTerm = GetTerminal(sock.m_term);

        // Read the hand shake data from the connection request, 
        // we could reuse the memory by passing a pointer of handshake
        // from Connect. But I think that would create some confusion
        // that suggest some kind of data dependency when in fact there
        // is none.
        auto pHandShake = pArena->allocate<HandShake>();
        auto dlen = ULONG(sizeof(*pHandShake));
        err = (StatusCode)pTerm->m_pConnector->GetPrivateData(
            pHandShake, &dlen);
        if (err != H_ok && err != StatusCode(ND_BUFFER_OVERFLOW))
        {
            Tracer::LogError(err, L"Failed to retrieve connection data.");
            return;
        }
        // TODO!! should not crash due to possible remote attack.
        Audit::Assert(pHandShake->Tag.WriteEdge == 0
            && pHandShake->Tag.TrailingEdge == m_bufSize,
            L"Rmote protocol mismatch!");

        StartTerminal(sock.m_term, *pHandShake);

        catchBlk.dismiss();
        err = H_ok;
        // async processing redirect to OnConnAccepted
        // AND wait for connection request
        HSocket* pConnSock = &sock;
        auto pOnAccepted = asyncLambda([this, pConnSock]
        (HRESULT hr, ActionArena* pA) {
            this->OnConnAccepted(*pConnSock, hr, pA);
        }, pArena);

        // Asyc complete connect
        auto pAccOv = m_overlapPool.GetObj(pOnAccepted, pTerm->m_pConnector);
        auto result = pTerm->m_pConnector->CompleteConnect(&pAccOv->OV);

        if (result != ND_PENDING)
        {
            // Async call returned immediately, inline processing
            (*pOnAccepted)(result, pArena);
            m_overlapPool.FreeObj(pAccOv);
        }

        wchar_t msg[256];
        swprintf_s(msg, L"Socket:[%I64u] Connect complete.", sock.m_handle);
        Tracer::LogInfo(err, msg);
    }


    // Async send, notification of completion will be carried by sendCallback
    //
    StatusCode HermesFixedRIO::Send(
        HSocketHandle handle,

        // place holder for the client to identify this operation
        // will be pass back to client in sendCallback
        _In_ void* sndContext,

        // Buffer must be available for the duration of the async
        // operation, until notified by sendCallback
        _In_ const gsl::span<gsl::byte> buf,

        // A call back, async notification of send completion
        _In_ SendCompletionNotifier& sendCallback
    ) 
    {
        if (buf.size_bytes() > hermes::MAX_MSG_SIZE)
            return StatusCode(E_INVALIDARG);

        auto err = StatusCode(E_HANDLE);
        auto pSock = GetSocket(HandleToId(handle));
        if (pSock == nullptr || pSock->m_handle != handle)  return err;

        if (m_closing || pSock->m_state != HSocket::State::Connected)
        {
            err = StatusCode(E_NOT_VALID_STATE);
            wchar_t msg[128];
            swprintf_s(msg, L"Socket Send issued on state %d", pSock->m_state);
            Tracer::LogError(err, msg);
            return err;
        }
        if (buf.size_bytes() < 0
            || ((size_t(buf.size_bytes())) & (hermes::MSG_ALIGN - 1)) != 0)
        {
            err = StatusCode(E_INVALIDARG);
            Tracer::LogError(err, L"Send called with illegal message size.");
            return err;
        }

        auto pTerm = GetTerminal(pSock->m_term);
        Audit::Assert(pTerm->m_handle == handle,
            L"Mismatched handle value found.");

        SndReq req{ sndContext, buf, &sendCallback };
        {
            Exclude<SRWLOCK> guard{ pTerm->m_sndLock };
            pTerm->m_sndQue.insert(pTerm->m_sndQue.begin(), req);

            if (pTerm->m_sndRunning)
            {
                return H_ok;
            }
            pTerm->m_sndRunning = true; 
        }

        ProcessSendQ(pSock->m_term);
        return H_ok;
    }

    // Put send request to circular buffer and write to remote,
    // read remote trailing edge,
    // Expect caller to ensure this function does not overlap
    // with itself on the same terminal
    //
    void HermesFixedRIO::ProcessSendQ(uint32_t termId)
    {
        auto pTerm = GetTerminal(termId);
        auto sendBuf = GetSndBuf(termId);

        Audit::Assert(!pTerm->m_sndQue.empty() && pTerm->m_sndRunning,
            L"Send q not ready or exclusive tag not set.");

        for (;;) {
            SndReq req = pTerm->m_sndQue.back();
            auto srcBuf = req.m_buf.subspan(
                pTerm->m_mSndIdx * Terminal::PACKET_SIZE);

            auto available = pTerm->m_sndEnd.TrailingEdge
                - pTerm->m_sndEnd.WriteEdge;

            auto pktSize = std::min(size_t(srcBuf.size_bytes()),
                Terminal::PACKET_SIZE) + 2 * sizeof(MsgEnd);
            if (pktSize > available)
            {
                // Not enough space left in the ring buffer
                pTerm->m_sndRunning = false;
                pTerm->ReadRmtEdge();
                return;
            }

            auto sequence = pTerm->m_mSndIdx;
            if (sequence == 0 && srcBuf.size_bytes() > Terminal::PACKET_SIZE)
            {
                auto numPckts = 
                    (size_t(srcBuf.size_bytes()) + Terminal::PACKET_SIZE - 1)
                    / Terminal::PACKET_SIZE;
                Audit::Assert(numPckts < INT16_MAX && numPckts > 1,
                    L"Message size too big!");
                sequence = 0 - int16_t(numPckts);
            }

            // Put down template and increment Writing edge
            DFrame frame;
            LocateInRingBuf(sendBuf, pTerm->m_sndEnd.WriteEdge, pktSize, frame);
            pTerm->m_sndEnd.WriteEdge += pktSize;

            uint32_t leftcrc = 0ul;
            uint32_t rightcrc = 0ul;

            ComputeCRC(frame.m_views[0], 
                srcBuf.subspan(0, frame.m_views[0].size_bytes()),
                leftcrc, rightcrc);
            ComputeCRC(frame.m_views[1], 
                srcBuf.subspan(frame.m_views[0].size_bytes(), 
                    frame.m_views[1].size_bytes()),
                leftcrc, rightcrc);

            frame.m_pEnd->Crc = uint64_t(leftcrc) << 32 | rightcrc;
            frame.m_pEnd->Size = uint32_t(srcBuf.size_bytes());
            frame.m_pEnd->Seq = sequence;
            frame.m_pEnd->Type = (sequence == 0) ? 
                MsgType::Single : MsgType::Multi;
            *frame.m_pStart = *frame.m_pEnd;

            // issue RDMA write, fire and forget. 
            //
            auto rmtToken = pTerm->m_rmtMemToken;
            if (rmtToken == 0)
            {
                // the connection is closing, we are in an inconsistent state
                // but since all the buffers will be reclaimed, so drop 
                // everything
                pTerm->m_sndRunning = false;
                return;
            }

            ND2_SGE sge;
            sge.Buffer = frame.m_pStart;
            sge.BufferLength = ULONG(pktSize);
            sge.MemoryRegionToken = pTerm->m_pMemRegion->GetLocalToken();

            if (frame.m_pEnd < frame.m_pStart) {
                // the data record frame is splitted by ring buffer end
                sge.BufferLength = ULONG(sizeof(MsgEnd)
                    + frame.m_views[0].size_bytes());

                ND2_SGE secondHalf;
                secondHalf.Buffer = frame.m_views[1].data();
                secondHalf.BufferLength = ULONG(sizeof(MsgEnd)
                    + frame.m_views[1].size_bytes());
                secondHalf.MemoryRegionToken =
                    pTerm->m_pMemRegion->GetLocalToken();
                auto err = pTerm->m_pQuePair->Write(nullptr, &secondHalf, 1,
                    pTerm->m_rmtBufAddr, rmtToken,
                    0);
                if (err != ND_SUCCESS && err != ND_PENDING)
                {
                    Audit::OutOfLine::Fail(StatusCode(err),
                        L"RDMA write failure, buffer length: %u", secondHalf.BufferLength);
                }
            }

            auto offset = (char*)sge.Buffer - (char*)sendBuf.data();

            auto hr = pTerm->m_pQuePair->Write(nullptr, &sge, 1,
                pTerm->m_rmtBufAddr + offset, rmtToken,
                0);

            if (hr != ND_SUCCESS && hr != ND_PENDING && hr != ND_CANCELED)
            {
                Audit::OutOfLine::Fail(StatusCode(hr),
                    L"RDMA write failure, buffer length: %u", sge.BufferLength);
            }
            if (hr == ND_PENDING) hr = ND_SUCCESS;

            // issue read, in order to update trailing edge
            // 
            pTerm->ReadRmtEdge();

            if (srcBuf.size_bytes() > Terminal::PACKET_SIZE)
            {
                // A multi message and we have not finished sending all
                // segments.  So go to begining of the loop to send next.
                pTerm->m_mSndIdx++;
                continue;
            }

            // notify caller send operation finished, they can release
            // buffer
            pTerm->m_mSndIdx = 0;
            (*req.m_pCallback)(StatusCode(hr), req.m_context, req.m_buf);

            {
                Exclude<SRWLOCK> guard{ pTerm->m_sndLock };
                Audit::Assert(req.m_buf.data()
                    == pTerm->m_sndQue.back().m_buf.data(),
                    L"Race condition detected in process send queue.");
                pTerm->m_sndQue.pop_back();
                if (pTerm->m_sndQue.empty())
                {
                    pTerm->m_sndRunning = false;
                    return;
                }
            } // critical section
        } // loop for send request que
    }

    bool HermesFixedRIO::TryCloseSocket(HSocket* pSock, StatusCode err)
    {
        if (pSock->m_state == HSocket::State::Disconnected)
        {
            pSock->m_state = HSocket::State::Stopping;
            FreeSocket(HandleToId(pSock->m_handle));
            return true;
        }

        if (pSock->m_state == HSocket::State::Connected)
        {
            Audit::Assert(pSock->m_term != INVALID_TERM_ID,
                L"Connected socket should have a terminal.");
            auto pTerm = GetTerminal(pSock->m_term);
            pTerm->m_pDisconnectCont->Post(intptr_t(err), 0);
            return true;
        }

        if (pSock->m_state == HSocket::State::Listening)
        {
            Audit::Assert(pSock->m_pListen != nullptr,
                L"Listener expected from a listening socket.");
            auto pListen = pSock->m_pListen;
            pSock->m_pListen = nullptr;
            pSock->m_state = HSocket::State::Stopping;
            pSock->m_port = 0;
            pListen->Release();
            FreeSocket(HandleToId(pSock->m_handle));
            return true;
        }
        return false;
    }

    void HermesFixedRIO::CloseSocket(HSocketHandle handle, StatusCode err) 
    {
        auto pSock = GetSocket(HandleToId(handle));
        if (pSock == nullptr || pSock->m_handle != handle)  return;

        for (auto i = 0; i < 10000; i++)
        {
            if (pSock->m_state == HSocket::State::Inpool)
                return;

            if (TryCloseSocket(pSock, err))
                return;
            // busy waiting as the socket is in transition state;
            std::this_thread::yield();
        }
        Audit::OutOfLine::Fail(H_unexpected, 
            L"Socket in transition state for too long: %d",
            int(pSock->m_state));
    }

    // Constructor, must be called from the factory below for additional check
    //
    HermesFixedRIO::HermesFixedRIO(
        // Don't have dynamic memory management, so number of connection
        // is fixed
        //
        size_t maxConcurrentConnections,

        // Registered buffer is fixed, currently we don't support message
        // larger than this size
        //
        size_t bufferSize,

        std::unique_ptr<mbuf::LargePageBuffer>& pPage
    )
        : m_bufSize(uint32_t(bufferSize))
        , m_maxConnection(uint32_t(maxConcurrentConnections))
        , m_pMemory(std::move(pPage))
        , m_connPool(uint32_t(maxConcurrentConnections))
        , m_sockPool(uint32_t(maxConcurrentConnections << 1))
        , m_overlapPool(SI::Ki)
        , m_poller(*this)
    {
        //  Open ND adapter
        //
        HRESULT hr = NdStartup();
        if (FAILED(hr)) {
            Audit::OutOfLine::Fail(StatusCode(hr),
                L"Could not initialize network direct library!");
        }

        m_pBuffers = mbuf::FixedBufManagerFactory();

        // Locate a valid physical address and bind to it.
        std::vector<in_addr> ipAddrs{};
        if (0 == ListIpAddresses(ipAddrs))
        {
            NdCleanup();
            Audit::OutOfLine::Fail(L"could not list adapter IP addresses");
        }

        // TODO!! Be able to bind to user specified adapter.
        sockaddr_in bindAddr;
        std::memset(&bindAddr, 0, sizeof(bindAddr));
        bindAddr.sin_family = AF_INET;
        bindAddr.sin_addr.S_un.S_addr = ipAddrs[0].S_un.S_addr;
        bindAddr.sin_port = htons(0);

        m_homeIp = ntohl(bindAddr.sin_addr.S_un.S_addr);

        hr = NdOpenAdapter(IID_IND2Adapter,
            (const struct sockaddr*)&bindAddr,
            sizeof(bindAddr),
            (void **)&m_pAdapter);

        if (FAILED(hr)) {
            NdCleanup();
            Audit::OutOfLine::Fail(StatusCode(hr),
                L"Could not load ND adaptor.");
        }

        // Connect to IOCP
        m_pAdapter->CreateOverlappedFile(&m_hAdapterFile);

        m_QueuePairsPool = GetQuePairPoolInterface(m_pAdapter, m_hAdapterFile);
        SliceNregister();

        m_hIocp = CreateIoCompletionPort(m_hAdapterFile,
            nullptr,

            // use special value so that event raised on this file
            // trigger the primary action in the IOCP thread pool,
            // which will be assocated with "this" later
            Scheduler::KEY_PRIMARY_ACTION,
            0);
        if (m_hIocp == nullptr) {
            NdCleanup();
            Audit::OutOfLine::Fail(L"Could not create IOCP.");
        }

        // OK, now we are ready to get the party started.
        m_pScheduler = SchedulerIOCPFactory(IOCPhandle(m_hIocp), 8, this);
        m_pScheduler->Post(&m_poller, 0, 0);
    }

} // namespace internal

namespace hermes
{

    std::unique_ptr<Hermes>
        HermesFixedRIOFactory(
            // Size of buffer allocated per connection.
            // will be split equally for sending and receiving
            //
            size_t conBufSize,

            // Number of concurrent connections. Need this now
            // since we don't have dynamic management yet.
            //
            uint32_t maxConnections
        )
    {
        Audit::Assert(conBufSize < 16 * SI::Mi && conBufSize > 4*SI::Ki,
            L"Buffer size out of bound.");
        Audit::Assert(mbuf::IsPower2(conBufSize), L"Buffer size must be power of 2.");

        Audit::Assert(maxConnections < 100 * SI::Ki,
            L"Do not support massive scale yet.");
        Audit::Assert(mbuf::IsPower2(maxConnections),
            L"maxConnections must be power of 2 in this prototype.");

        Tracer::InitializeLogging(L"hermes.etl");

        // Need to allocate a buffer and a book keeping block for each
        // connection

        // For each connection, we need to allocate 2 buffers (conBufSize)
        // 2 socket (leave room for server socket) and 1 connection block
        // The computation here is tightly coupled with the constructor
        // since we need the allocate the right size pool for connections
        // and sockets there
        //
        size_t allocatedSize;
        uint32_t allocatedBuffers;
        auto tSize = conBufSize + sizeof(hinternal::Terminal)
            + 2 * sizeof(hinternal::HSocket);

        auto pMem = std::make_unique<mbuf::LargePageBuffer>(tSize,
            maxConnections, allocatedSize, allocatedBuffers);
        Audit::Assert(allocatedBuffers >= maxConnections,
            L"can not allocate enough memory for registered buffers");
        // TODO a little bit of memory waste at the end of the large page.

        return std::make_unique<hinternal::HermesFixedRIO>(maxConnections, conBufSize >> 1, pMem);
    }

}