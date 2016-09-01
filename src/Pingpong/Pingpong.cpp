// Pingpong.cpp : Defines the entry point for the console application.
//

#include "stdafx.h"

#include <hermes.hpp>

#include <WinSock2.h>
#include <iostream>
#include <chrono>
#include <ctime>

using namespace hermes;

const uint16_t x_DefaultPort = 54325;
const uint64_t x_MaxXfer = (4 * 1024 * 1024);
const uint64_t x_HdrLen = 40;
const size_t x_MaxVolume = (500 * x_MaxXfer);
const uint64_t x_MaxIterations = 100000;
const size_t x_MaxMsgSize = 8 * 1024 * 1024; // 4MB

#define RECV    0
#define SEND    1


void ShowUsage()
{
    printf("Run as server: pingpong s <port>\n"
        "Run as client: pingpong c <ip>:<port> <message_size_in_K> <num_tests>\n"
        "\ts - start as server (listen on Port)\n"
        "\tc - start as client (connect to server IP:Port)\n"
        "\t<ip> - IPv4 Address, only needed in client\n"
        "\t<port> - Port number"
        "\t<message_size_in_K> - size of the test message, in kilo-byte"
        "\t<num_tests> - Number of round trips to run"
    );
}

std::unique_ptr<Hermes> g_pHermes;
auto g_connection = HSocketHandle(INVALID_HANDLE_VALUE);
auto g_numTests = 1'000'000ull;
auto g_MsgSize = 0ull;

std::chrono::time_point<std::chrono::high_resolution_clock> g_start, g_end;
char g_msg[x_MaxMsgSize];


// Used by echo server, reclaim the buffer used to send message
//
class OnEchoFinish : public SendCompletionNotifier
{
    void operator() (
        // Indicate result of the send operation
        StatusCode status,

        // parameters provided to the original send operation
        _In_ void* sndContext,
        _In_ const gsl::span<gsl::byte> buf
        ) override
    {
        auto pBuf = reinterpret_cast<mbuf::DisposableBuffer*>(sndContext);
        if (pBuf->GetData().data() != buf.data())
        {
            std::cerr << "OnSent parameter error!" << std::endl;
            std::terminate();
        }
        pBuf->Dispose();
        pBuf = nullptr;
    }
} g_onEchoFinish;


// Used by client
class OnSent : public SendCompletionNotifier
{
public:
    void operator() (
        // Indicate result of the send operation
        StatusCode status,

        // parameters provided to the original send operation
        _In_ void* sndContext,
        _In_ const gsl::span<gsl::byte> buf
        ) override
    {
        if (sndContext != nullptr)
        {
            std::cerr << "Unexpected context!" << std::endl;
            std::terminate();
        }
    }
} g_onSent;


class OnConnect: public ConnectionNotifier
{
public:
    void operator() (
        // Socket that holds the new connection
        _In_ HSocketHandle pSocket,

        StatusCode status
        ) override
    {
        std::cout << "Socket connected!" << std::endl;
        if (status != H_ok) {
            std::cerr << "Socket closed: " << std::hex << status << std::dec << std::endl;
            std::exit(0);
        }

        g_connection = pSocket;

    }
}  g_onConnect;


class ClientConnected : public ConnectionNotifier
{
public:
    void operator()
        (
            // Socket that holds the new connection
            _In_ HSocketHandle pSocket,

            StatusCode status
            ) override
    {
        std::cout << "Socket connected!" << std::endl;
        if (status != H_ok) {
            std::cerr << "client connection closed: " << std::hex << status << std::dec << std::endl;
            std::exit(0);
        }

        if (g_connection != pSocket)
        {
            std::cerr << "Socket failure" << std::endl;
            std::terminate();
        }

        auto cv = '!';
        for (auto& c : g_msg) {
            c = cv++;
            if (cv > '}')
                cv = '!';
        }
        g_msg[g_MsgSize - 1] = '\0';

        auto  buf = gsl::span<gsl::byte>(reinterpret_cast<gsl::byte*>(g_msg), g_MsgSize);
        g_start = std::chrono::high_resolution_clock::now();
        g_pHermes->Send(pSocket, nullptr, buf, g_onSent);
    }
} g_clientConnected;


class OnServerReceive: public RcvNotifier
{

public:
    OnServerReceive()
    {}

    void operator() (
        // Socket that holds the connection
        HSocketHandle pSocket,

        // An reclaimable buffer that contains the message
        // User must call pBuf->Dispose() to release it
        // after finished processing the message. 
        // 
        _In_ mbuf::DisposableBuffer* pBuf,

        StatusCode status
        ) override
    {
        //std::cout << "Socket Received! " << g_numTests << std::endl;
        if (status != H_ok) {
            std::cerr << "Failure in server receive: " << status << std::endl;
            std::terminate();
        }

        g_pHermes->Send(pSocket, pBuf, pBuf->GetData(), g_onEchoFinish);
        g_numTests++;
    }
} g_serverReceived;


class OnClientReceive : public RcvNotifier
{
public:
    OnClientReceive()
    { }

