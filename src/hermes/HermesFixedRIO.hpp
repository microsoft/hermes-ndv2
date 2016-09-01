// HermesFixedRIO.hpp
// Define an implementation of Hermes interface that uses
// registered circular buffer and read/write verbs
//
// Each connection is split into two channels. One channel for receive
// and one for send. For each channel there are two circular buffers.
// The sender place data in the outgoing messages in the local "send"
// buffer and try to write the remote "receive" buffer at the same
// location. Read the remote "receive" buffer's trailing edge after
// that.
//
// Trailing edge and writing edge forms natural flow control.
// Bigger messages (bigger than buffer size - headers) are not supported
// at this point.
//

#pragma once
#include "stdafx.h"

#include <vector>
#include <thread>

#include "FMem.hpp"
#include "internal.hpp"
#include "MiscWin.hpp"
#include "Utilities.hpp"

using namespace hermes;
using namespace Utilities;
using namespace Schedulers;

namespace hinternal
{

    // Book keeping for a circular buffers. Two for each connection
    // The receiving end CirTag should be registered together with
    // the circular buffer so the sending end can read the trailing
    // edge of the receiving end
    //
    struct CirTag
    {
        uint64_t TrailingEdge;
        uint64_t WriteEdge;
    };

    // Initial data exchange when estabilishing connection
    //
    struct HandShake
    {
        CirTag Tag;
        uint64_t BufAddr;
        uint64_t TagAddr;

        uint32_t MemToken;
        uint32_t TagToken;
    };

    class VerbCompletion;

    // Data structure for processing send/receive for a connection
    // Hermes create same number of Terminal and buffers pairs.
    // So one Terminal also currespond to one registered buffer pair
    //
    struct Terminal
    {
        static const int MAX_PENDING_READ = 2;

        // big blobs will be divided into multiple packets
        static const size_t PACKET_SIZE = 32 * SI::Ki;

        CirTag m_rcvEnd;
        CirTag m_sndEnd;

        // Points to the parent socket
        HSocketHandle m_handle;

        // regions can not be shared among connections, for security.
        IND2MemoryRegion *m_pMemRegion = nullptr;
        IND2MemoryRegion *m_pTagRegion = nullptr;

        IND2QueuePair* m_pQuePair;
        IND2CompletionQueue*  m_pCQ;
        IND2Connector* m_pConnector;

        // Async Notifiers
        ConnectionNotifier* m_pConnFunc;
        RcvNotifier* m_pRcvFunc;

        // Continuation to post received message to user
        ContinuationBase* m_pRcvCont;
        ContinuationBase* m_pDisconnectCont;
        VerbCompletion* m_pVerbCompletion;

        // A locked send que, we queue up send request when the send ring
        // buffer is full.
        std::vector<SndReq> m_sndQue;
        SRWLOCK m_sndLock;
        std::atomic<int> m_pendingRead = 0;

        uint32_t m_rmtIpAddr;
        uint16_t m_rmtPort;

        // Bigger messages are divided into multiple packets. Each
        // connection can not have more than one long message on each
        // direction at the same time.
        // Index value of the first packet would be 0 - num_total_packets
        // so if the message is 5 packet long, each of its packet index
        // would be: -5 1 2 3 4
        // So 0 is only for single packet messages.
        //
        int16_t m_mSndIdx;

        // total number of packets of the current receiving multi-message;
        int16_t m_mRcvSize;
        int16_t m_mRcvIdx;

        // buffer for collecting the content of the receiving multi-message
        mbuf::DisposableBuffer* m_rcvBuf;

        // No two threads should process the sending of a connection at
        // the same time. use this flag and the lock above to ensure this.
        bool m_sndRunning;

        // we are in the middle of calling client receive call back
        bool m_rcvProcessing;

        // Token used to write to remote buffer
        // duplicate as flag indicating the channel is still open
        //
        uint32_t m_rmtMemToken;
        uint32_t m_rmtTagToken;

        // Remote memory address for RDMA write and read operations
        uint64_t m_rmtBufAddr;
        uint64_t m_rmtTagAddr;


