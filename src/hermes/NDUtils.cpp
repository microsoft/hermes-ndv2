// NDUtiles.cpp : misc functions that interact directly with NDv2
//
//

#pragma once
#include "stdafx.h"

#include "Utilities.hpp"
#include "Tracer.hpp"
#include "internal.hpp"

using namespace Utilities;
using namespace hermes;

namespace hinternal
{
    // Some adaptors support queue pairs pool, try get that interface, optional
    // Return null if we can not find that interface
    //
    IND2QueuePairsPool* GetQuePairPoolInterface(IND2Adapter* pAdapter, HANDLE adapterFile)
    {
        IND2QueuePairsPool* pQuePairPool = nullptr;

        auto hr = pAdapter->QueryInterface(IID_IND2QueuePairsPool,
            (LPVOID *)&pQuePairPool);
        if (FAILED(hr))
        {
            Tracer::LogWarning(StatusCode(::GetLastError()),
                L"INDAdapter::QueryInterface for IID_IND2QueuePairsPool failed");
            return nullptr;
        }
        // Init pool
        hr = pQuePairPool->SetLimits((QP_POOL_SIZE >> 2) * 3,
            (QP_POOL_SIZE >> 2) * 5);
        if (FAILED(hr)) {
            pQuePairPool->Release();
            pQuePairPool = nullptr;
            NdCleanup();
            Audit::OutOfLine::Fail(StatusCode(hr),
                L"Queue pairs pool set limit failed.");
        }

        hr = pQuePairPool->SetQueuePairParams(nullptr,
            MAX_OVERLAPPED_ENTRIES,
            MAX_OVERLAPPED_ENTRIES,
            MAX_SGE,
            MAX_SGE,
            0);
        if (FAILED(hr))
        {
            pQuePairPool->Release();
            pQuePairPool = nullptr;
            NdCleanup();
            Audit::OutOfLine::Fail(StatusCode(hr),
                L"Queue pairs pool set parameter failed.");
        }

        hr = pQuePairPool->SetCompletionQueueParams(adapterFile,
            MAX_OVERLAPPED_ENTRIES << 1,
            0,
            0);
        if (FAILED(hr))
        {
            pQuePairPool->Release();
            pQuePairPool = nullptr;
            NdCleanup();
            Audit::OutOfLine::Fail(StatusCode(hr),
                L"Queue pairs pool set completion queue parameter failed.");
        }

        hr = pQuePairPool->Fill(QP_POOL_SIZE, true);
        if (FAILED(hr))
        {
            pQuePairPool->Release();
            pQuePairPool = nullptr;
            NdCleanup();
            Audit::OutOfLine::Fail(StatusCode(hr),
                L"Queue pairs pool Fill failed.");
        }

        return pQuePairPool;
    }

    // Initialize completion que, que pair, and connector for an connection
    //
    StatusCode InitQues(
        // Optional que pair pool
        _In_ IND2QueuePairsPool* qpPool,

        // NDv2 Adpter
        _In_ IND2Adapter* pAdapter, 
    
        // NDv2 adptor file handle
        _In_ HANDLE adapterFile,

        _Out_ IND2QueuePair*& pQuePair,
        _Out_ IND2CompletionQueue*&  pCQ,
        _Out_ IND2Connector*& pConnector
    )
    {
        HRESULT hr = E_UNEXPECTED;
        auto catchAll = Ctrl::scopeGuard([&] {
            Tracer::LogError(StatusCode(hr),
                L"Failed to prepare queues for connection.");
        });

        if (nullptr != qpPool)
        {
            hr = qpPool->CreateObjects(&pQuePair, &pCQ);
            if (FAILED(hr))
            {
                return StatusCode(hr);
            }
        }
        else
        {
            hr = pAdapter->CreateCompletionQueue(IID_IND2CompletionQueue,
                adapterFile,
                MAX_OVERLAPPED_ENTRIES << 1,
                0,
                0,
                reinterpret_cast<void**>(&pCQ));

            if (FAILED(hr))
            {
                return StatusCode(hr);
            }

            hr = pAdapter->CreateQueuePair(IID_IND2QueuePair,
                pCQ,
                pCQ,
                nullptr,
                MAX_OVERLAPPED_ENTRIES,
                MAX_OVERLAPPED_ENTRIES,
                MAX_SGE,
                MAX_SGE,
                0,
                reinterpret_cast<void**>(&pQuePair));
            if (FAILED(hr))
            {
                return StatusCode(hr);
            }
        }

        hr = pAdapter->CreateConnector(IID_IND2Connector,
            adapterFile,
            reinterpret_cast<void**>(&pConnector));
        if (!FAILED(hr))
        {
            catchAll.dismiss();
        }
        return StatusCode(hr);
    }

}