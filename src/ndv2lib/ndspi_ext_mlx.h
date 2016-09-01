/*++
// Copyright (C) Mellanox Technologies, Ltd. 2001-2015. ALL RIGHTS RESERVED.

Module Name:

    ndspi_ext_mlx.h

Abstract:

    NetworkDirect Service Provider Interfaces
    Mellanox specific extensions

Environment:

    User mode

--*/

#pragma once

#include "ndspi.h"

//
// IND2QueuePairsPool object
//
#undef INTERFACE
#define INTERFACE IND2QueuePairsPool

// {5FF5992A-B671-4976-A04D-FBE07776D1BE}
DEFINE_GUID(IID_IND2QueuePairsPool,
0x5ff5992a, 0xb671, 0x4976, 0xa0, 0x4d, 0xfb, 0xe0, 0x77, 0x76, 0xd1, 0xbe);

typedef struct _ND2_QUEUE_PAIRS_POOL_STATS {
    // Input  - capacity of struct
    // Output - sizeof struct for which members got filled (rest will be zeroed)
    ULONG    cbSizeOfStruct;

    // number of times that the pool was empty when CreateObjects was called
    ULONG64  cEmptyPool;

    // number of times that the pool was filled using background thread
    ULONG64  cImplicitFill;

    // number of times that CreateObjects was called
    ULONG64  cCreateCalls;

} ND2_QUEUE_PAIRS_POOL_STATS;

//
// IND2QueuePairsPool - Mellanox specific extension
// acquire using IND2Adapter::QueryInterface w/ IID_IND2QueuePairsPool
//
DECLARE_INTERFACE_(IND2QueuePairsPool, IUnknown)
{
    // *** IUnknown methods ***
    IFACEMETHOD(QueryInterface)(
        THIS_
        REFIID riid,
        __deref_out LPVOID* ppvObj
        ) PURE;

    IFACEMETHOD_(ULONG,AddRef)(
        THIS
        ) PURE;

    IFACEMETHOD_(ULONG,Release)(
        THIS
        ) PURE;

    //
    // GetSize
    // returns the current number of items in the pool
    // each item is an QueuePair with attached send/recv completion-queue
    //
    STDMETHOD(GetSize)(
        THIS_
        __out ULONG* pPoolSize
        ) PURE;

    //
    // QueryStats
    //
    STDMETHOD(QueryStats)(
        THIS_
        __inout ND2_QUEUE_PAIRS_POOL_STATS* pStats
        ) PURE;

    //
    // Must be called once before CreateObjects
    // note: call Fill() to force immediate pre allocation
    //
    STDMETHOD(SetLimits)(
        THIS_
        ULONG nMinPoolSize,
        ULONG nMaxPoolSize
        ) PURE;

    //
    // Must be called once before CreateObjects
    // see MSDN IND2Adapter::CreateQueuePair for parameters info
    //
    STDMETHOD(SetQueuePairParams)(
        THIS_
        VOID* context,
        ULONG receiveQueueDepth,
        ULONG initiatorQueueDepth,
        ULONG maxReceiveRequestSge,
        ULONG maxInitiatorRequestSge,
        ULONG inlineDataSize
        ) PURE;

    //
    // Must be called once before CreateObjects
    // see MSDN IND2Adapter::CreateCompletionQueue for parameters info
    //
    STDMETHOD(SetCompletionQueueParams)(
        THIS_
        HANDLE    hOverlappedFile,
        ULONG     queueDepth,
        USHORT    group,
        KAFFINITY affinity
        ) PURE;

    //
    // Call to assure that pool has at least nPoolSize items
    // after this call returns if bAsync == FALSE
    //
    STDMETHOD(Fill)(
        THIS_
        ULONG nPoolSize,
        bool  bAsync
        ) PURE;

    //
    // CreateObjects
    //
    STDMETHOD(CreateObjects)(
        THIS_
        __deref_out IND2QueuePair**       ppQp,
        __deref_out IND2CompletionQueue** ppCq
        ) PURE;

};

#undef INTERFACE