        Terminal()
        {
            VoidFlds();
        }

        void VoidFlds()
        {
            m_rcvEnd.TrailingEdge = 0;
            m_rcvEnd.WriteEdge = 0;
            m_sndEnd.TrailingEdge = 0;
            m_sndEnd.WriteEdge = 0;

            m_handle = HSocketHandle(INVALID_HANDLE_VALUE);

            m_pConnector = nullptr;
            m_pQuePair = nullptr;
            m_pCQ = nullptr;

            m_pConnFunc = nullptr;
            m_pRcvFunc = nullptr;

            m_pRcvCont = nullptr;
            m_pDisconnectCont = nullptr;
            m_pVerbCompletion = nullptr;

            m_mSndIdx = 0;
            m_mRcvSize = 0;
            m_mRcvIdx = 0;
            m_rcvBuf = nullptr;

            m_sndRunning = false;
            m_rcvProcessing = false;

            m_rmtBufAddr = 0;
            m_rmtTagAddr = 0;
            m_rmtMemToken = 0;
            m_rmtTagToken = 0;
            m_rmtIpAddr = 0;
            m_rmtPort = 0;
        }

        // Usually called when a connection is about to start,
        // we save the keys the remote end handed to us
        //
        void SaveRemoteKeys(const HandShake& remote)
        {
            m_rmtBufAddr = remote.BufAddr;
            m_rmtTagAddr = remote.TagAddr;
            m_rmtMemToken = remote.MemToken;
            m_rmtTagToken = remote.TagToken;
        }

        // Read the edges of remote receive buffer and update
        // the edges of local send buffer.
        //
        void ReadRmtEdge()
        {
            auto rmtToken = m_rmtTagToken;
            if (rmtToken == 0)
            {
                // connection is closing, abandon ship
                return;
            }

            if (m_pendingRead.load() >= MAX_PENDING_READ) {
                return;
            }
            m_pendingRead++;

            ND2_SGE sge;
            sge.Buffer = &m_sndEnd.TrailingEdge;
            sge.BufferLength = sizeof(m_sndEnd.TrailingEdge);
            sge.MemoryRegionToken = m_pTagRegion->GetLocalToken();

            auto hr = m_pQuePair->Read(this, &sge, 1, m_rmtTagAddr, rmtToken, 0);

            if (hr != ND_SUCCESS && hr != ND_PENDING
                && hr != ND_CANCELED) {
                Audit::OutOfLine::Fail(StatusCode(hr), L"Read remote edge failed.");
            }
        }

        StatusCode Prepare(uint32_t bufSize,
            IND2QueuePairsPool* qpPool,
            IND2Adapter* pAdapter, HANDLE adapterFile)
        {
            Audit::Assert(m_pQuePair == nullptr
                && m_pCQ == nullptr
                && m_pConnector == nullptr,
                L"ND resources leaking detected!");

            m_rcvEnd.TrailingEdge = bufSize;
            m_rcvEnd.WriteEdge = 0;
            m_sndEnd.TrailingEdge = bufSize;
            m_sndEnd.WriteEdge = 0;

            return InitQues(qpPool, pAdapter, adapterFile,
                m_pQuePair, m_pCQ, m_pConnector);
        }

        void Cleanup()
        {
            if (m_pConnector != nullptr) {
                m_pConnector->Release();
                m_pConnector = nullptr;

                m_pQuePair->Release();
                m_pQuePair = nullptr;

                m_pCQ->Release();
                m_pCQ = nullptr;
            }
            if (m_rcvBuf != nullptr) {
                m_rcvBuf->Dispose();
                m_rcvBuf = nullptr;
            }
            VoidFlds();
        }

    };

    // Data structure for Hermes socket. The library should maintain
    // a table of these sockets, and gives user the handles that index
    // into the table.
    //
    // Pity we can not merge HSocket with Terminal, as server
    // sockets have no connections. Wish we can provide Java like
    // interface, forcing users to distinguish server socket and normal
    // ones.
    //
    struct HSocket
    {
        HSocket& operator=(const HSocket&) = delete;
        HSocket(const HSocket&) = delete;

