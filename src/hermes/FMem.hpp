#pragma once

#include "stdafx.h"

#include "Mem.hpp"


namespace mbuf
{
	
    class BufManager
    {
    public:
        virtual DisposableBuffer* Allocate(
            uint32_t size,
            uint32_t alignment = 8,
            uint32_t offset = 0) = 0;
    };

    extern std::unique_ptr<BufManager> FixedBufManagerFactory();
}