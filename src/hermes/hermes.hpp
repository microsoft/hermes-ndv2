// hermes.hpp  Defines API supported by hermes library
//

#pragma once

#include "stdafx.h"

#include <stdint.h>
#include <gsl>

#include "Audit.hpp"
#include "Mem.hpp"

namespace hermes 
{
    // Socket Handle
    using HSocketHandle = intptr_t;

    // Message size must align to 16 byte boundary
    const size_t MSG_ALIGN = 16;
    
    // Max message size supported 1023M
    const size_t MAX_MSG_SIZE = 1023 * 1024 * 1024;

    /////////////////////////////////////////////////////////////////////
    // Async call backs for network events
    //
    // One problem with this library is that we can not tolerate any
    // long running computation or blocking operations in these
    // callbacks. Doing so would cause severe perf degradation, 
    // resources depletion or even application crash. 
    //
    // We need to add enforcement in the future.
    // 

    // Called when an new connection is established or an connected
    // socket is disconnected
    //
    class ConnectionNotifier
    {
    public:
        virtual void operator() (
            // Socket that holds the new connection
            _In_ HSocketHandle pSocket,

            _In_ StatusCode status
            ) = 0;
    };

    // Called when a message is received. Must return quickly. In this
	// version, any delay in this method would stall transportation
	// and hurt performance.
    //
    class RcvNotifier
    {
    public:
        virtual void operator() (
            // Socket that holds the connection
            HSocketHandle pSocket,

            // An reclaimable buffer that contains the message
            // User must call pBuf->Dispose() to release it
            // after finished processing the message. 
            // 
            _In_ mbuf::DisposableBuffer* pBuf,

            StatusCode status
            ) = 0;
    };

    // Notify sender an async send operation completed, buffer
    // can be released
	//
    class SendCompletionNotifier
    {
    public:
        virtual void operator() (
            // Indicate result of the send operation
            StatusCode status, 

            // parameters provided to the original send operation
            _In_ void* sndContext,
            _In_ const gsl::span<gsl::byte> buf
            ) = 0;
    };

    // End callbacks
    /////////////////////////////////////////////////////////////////////

    // Entry point of the hermes library. 
    // In this version only one instance can be instantiated due to tight
    // binding with NDv2. 
    //
    class Hermes
    {
    public:
        // Each Hermes instance is bind to one IP. Currently only IPv4
        // is supported.
        //
        virtual uint32_t GetHomeIp() = 0;

        // Create a new socket ==> WSA socket 
		// Only IPv4 is supported in this version
		// Automatically binded, no "bind" function needed
        //
        virtual HSocketHandle CreateSocket(
            _Out_ StatusCode& error
        ) = 0;

        // Only valid for socket in connected or listening state
        // returns 0 when unknown.
        virtual uint16_t GetHomePort(HSocketHandle) = 0;

        // Only valid for connected sockets, return 0 when unknown.
        virtual uint32_t GetRemoteIp(HSocketHandle) = 0;
        virtual uint16_t GetRemotePort(HSocketHandle) = 0;

        // Begin Listen on a certain port.  ==> WSA listen
        // automatically assigned if 0
        //
        virtual StatusCode Listen(HSocketHandle sock, uint16_t port) = 0;

        // Signal that one connection can be accepted.  ==> WSA accept
        // new socket of the newly established connection is provided async
        // by connectionCallback
        //
        virtual StatusCode Accept(
            HSocketHandle sock,

            // hook for Async return of the connected socket
            _In_ ConnectionNotifier& connectCallback,

            // A receive call back, will be assigned to the connected
            // socket, and triggered whenever a new message received
            // on that connected socket.
            // 
            _In_ RcvNotifier& rcvCallback
        ) = 0;

        // Async call to connecting to a remote end point ==> WSA connect
        // new socket of the newly established connection is provided async
        // by connectionCallback
        virtual StatusCode Connect(
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
        ) = 0;

        // Async send, notification of completion will be carried by 
        // sendCallback provided to Connect or Accept method
        //
        virtual StatusCode Send(
            HSocketHandle sock,

            // place holder for the client to identify this operation
            // will be pass back to client in sendCallback
            _In_ void* sndContext,

            // Buffer must be available for the duration of the async
            // operation, until notified by sendCallback
            _In_ const gsl::span<gsl::byte> buf,

            // A send status call back, async notification of send completion
            _In_ SendCompletionNotifier& sendCallback
        ) = 0;

        // Close the socket. handle becomes invalid
        virtual void CloseSocket(HSocketHandle sock,
            StatusCode reason = H_ok) = 0;
    };

	// Construct the root of this library, ==> WSAStartup()
	//
    extern std::unique_ptr<Hermes>  HermesFixedRIOFactory(
            // Size of buffer allocated per connection.
            // will be split equally for sending and receiving
            //
            size_t conBufSize,

            // Number of concurrent connections. Need this now
            // since we don't have dynamic management yet.
            //
            uint32_t maxConnections
        );

}