        // NDv2 Listener, only used by server socket that accepting
        // connections
        IND2Listener* m_pListen = nullptr;

        // Index into connection pool
        uint32_t m_term = INVALID_TERM_ID;

        uint16_t m_port = 0;

        enum class State : uint8_t
        {
            Inpool = 0,

            StartToListen,
            Listening,

            // On server, a connection request received
            Accept1, 

            // a client trying to connect
            Connecting,

            // Both server and client, both accepted
            ConnAgreed,

            // a client received conntion ack from server,
            // i.e. Connect() completed
            ConnAck, 

            Connected,

            Stopping,
            Disconnected
        };

        State m_state = State::Inpool;

        // Socket handle is just a index to socket object table. we use the
        // extra bits here to defend against dangling handle
        HSocketHandle m_handle;
    };

    enum class MsgType : uint16_t
    {
        Single,
        Multi // one segment that belongs to a sequence of messages
    };

    // Header and footer of the message
    struct MsgEnd
    {
        uint64_t Crc;
        uint32_t Size;
        int16_t Seq; // sequence number for multi messages. 
        MsgType Type;
    };
    static_assert(sizeof(MsgEnd) == MSG_ALIGN, "Header & footer should align");

    class HermesFixedRIO;

    // Thread in a tight loop to poll all terminals for incoming data
    //
    class Poller : public Work
    {
        HermesFixedRIO& m_root;
        uint32_t m_idx = 1ul;
    public:
        Poller(HermesFixedRIO& root)
            : m_root(root)
        {}

        void Run(
            _In_ WorkerThread&  thread,
            intptr_t       continuationHandle,
            uint32_t       length
        ) override;
    };


    // Implementation of Hermes interface, root of the library. Using fixed
    // ring buffers for connection. 
    //
    // Currently the max number of connections is also fixed. 
    // We plan to group connection data structure into pages and develop
    // dynamic management of these pages to remove this restriction later.
    //
    class HermesFixedRIO : public Hermes, public Work
    {
        friend class Poller;
        friend class Conveyor;
        friend class TerminalCloser;
        friend class VerbCompletion;

        HermesFixedRIO() = delete;
        HermesFixedRIO(const HermesFixedRIO&) = delete;
        HermesFixedRIO& operator=(const HermesFixedRIO&) = delete;

        // ND Adapter related
        //
        IND2Adapter *m_pAdapter;
        IND2QueuePairsPool *m_QueuePairsPool;

        // IOCP related
        //
        HANDLE m_hAdapterFile;
        HANDLE m_hIocp;
        std::unique_ptr<Scheduler> m_pScheduler;

        Poller m_poller;

        // Buffers, connections and sockets
        std::unique_ptr<mbuf::LargePageBuffer> m_pMemory;
        gsl::span<gsl::byte> m_buffers;
        gsl::span<Terminal> m_connections;
        gsl::span<HSocket> m_sockTbl;

        // Pools for connections and sockets, number is used to index into
        // connection object array.
        // Note the index we give user start with 1, leaving 0 as
        // invalid to comform to nullptr value.
        //
        mbuf::CircularQue<uint32_t> m_connPool;
        mbuf::CircularQue<uint32_t> m_sockPool;

        HOverlapPool m_overlapPool;

        std::unique_ptr<mbuf::BufManager> m_pBuffers;

        const uint32_t m_bufSize;
        const uint32_t m_maxConnection;
        uint32_t m_homeIp;

        bool m_closing = false;

        // Allocate a socket structure
        // return index to the socket table
        //
        HSocket* AllocateSocket(StatusCode& err)
        {
            uint32_t id = INVALID_SOCK_ID;
            auto success = m_sockPool.Get(id);
            if (!success) {
                err = StatusCode(E_NOT_SUFFICIENT_BUFFER);
                return nullptr;
            }

            auto& sock = *GetSocket(id);
            Audit::Assert(sock.m_state == HSocket::State::Inpool,
                L"Socket state changed while in socket pool!");
            Audit::Assert(sock.m_term == INVALID_TERM_ID
                && sock.m_pListen == nullptr,
                L"Socket state not cleared.");
            Audit::Assert(HandleToId(sock.m_handle) == id,
                L"Lower 32b of handle should be the index");

            sock.m_state = HSocket::State::Disconnected;
            err = H_ok;
            return &sock;
        }