    void operator() (
        // Socket that holds the connection
        HSocketHandle pSocket,

        // An reclaimable buffer that contains the message
        // User must call pBuf->Dispose() to release it
        // after finished processing the message. 
        // 
        _In_ mbuf::DisposableBuffer* pBuf,

        StatusCode status
        ) override
    {
        //std::cout << "Socket Received! " << g_numTests << std::endl;
        if (status != H_ok) {
            std::cerr << "Failed to create server socket: " << status << std::endl;
            std::terminate();
        }

        g_numTests--;
        if (g_numTests > 0)
        {
            gsl::span<gsl::byte>  buf = gsl::span<gsl::byte>(reinterpret_cast<gsl::byte*>(g_msg), g_MsgSize);
            g_pHermes->Send(pSocket, nullptr, buf, g_onSent);
        }
        else {
            g_end = std::chrono::high_resolution_clock::now();
            std::chrono::duration<double> elapsed_seconds = g_end - g_start;
            std::cout << "Elapsed time: " << elapsed_seconds.count() << std::endl;
        }
        pBuf->Dispose();

    }
} g_clientReceived;



// Server side of the test, listen on a port, accept one connection, and echo
// all received messages.
//
void Server(uint16_t port)
{

    StatusCode err;
    auto serverSock = g_pHermes->CreateSocket(err);

    if (err != H_ok) {
        std::cerr << "Failed to create server socket: " << err << std::endl;
        std::terminate();
    }

    err = g_pHermes->Listen(serverSock, port);
    if (err != H_ok) {
        std::cerr << "Failed to listen on designated port" << err << std::endl;
        std::terminate();
    }

    std::cout << "Echo Server Listens on port: " << g_pHermes->GetHomePort(serverSock) << std::endl;

    err = g_pHermes->Accept(serverSock, g_onConnect, g_serverReceived);
    if (err != H_ok) {
        std::cerr << "Failed to accept new connection: " << err << std::endl;
        std::terminate();
    }

    while (true)
    {
        std::cout << "Number of tests: " << g_numTests << std::endl;
        Sleep(10000);
    }

}


// Client side of the test, connect to server, and send the first
// message, reply to messages from server.
//
void Client(uint32_t ip, uint16_t port)
{
    StatusCode err;
    g_connection = g_pHermes->CreateSocket(err);
    if (err != H_ok) {
        std::cerr << "Failed to create server socket: " << err << std::endl;
        std::terminate();
    }

    err = g_pHermes->Connect(g_connection, ip, port, g_clientConnected, g_clientReceived);
    if (err != H_ok) {
        std::cerr << "Failed to connect: " << err << std::endl;
        std::terminate();
    }

    while (g_numTests > 0)
    {
        std::cout << "Number of tests: " << g_numTests << std::endl;
        Sleep(10000);
    }

}

int __cdecl _tmain(int argc, TCHAR* argv[])
{
    g_pHermes = hermes::HermesFixedRIOFactory(size_t(512 * 1024), uint32_t(32));
    bool bServer = false;
    bool bClient = false;
    struct sockaddr_in v4Server = { 0 };
    int port = 0;

    for (int i = 1; i < argc; i++)
    {
        const TCHAR* pArg;
        int len;

        pArg = argv[i];

        // Skip leading dashes/slashes
        while (*pArg == '-' || *pArg == '/')
        {
            pArg++;
        }

        switch (*pArg)
        {
        case _T('s'):
        case _T('S'):
            bServer = true;
            if (++i == argc)
            {
                break;
            }

            port = _wtoi(argv[i]);
            if (port == 0)
            {
                port = x_DefaultPort;
            }

            break;
        case _T('c'):
        case _T('C'):
            bClient = true;
            if (++i == argc)
            {
                fprintf_s(stderr, "Missing Parameter: <ip>:<port>");
                ShowUsage();
                exit(-1);
            }

            len = sizeof(v4Server);
            WSAStringToAddress(
                argv[i],
                AF_INET,
                NULL,
                reinterpret_cast<struct sockaddr*>(&v4Server),
                &len
            );

            if (++i == argc)
            {
                fprintf_s(stderr, "Missing Parameter: <message_size_in_K>");
                ShowUsage();
                exit(-1);
            }
            g_MsgSize = _wtoi(argv[i]) * 1024;
            if (0 == g_MsgSize)
            {
                fprintf_s(stderr, "Invalid Parameter: <message_size_in_K>");
                ShowUsage();
                exit(-1);
            }
            if (g_MsgSize > x_MaxMsgSize)
            {
                fprintf_s(stderr, "Message size too big: max value 4MB.");
                ShowUsage();
                exit(-1);
            }

            if (++i == argc)
            {
                fprintf_s(stderr, "Missing parameter: <num_tests>!");
                ShowUsage();
                exit(-1);
            }
            g_numTests = _wtoi(argv[i]);
            break;

        default:
            _tprintf(_T("Unknown parameter %s\n"), pArg);
            ShowUsage();
            exit(__LINE__);
        }
    }
    if ((bClient && bServer) || (!bClient && !bServer))
    {
        printf("Exactly one of client (c or "
            "server (s) must be specified.\n\n");
        ShowUsage();
        exit(__LINE__);
    }

    if (bServer)
    {
        g_numTests = 0;
        Server(port);
    }
    else
    {
        if (v4Server.sin_addr.S_un.S_addr == 0)
        {
            ShowUsage();
            exit(-1);
        }
        if (v4Server.sin_port == 0)
        {
            v4Server.sin_port = htons(x_DefaultPort);
        }
        Client(ntohl(v4Server.sin_addr.S_un.S_addr), ntohs(v4Server.sin_port));
    }

    return 0;
}

