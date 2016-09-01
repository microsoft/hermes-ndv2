// NetBursts.cpp : Defines the entry point for the console application.
//

#pragma once

#include "stdafx.h"

#include <utility>
#include <algorithm>
#include <vector>
#include <stack>

#include "Audit.hpp"
#include "MiscWin.hpp"
#include "Utilities.hpp"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <Ws2def.h>
#include <iphlpapi.h>

using namespace std;

namespace Utilities
{

    // Inspect the system and figure out all the IP Addresses we might be using.
    //
    size_t ListIpAddresses(vector<in_addr>& ipAddrs)
    {
        ipAddrs.clear();

        IP_ADAPTER_ADDRESSES adapterAddresses[256];
        ULONG adapterAddressesSize{ sizeof(adapterAddresses) };

        DWORD error = ::GetAdaptersAddresses(
            AF_INET,
            GAA_FLAG_SKIP_ANYCAST |
            GAA_FLAG_SKIP_MULTICAST |
            GAA_FLAG_SKIP_DNS_SERVER |
            GAA_FLAG_SKIP_FRIENDLY_NAME,
            nullptr,
            adapterAddresses,
            &adapterAddressesSize);

        if (ERROR_SUCCESS != error)
        {
            return 0;
        }

        // Iterate through all of the adapters
        for (IP_ADAPTER_ADDRESSES* adapter = adapterAddresses; nullptr != adapter; adapter = adapter->Next)
        {
            // Skip loopback and none-working adaptors 
            if (IF_TYPE_SOFTWARE_LOOPBACK == adapter->IfType || IfOperStatusUp != adapter->OperStatus)
            {
                continue;
            }

            // Parse all IPv4 and IPv6 addresses
            for (IP_ADAPTER_UNICAST_ADDRESS* unicaster = adapter->FirstUnicastAddress; nullptr != unicaster; unicaster = unicaster->Next)
            {
                auto family = unicaster->Address.lpSockaddr->sa_family;
                if (AF_INET == family)
                {
                    // IPv4
                    ipAddrs.push_back(reinterpret_cast<sockaddr_in*>(unicaster->Address.lpSockaddr)->sin_addr);
                }
                else if (AF_INET6 == family)
                {
                    // Skip all other types of addresses
                    continue;
                }
            }
        }
        return ipAddrs.size();
    }

}

namespace mbuf
{
    void EnableLargePages()
    {
        HANDLE      hToken;
        TOKEN_PRIVILEGES tp;

        if (!OpenProcessToken(GetCurrentProcess(), TOKEN_ADJUST_PRIVILEGES | TOKEN_QUERY, &hToken))
        {
            Audit::OutOfLine::Fail((StatusCode)GetLastError());
        }
        // the app needs to run at elevated privilege, with local security policy allowing memory locking.
        auto closer = Ctrl::AutoClose(hToken,
            [&] {
            if (!LookupPrivilegeValue(nullptr, TEXT("SeLockMemoryPrivilege"), &tp.Privileges[0].Luid))
            {
                Audit::OutOfLine::Fail((StatusCode)::GetLastError());
            }
            tp.PrivilegeCount = 1;
            tp.Privileges[0].Attributes = SE_PRIVILEGE_ENABLED;
            AdjustTokenPrivileges(hToken, FALSE, &tp, 0, nullptr, 0);
            // AdjustToken silently fails, so we always need to check last status.
            auto status = (StatusCode)::GetLastError();
            if (status != H_ok)
            {
                if (status == (StatusCode)ERROR_PRIVILEGE_NOT_HELD)
                    Audit::OutOfLine::Fail(L"run at elevated privilege, with local security policy allowing memory locking");
                else
                    Audit::OutOfLine::Fail((StatusCode)status);
            }
        });
    }

    /* Allocate multiple units of buffers contiguously in large pages which will stay resident
    */
    LargePageBuffer::LargePageBuffer(
        const size_t oneBufferSize,
        const unsigned bufferCount,
        __out size_t& allocatedSize,
        __out unsigned& allocatedBufferCount
    )
    {
        m_pBuffer = nullptr;
        m_allocatedSize = 0;
        const size_t granularity = ::GetLargePageMinimum();
        const size_t minimumSize = oneBufferSize * bufferCount;
        allocatedSize = (minimumSize + granularity - 1) & (0 - granularity);
        allocatedBufferCount = 0;

        m_pBuffer = ::VirtualAlloc(
            nullptr,
            allocatedSize,
            MEM_COMMIT | MEM_RESERVE | MEM_LARGE_PAGES,
            PAGE_READWRITE
        );

        if (m_pBuffer != nullptr)
        {
            m_allocatedSize = allocatedSize;
            allocatedBufferCount = (int)(allocatedSize / oneBufferSize);
        }
        else
        {
            Audit::OutOfLine::Fail((StatusCode)::GetLastError(), L"Cannot allocate large page!");
        }
    }

    LargePageBuffer::~LargePageBuffer()
    {
        if (m_pBuffer != nullptr)
        {
            ::VirtualFreeEx(GetCurrentProcess(), m_pBuffer, m_allocatedSize, 0);
            m_pBuffer = nullptr;
        }
    }
}