        void FreeSocket(uint32_t id)
        {
            auto& sock = *GetSocket(id);
            Audit::Assert(sock.m_state != HSocket::State::Inpool,
                L"Double free of socket");
            sock.m_state = HSocket::State::Inpool;
            sock.m_handle += 1ull << 32; // defend against dangling handles
            Audit::Assert(HandleToId(sock.m_handle) == id,
                L"Lower 32b of handle should be the index");

            Audit::Assert(m_sockPool.Add(id),
                L"Unexpected sock pool overflow during deallocation");
        }

        uint32_t HandleToId(HSocketHandle handle)
        {
            return uint32_t(handle);
        }

        HSocket* GetSocket(uint32_t id)
        {
            // user id start from 0, but vector index starts from 0
            if (id >= 1 && id <= m_sockPool.GetCapacity())
                return &m_sockTbl[id - 1];
            else return nullptr;
        }

        // Allocate a handle to all data structure needed for a connected
        // socket, including buffer pair, queue pair, completion queue
        // connector.
        // 
        // When failed, returns INVALID_TERM_ID with error code set
        // to err parameter
        //
        uint32_t AllocTerminal(StatusCode& err, HSocketHandle parent)
        {
            uint32_t id;
            Audit::Assert(m_connPool.Get(id),
                L"Exhausted connection!");

            auto ptr = GetTerminal(id);
            Audit::Assert(ptr->m_rcvEnd.TrailingEdge == 0
                && ptr->m_sndEnd.TrailingEdge == 0,
                L"Invalid connection state from connection pool");

            err = ptr->Prepare(m_bufSize,
                m_QueuePairsPool, m_pAdapter, m_hAdapterFile);
            if (err != H_ok)
            {
                ptr->m_sndEnd.TrailingEdge = m_bufSize;
                FreeTerminal(id);
                return INVALID_TERM_ID;
            }
            CreateMemRegions(id);
            ptr->m_handle = parent;
            return id;
        }

        // Deallocate connection information and buffers, release all ND
        // related resources
        //
        void FreeTerminal(uint32_t id)
        {
            auto ptr = GetTerminal(id);
            Audit::Assert(ptr->m_rcvEnd.TrailingEdge != 0
                && ptr->m_sndEnd.TrailingEdge != 0,
                L"Double free to connection pool");

            ptr->Cleanup();
            Audit::Assert(m_connPool.Add(id),
                L"Connection pool internal error.");
        }

        Terminal* GetTerminal(uint32_t conId)
        {
            conId--; // user index start with 1
            Audit::Assert(conId < m_maxConnection,
                L"Invalid Connection Id");
            return &(m_connections[conId]);
        }

        // Start the terminal as the passengers are coming
        //
        void StartTerminal(uint32_t termId, const HandShake& handShake);

        gsl::span<gsl::byte> GetBufPair(uint32_t conId)
        {
            conId--; // user index start with 1
            Audit::Assert(conId < m_maxConnection,
                L"Invalid Connection Id");

            // two buffers per connection
            auto offset = conId * m_bufSize * 2;

            return m_buffers.subspan(offset, m_bufSize * 2);
        }

        gsl::span<gsl::byte> GetRcvBuf(uint32_t conId)
        {
            conId--; // user index start with 1
            Audit::Assert(conId < m_maxConnection,
                L"Invalid Connection Id");

            // two buffers per connection
            auto offset = conId * m_bufSize * 2;

            return m_buffers.subspan(offset, m_bufSize);
        }

