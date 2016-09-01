// FixMem.cpp a simple implemenation of a memory buffer allocator
//            using a fixed number of fixed size bufferes.
//

#pragma once

#include "stdafx.h"

#include "FMem.hpp"
#include "Utilities.hpp"

namespace mbuf
{
    // An aligned buffer allocated from heap. Not copiable
    //
    class HeapBuffer : public DisposableBuffer
    {
    private:
        uint32_t const m_blobSize;
        uint32_t m_offset;

        HeapBuffer(uint32_t size, uint32_t offset)
            : m_blobSize(size)
            , m_offset(offset)
        {}

        HeapBuffer& operator=(const HeapBuffer&) = delete;
        HeapBuffer(const HeapBuffer&) = delete;
    public:
        void Dispose() override
        {
            ::_aligned_free(PStart());
        }

        void* PStart() const override
        {
            return (char*)this - m_blobSize;
        }

        uint32_t Offset() const override { return m_offset; }
        void SetOffset(size_t v) override
        {
            Audit::Assert(v < (1ull << 32));
            m_offset = (uint32_t)v;
        }

        uint32_t Size() const override { return m_blobSize; }

        static DisposableBuffer* Allocate(uint32_t dataBlobSize, uint32_t alignment = 8, uint32_t offset = 0)
        {
            Audit::Assert(mbuf::IsPower2(alignment),
                L"Alignment must be a power of 2");

            void* buf = ::_aligned_malloc(dataBlobSize + sizeof(HeapBuffer), alignment);
            Audit::Assert(buf != nullptr,
                L"can not allocate memory from heap!");

            // stick the recycler at the end of the buffer to keep data buffer aligned
            HeapBuffer* ret = new ((char*)buf + dataBlobSize)HeapBuffer(dataBlobSize, offset);
            return ret;
        }
    };


    // Disposabuffers are allocated to carry network messages,
    // and read data from SSD. Most of the time these data are
    // small and transient. We use these buffer pools to reduce
    // memory churn, only go to heap for big ones.
    //
    // Worth it to implement a buddy system? Either allocation
    // or free would be slow.
    //
    template <uint32_t BUF_SIZE, uint32_t NUM_BUFS>
    class SmallBufferPool
    {
    private:
        static_assert((BUF_SIZE & (BUF_SIZE - 1)) == 0,
            "Buffer size must be power of 2.");

        struct SmallBuffer : public DisposableBuffer
        {
            SmallBufferPool* m_pPool = nullptr;
            uint32_t m_size = 0;
            uint32_t m_offset = 0;

            SmallBuffer& operator=(const SmallBuffer&) = delete;
            SmallBuffer(const SmallBuffer&) = delete;

            SmallBuffer() {}

            void Dispose() override
            {
                m_pPool->Release(this);
            }

            void* PStart() const override
            {
                return m_pPool->GetData(this);
            }

            uint32_t Offset() const override { return m_offset; }
            void SetOffset(size_t v) override
            {
                Audit::Assert(v < m_size);
                m_offset = (uint32_t)v;
            }

            uint32_t Size() const override { return m_size; }
        };

        SmallBuffer m_body[NUM_BUFS];
        mbuf::CircularQue<uint32_t> m_free;
        std::unique_ptr<LargePageBuffer> m_pMemory;

        void* GetData(const SmallBuffer* pBuf)
        {
            Audit::Assert(pBuf->m_size != 0,
                L"Access Released Buffer!");
            auto index = pBuf - m_body;
            Audit::Assert(index < m_free.GetCapacity(),
                L"Buffer released to wrong pool!");

            return &(m_pMemory->GetData()[index * BUF_SIZE]);
        }

        void Release(SmallBuffer* pBuf)
        {
            Audit::Assert(pBuf->m_size != 0,
                L"Double buffer release detected!");
            pBuf->m_size = 0;
            pBuf->m_offset = 0;

            auto index = pBuf - m_body;
            Audit::Assert(&m_body[index] == pBuf,
                L"Internal error in pointer arithmetic.");
            Audit::Assert(index < m_free.GetCapacity(),
                L"Buffer released to wrong pool!");
            m_free.Add((uint32_t)index);
        }

    public:
        SmallBufferPool()
            : m_free(NUM_BUFS)
        {
            size_t allocatedSize;
            uint32_t actualCount;
            m_pMemory = std::make_unique<mbuf::LargePageBuffer>
                (BUF_SIZE, NUM_BUFS, allocatedSize, actualCount);
            Audit::Assert(actualCount == NUM_BUFS,
                L"Ill formed large page size for buffer pool");

            for (uint32_t i = 0; i < NUM_BUFS; i++) {
                m_body[i].m_pPool = this;
                m_free.Add(i);
            }
        }

        uint32_t BufSizeLimit()
        {
            return BUF_SIZE;
        }

        DisposableBuffer* Allocate(uint32_t dataBlobSize, uint32_t alignment = 8, uint32_t offset = 0)
        {
            Audit::Assert(dataBlobSize > 0, L"Can not allocate empty buffer.");
            Audit::Assert(dataBlobSize <= BUF_SIZE,
                L"Allocation buffer size too big.");
            Audit::Assert(alignment <= BUF_SIZE && ((alignment & (alignment - 1)) == 0),
                L"Buffer aligenment requirement can not be met!");
            auto index = UINT32_MAX;
            Audit::Assert(m_free.Get(index), L"Small Buffer Pool empty.");
            m_body[index].m_offset = offset;
            m_body[index].m_size = dataBlobSize;
            return &m_body[index];
        }
    };


    class FixedBufManager : public BufManager
    {
        // Small buffer pools to reduce memory churn.
        SmallBufferPool<2 * SI::Ki, 1024> m_sPool;
        SmallBufferPool<8 * SI::Ki, 1024> m_mPool;
        SmallBufferPool<32 * SI::Ki, 1024> m_bPool;

    public:
        FixedBufManager() = default;

        DisposableBuffer* Allocate(
            uint32_t size,
            uint32_t alignment = 8,
            uint32_t offset = 0) override
        {
            if (size <= m_sPool.BufSizeLimit() && alignment <= m_sPool.BufSizeLimit()) {
                return m_sPool.Allocate(size, alignment, offset);
            }
            else if (size <= m_mPool.BufSizeLimit() && alignment <= m_mPool.BufSizeLimit()) {
                return m_mPool.Allocate(size, alignment, offset);
            }
            else if (size <= m_bPool.BufSizeLimit() && alignment <= m_bPool.BufSizeLimit()) {
                return m_bPool.Allocate(size, alignment, offset);
            }
            else {
                return HeapBuffer::Allocate(size, alignment, offset);
            }
        }

    };

    std::unique_ptr<BufManager> FixedBufManagerFactory()
    {
        return std::make_unique<FixedBufManager>();
    }
}