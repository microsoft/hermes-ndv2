// Utilities
//

#pragma once

#include "stdafx.h"

#include <memory>
#include <string>
#include <atomic>
#include <cstdarg>
#include <vector>

#include <gsl>

#include "hermes.hpp"
#include "Mem.hpp"
#include "MiscWin.hpp"


namespace Utilities
{

    template <typename T>
    struct ObjectPool
    {
        std::unique_ptr<T[]> m_body;
        mbuf::CircularQue<T*> m_pool;

        ObjectPool(uint32_t size)
            : m_pool(size)
            , m_body(new T[size])
        {
            for (auto i = 0ul; i < m_pool.GetCapacity(); i++)
            {
                Audit::Assert(m_pool.Add(&m_body[i]),
                    L"Internal error, pool size mismatch.");
            }
        }

        T* Alloc()
        {
            T* ret;
            Audit::Assert(m_pool.Get(ret),
                L"Resource pool empty!");
            return ret;
        }

        void Free(T* v)
        {
            Audit::Assert(m_pool.Add(v), L"Double free overflow!");
        }
    };


    const bool SKIP_CRC = false;

    inline void ComputeCRC(_In_ const gsl::span<const gsl::byte> buf, _Inout_ uint32_t& leftcrc, _Inout_ uint32_t& rightcrc)
    {
        if (SKIP_CRC) return;

        auto size = size_t(buf.size_bytes());
        Audit::Assert((size & 15) == 0,
            L"Need size alignment to 128b boundary for crc computation");

        auto dSrc = buf.data();
        Audit::Assert((uint64_t(dSrc) & 15) == 0,
            L"Need address alignment to 128b boundary for crc computation");

        auto l = uint64_t(leftcrc);
        auto r = uint64_t(rightcrc);

        // sample one every cache line (64 bytes)
        for (size_t line = 0; line < size / 64; line ++)
        {
            auto first = reinterpret_cast<const uint64_t*>(dSrc)[0];
            auto second = reinterpret_cast<const uint64_t*>(dSrc)[1];

            l = _mm_crc32_u64(l, first);
            r = _mm_crc32_u64(r, second);

            dSrc += 64;
        }
        leftcrc = uint32_t(l);
        rightcrc = uint32_t(r);
    }

    // Compute CRC while copy memory
    inline void ComputeCRC(
        _Out_ gsl::span<gsl::byte>dst,
        _In_ const gsl::span<const gsl::byte> src,
        _Inout_ uint32_t& leftcrc, _Inout_ uint32_t& rightcrc)
    {
        if (SKIP_CRC) 
        {
            memcpy_s(dst.data(), dst.size_bytes(), src.data(), src.size_bytes());
            return;
        }

        auto bound = size_t(dst.size_bytes());
        auto size = size_t(src.size_bytes());
        Audit::Assert((size & 15) == 0,
            L"Need alignment to 128b boundary for crc computation");

        Audit::Assert(bound >= size, L"Write memory over bound");

        auto dSrc = src.data();
        Audit::Assert((uint64_t(dSrc) & 15) == 0,
            L"Need address alignment to 128b boundary for crc computation");

        auto dDst = dst.data();
        Audit::Assert((uint64_t(dDst) & 15) == 0,
            L"Need address alignment to 128b boundary for crc computation");

        auto l = uint64_t(leftcrc);
        auto r = uint64_t(rightcrc);

        // sample one crc every cache line (64 bytes)
        for (size_t line = 0; line < size / 64; line++)
        {
            auto pSrc = reinterpret_cast<const __m128i*>(dSrc);
            auto pDst = reinterpret_cast<__m128i*>(dDst);

            auto a = _mm_load_si128(pSrc);
            auto b = _mm_load_si128(pSrc+1);
            auto c = _mm_load_si128(pSrc+2);
            auto d = _mm_load_si128(pSrc+3);

            auto first = reinterpret_cast<const uint64_t*>(dSrc)[0];
            auto second = reinterpret_cast<const uint64_t*>(dSrc)[1];

            _mm_store_si128(pDst, a);
            l = _mm_crc32_u64(l, first);

            _mm_store_si128(pDst+1, b);
            r = _mm_crc32_u64(r, second);

            _mm_store_si128(pDst+2, c);
            _mm_store_si128(pDst+3, d);

            dSrc += 64;
            dDst += 64;
        }
        
        auto remain = size & 63;
        if (remain > 0)
        {
            memcpy_s(dDst, remain, dSrc, remain);
        }

        leftcrc = uint32_t(l);
        rightcrc = uint32_t(r);
    }

    extern size_t ListIpAddresses(std::vector<in_addr>& ipAddrs);

}