        gsl::span<gsl::byte> GetSndBuf(uint32_t conId)
        {
            conId--; // user index start with 1
            Audit::Assert(conId < m_maxConnection,
                L"Invalid Connection Id");

            // two buffers per connection and we want the later one
            auto offset = conId * m_bufSize * 2 + m_bufSize;

            return m_buffers.subspan(offset, m_bufSize);
        }

        void PrepareHandShakeData(uint32_t termId, _Out_ HandShake& res)
        {
            auto pTerm = GetTerminal(termId);
            res.Tag.WriteEdge = pTerm->m_rcvEnd.WriteEdge;
            res.Tag.TrailingEdge = pTerm->m_rcvEnd.TrailingEdge;
            res.TagAddr = uint64_t(&pTerm->m_rcvEnd.TrailingEdge);
            res.BufAddr = uint64_t(GetRcvBuf(termId).data());

            while (pTerm->m_pMemRegion == nullptr || pTerm->m_pTagRegion == nullptr)
            {
                // really crappy busy waiting!!! Hopefully it happens rarely
                // as we leave enough room between this and register of memory
                // regions.
                std::this_thread::yield();
            }
            res.MemToken = pTerm->m_pMemRegion->GetRemoteToken();
            res.TagToken = pTerm->m_pTagRegion->GetRemoteToken();
        }

        // Called before setting up connection to create memory regions.
        //
        // Memory regions control tokens, which gives remote server access
        // to our memory. We don't want tokens to be shared by any two
        // remote servers. So we must destroy and recreate them every time
        //
        void CreateMemRegions(uint32_t conId);

        // Try to close a socket if it is in a stead state:
        // connected, listening or disconnected.
        // return whether successfully closed
        //
        bool TryCloseSocket(HSocket * pSock, StatusCode err);

        //
        // Initialization sub contractors, must be called from constructor
        // putting into seperate functions just for readability
        // 

        // Slice large page buffers into array of buffer pairs Terminal
        //
        void SliceNregister();

    public:
        // Constructor, must be called from the factory;
        HermesFixedRIO(
            size_t maxConcurrentConnections,
            size_t bufferSize,
            std::unique_ptr<mbuf::LargePageBuffer>& pPage
        );

        uint32_t GetHomeIp() override { return m_homeIp; }

        HSocketHandle CreateSocket(
            _Out_ StatusCode& error
        ) override
        {
            if (m_closing) {
                error = StatusCode(ND_CANCELED);
                return intptr_t(INVALID_HANDLE_VALUE);
            }

            auto pSock = AllocateSocket(error);
            if (pSock == nullptr) {
                return intptr_t(INVALID_HANDLE_VALUE);
            }
            // no hurry to allocate connection or setup receive
            // thred yet, wait until accept or connect

            return pSock->m_handle;
        }

        ~HermesFixedRIO()
        {
            m_closing = true;
            auto workToDo = true;
            while (workToDo)
            {
                workToDo = false;
                for (auto id = uint32_t(1); id <= m_sockPool.GetCapacity(); id++)
                {
                    auto& sock = m_sockTbl[id - 1];
                    if (sock.m_state == HSocket::State::Inpool)
                        continue;

                    workToDo = true;
                    wchar_t msg[256];
                    swprintf_s(msg, L"Closing socket %u, in state %u", 
                        id, sock.m_state);
                    Tracer::LogWarning(H_unexpected, msg);
                    TryCloseSocket(&sock, StatusCode(ND_CANCELED));
                }
            }

            m_pAdapter->Release();
            if (m_QueuePairsPool != nullptr)
            {
                m_QueuePairsPool->Release();
            }

            Tracer::DisposeLogging();
        }

        // Begin Listen on a certain port. automatically assigned if 0
        //
        StatusCode Listen(HSocketHandle handle, uint16_t port) override;

        // Signal that one connection can be accepted. new socket
        // of that connection is provided async by connectionCallback
        //
        StatusCode Accept(
            HSocketHandle handle,

            // hook for Async return of the connected socket
            _In_ ConnectionNotifier& connectCallback,

            // A receive call back, will be assigned to the connected
            // socket, and triggered whenever a new message received
            // on that connected socket.
            // 
            _In_ RcvNotifier& rcvCallback
        ) override;

