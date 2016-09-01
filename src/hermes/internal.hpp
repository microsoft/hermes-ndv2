// internal.hpp
// Define internal data structure of hermes library

#pragma once
#include "stdafx.h"

#include <stdint.h>
#include <gsl>

#include "hermes.hpp"
#include "Utilities.hpp"
#include "Scheduler.hpp"

#include "ndsupport.h"

namespace hinternal
{
    // Note: I am not complete sure about these const values due to limited
    // knowledge. Need to adjust later

    // Max completion queue depth
    const uint32_t MAX_OVERLAPPED_ENTRIES = 256;

    const uint32_t MAX_SGE = 32;

    const uint32_t QP_POOL_SIZE = 128;

    // A Terminal is the data structure for a connection.
    // We keep an terminal table and a socket table. as not all socket are
    // connected and need a terminal.  Both indexes start from 1
    //
    const uint32_t INVALID_TERM_ID = 0;
    const uint32_t INVALID_SOCK_ID = 0;

	// Functor to process async notifications.
    // Since NDv2 providers would skip IOCP on success, we need to process
    // the completion both on sync return and async completion. 
    // 
    // !!! Must be allocated on an Arena!!!
    // Async -> always triggered by HermesFixedRIO::Run 
    // Sync -> must be called by pass async opertion result code, and 
    // the arena it is allocated on.
	//
    struct AsyncJob
    {
        // Actual logic for processing the async result. Sub class defining
        // this method must either reclaim pArena, or pass it to other
        // AsyncJob.
        //
        virtual void operator()(
            // Result code from GetOverlappedResult();
            HRESULT hr, 

            // Caller must provide the Arena on which this 
            // object is allocated.
            _In_ Schedulers::ActionArena* pArena) = 0;
    };

    template<class Func>
    class AsyncLambda : public AsyncJob
    {
        Func f_;
    public:
        AsyncLambda(Func f)
            : f_(std::move(f))
        {}

        ~AsyncLambda() {}

        void operator()(
            // Result code from GetOverlappedResult();
            HRESULT hr, 

            // Arena on which this object is allocated.
            _In_ Schedulers::ActionArena* pArena) override
        {
            f_(hr, pArena);
        }
    };

    template<class Func>
    AsyncJob* asyncLambda(
		// lambda function that define the async processing logic
		Func f,

		// memory arena on which this object is allocated
		Schedulers::ActionArena* pArena
	)
    {
        return pArena->allocate<AsyncLambda<Func>>(std::move(f));
    }


    // Attach pointers to async continuation at OVERLAPPED so that we
    // know where to go after an async notification.
    //
    struct HOverlap
    {
        OVERLAPPED OV;
        IND2Overlapped* PReceiver;
        AsyncJob *PNext;

        HOverlap()
        {
            std::memset(this, 0, sizeof(HOverlap));
        }

        static HOverlap* GetContext(OVERLAPPED* pOv)
        {
            return reinterpret_cast<HOverlap*>
                (reinterpret_cast<char*>(pOv) - offsetof(HOverlap, OV));
        }
    };

    struct HOverlapPool
    {
        Utilities::ObjectPool<HOverlap> m_pool;

        HOverlapPool(uint32_t size)
            : m_pool(size)
        {}

        HOverlap* GetObj(AsyncJob* pjob, IND2Overlapped* pReceiver)
        {
            auto ret = m_pool.Alloc();
            Audit::Assert(ret->PNext == nullptr
                && ret->PReceiver == nullptr,
                L"Object stat changed while in pool!");
            ret->PNext = pjob;
            ret->PReceiver = pReceiver;
            return ret;
        }

        void FreeObj(HOverlap* v)
        {
            Audit::Assert(v->PNext != nullptr
                || v->PReceiver != nullptr,
                L"Double free!");
            new(v) HOverlap();
            m_pool.Free(v);
        }
    };


    // Send request
    struct SndReq {
        void* m_context;
        gsl::span<gsl::byte> m_buf;
        hermes::SendCompletionNotifier* m_pCallback;
    };

    extern IND2QueuePairsPool* GetQuePairPoolInterface(IND2Adapter* pAdapter, HANDLE adapterFile);

    // Initialize completion que, que pair, and connector for an connection
    //
    extern StatusCode InitQues(
        // Optional que pair pool
        _In_ IND2QueuePairsPool* qpPool,

        // NDv2 Adpter
        _In_ IND2Adapter* pAdapter,

        // NDv2 adptor file handle
        _In_ HANDLE adapterFile,

        _Out_ IND2QueuePair*& pQuePair,
        _Out_ IND2CompletionQueue*&  pCQ,
        _Out_ IND2Connector*& pConnector
    );

}