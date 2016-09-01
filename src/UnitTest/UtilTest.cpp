#include "stdafx.h"
#include "CppUnitTest.h"

#include <random>
#include <memory>
#include <malloc.h>

#include "Utilities.hpp"


using namespace Microsoft::VisualStudio::CppUnitTestFramework;

namespace UnitTest
{
    const size_t BUF_SIZE = 1030;
	TEST_CLASS(UtilTest)
	{
	public:
		
		TEST_METHOD(CRCTest)
		{
            auto testBuf = reinterpret_cast<uint64_t*>(_aligned_malloc(BUF_SIZE * sizeof(uint64_t), 16));
            auto dstBuf = reinterpret_cast<uint64_t*>(_aligned_malloc(BUF_SIZE * sizeof(uint64_t), 16));

            auto tv = gsl::span<gsl::byte>(reinterpret_cast<gsl::byte*>(testBuf), BUF_SIZE * sizeof(uint64_t));
            auto dv = gsl::span<gsl::byte>(reinterpret_cast<gsl::byte*>(dstBuf), BUF_SIZE * sizeof(uint64_t));

            std::default_random_engine generator;
            std::uniform_int_distribution<uint64_t> distribution;

            for (int i = 0; i < BUF_SIZE; i++)
            {
                testBuf[i] = distribution(generator);
            }

            auto leftcrc = uint32_t(0);
            auto rightcrc = uint32_t(0);

            Utilities::ComputeCRC(tv, leftcrc, rightcrc);

            auto l = uint32_t(0);
            auto r = uint32_t(0);
            Utilities::ComputeCRC(dv, tv, l, r);

            Audit::Assert(leftcrc == l);
            Audit::Assert(rightcrc == r);

            for (int i = 0; i < BUF_SIZE; i++)
            {
                Audit::Assert(testBuf[i] == dstBuf[i]);
            }

            leftcrc = uint32_t(0);
            rightcrc = uint32_t(0);

            Utilities::ComputeCRC(tv, leftcrc, rightcrc);
            Audit::Assert(leftcrc == l);
            Audit::Assert(rightcrc == r);

        }

	};
}