        // Called when we have a connection request arrived, i.e.
        // completion of Listen->GetConnectionRequest(connector);
        //
        void ProcessConnReq(HSocket& connSock, HRESULT hr, ActionArena* pArena);

        // Called when async call connector->Accept is finished 
        //
        void OnConnAccepted(HSocket& connsock, HRESULT hr, ActionArena* pArena);

        // Async call to connecting to a remote end point ==> WSA connect
        // new socket of the newly established connection is provided async
        // by connectionCallback
        StatusCode Connect(
            HSocketHandle sock,

            uint32_t remoteIp,
            uint16_t remotePort,

            // hook for async notification of connection success or failure
            // socket parameter should be this socket.
            _In_ ConnectionNotifier& connectCallback,

            // A receive call back, once connected, new messages received
            // will trigger this
            // 
            _In_ RcvNotifier& rcvCallback
        ) override;

        // Called on completion of async Connect call.
        // server accepted, need to complete connect
        //
        void OnConnAck(
            // socket  on which we need to call complete connect
            HSocket& sock, 

            // return code of the Connect funciton
            HRESULT hr, 

            // memory on which the async 
            ActionArena* pArena);


        // Async send, notification of completion will be carried by 
        // sendCallback provided to Connect or Accept method
        //
        StatusCode Send(
            HSocketHandle sock,

            // place holder for the client to identify this operation
            // will be pass back to client in sendCallback
            _In_ void* sndContext,

            // Buffer must be available for the duration of the async
            // operation, until notified by sendCallback
            _In_ const gsl::span<gsl::byte> buf,

            // A send status call back, async notification of send completion
            _In_ SendCompletionNotifier& sendCallback
        ) override;

        // Put send request to circular buffer and write to remote,
        // read remote trailing edge,
        // Expect caller to ensure this function does not overlap
        // with itself.
        //
        void ProcessSendQ(uint32_t termId);

        void CloseSocket(HSocketHandle handle, StatusCode err) override;

        // Triggered by IOCP notification on the adapter file handle
        // Simply redirect to AsyncJob registered on the HOverlap obj
        // and then release the HOverlap obj
        //
        void Run(
            _In_ WorkerThread&  /*thread*/,
            intptr_t       continuationHandle,
            uint32_t       /*length*/
        ) override
        {
            auto pOv = reinterpret_cast<OVERLAPPED*>(continuationHandle);
            auto pContext = HOverlap::GetContext(pOv);
            auto hr = pContext->PReceiver->GetOverlappedResult(pOv, false);
            Audit::Assert(hr != ND_PENDING,
                L"Pending status not expected after IOCP notification!");

            (*(pContext->PNext))(hr, m_pScheduler->GetArena(pContext->PNext));

            m_overlapPool.FreeObj(pContext);
        }

        Scheduler* GetScheduler()
        {
            return m_pScheduler.get();
        }

        // Only valid for socket in connected or listening state
        // returns 0 when unknown.
        uint16_t GetHomePort(HSocketHandle handle) override
        {
            auto pSock = GetSocket(HandleToId(handle));
            if (pSock->m_handle != handle) {
                return 0;
            }

            return pSock->m_port;
        }

        // Only valid for connected sockets, return 0 when unknown.
        uint32_t GetRemoteIp(HSocketHandle handle) override
        {
            auto pSock = GetSocket(HandleToId(handle));
            if (pSock == nullptr || pSock->m_handle != handle
                || pSock->m_term == INVALID_TERM_ID) {
                return 0;
            }

            return GetTerminal(pSock->m_term)->m_rmtIpAddr;
        }

        uint16_t GetRemotePort(HSocketHandle handle) override
        {
            auto pSock = GetSocket(HandleToId(handle));
            if (pSock->m_handle != handle
                || pSock->m_term == INVALID_TERM_ID) {
                return 0;
            }

            return GetTerminal(pSock->m_term)->m_rmtPort;
        }

    };

    // Belongs to a certain terminal.
    // A continuation to close the connection and release the socket
    //
    class TerminalCloser : public ContinuationBase
    {
        HermesFixedRIO& m_root;
        uint32_t m_termId;
    public:
        TerminalCloser(
            // Thread for sequentializing all receive related actions
            Activity& act,

            HermesFixedRIO& root,

            // Terminal for the connection
            uint32_t termId
            )
            : ContinuationBase(act)
            , m_termId(termId)
            , m_root(root)
        {}

        void Cleanup()
        {
            // have not found anything needs cleanup yet.
        }

        void OnReady(
            _In_ WorkerThread&  thread,
            intptr_t       continuationHandle,
            uint32_t       length
        ) override;

    };


    // Belongs to a terminal.
    // Triggered when the poller discovered we have incoming message.
    // So we call user call back. If the user has a call back that runs
    // too long, then it would harm the speed of this single connection
    // not others.
    //
    class Conveyor : public ContinuationBase
    {
        HermesFixedRIO& m_root;
        uint32_t m_termId;
    public:
        Conveyor(
            // Thread for sequentializing all receive related actions
            Activity& act,

            HermesFixedRIO& root,

            // Terminal for the connection
            uint32_t termId
        )
            : ContinuationBase(act)
            , m_termId(termId)
            , m_root(root)
        {}

        void Cleanup()
        {
            // have not found anything needs cleanup yet.
        }

        void OnReady(
            _In_ WorkerThread&  thread,
            intptr_t       continuationHandle,
            uint32_t       length
        ) override;
    };


    // Completion handler for ND completion queue.
    // maybe we can have all connections use one completion queue.
    //
    class VerbCompletion : public AsyncJob
    {
        HermesFixedRIO& m_root;

        uint64_t m_oldTrailing;
        uint32_t m_termId;

        bool m_shutdown = false;
        
    public:
        VerbCompletion(HermesFixedRIO& root,
            uint32_t termId)
            : m_root(root)
            , m_termId(termId)
        {
            m_oldTrailing = m_root.GetTerminal(m_termId)->m_rcvEnd.TrailingEdge;
        }

        void RequestShutdown() { m_shutdown = true; }

        virtual void operator()(
            // Result code from GetOverlappedResult();
            HRESULT /*hr*/,

            // Arena on which this object is allocated.
            _In_ Schedulers::ActionArena* pArena) override;
    };

    // A frame for data layout on ring buffer
    //
    struct DFrame {
        // header
        MsgEnd* m_pStart;

        // actual data, maybe split by ring buf end
        gsl::span<gsl::byte> m_views[2];

        // footer
        MsgEnd* m_pEnd;
    };


    // Given a starting edge and size, locate the header, footer, and data
    // segments
    //
    inline void LocateInRingBuf(
        // ring buffer
        _In_ gsl::span<gsl::byte> buffer,

        // start edge and size of blob plus header footer
        uint64_t startEdge, size_t size,

        _Out_ DFrame& rec)
    {
        auto bufSize = buffer.size_bytes();
        Audit::Assert(bufSize > 0 && mbuf::IsPower2(bufSize),
            L"Size of ring buffer must be power of 2");

        auto offset = startEdge & (bufSize - 1); // mod for power of 2 value
        Audit::Assert(offset <= (bufSize - sizeof(MsgEnd)),
            L"MsgEnd splited into two");

        rec.m_pStart = reinterpret_cast<MsgEnd*>(&buffer[offset]);
        auto blobSize = size - 2 * sizeof(MsgEnd);
        offset += sizeof(MsgEnd);

        auto len1 = std::min<size_t>(blobSize, bufSize - offset);
        auto len2 = blobSize - len1;

        rec.m_views[0] = buffer.subspan(offset, len1);
        rec.m_views[1] = buffer.subspan(0, len2);

        auto endPos = (len2 > 0) ?  len2 :
            (offset + len1) & (bufSize - 1);
        rec.m_pEnd = reinterpret_cast<MsgEnd*>(&buffer[endPos]);
    }

}
