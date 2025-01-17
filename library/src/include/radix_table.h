/******************************************************************************
* Copyright (c) 2016 - present Advanced Micro Devices, Inc. All rights reserved.
*
* Permission is hereby granted, free of charge, to any person obtaining a copy
* of this software and associated documentation files (the "Software"), to deal
* in the Software without restriction, including without limitation the rights
* to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
* copies of the Software, and to permit persons to whom the Software is
* furnished to do so, subject to the following conditions:
*
* The above copyright notice and this permission notice shall be included in
* all copies or substantial portions of the Software.
*
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
* IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
* FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.  IN NO EVENT SHALL THE
* AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
* LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
* OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
* THE SOFTWARE.
*******************************************************************************/

#pragma once
#if !defined(RADIX_TABLE_H)
#define RADIX_TABLE_H

#include <assert.h>
#include <functional>
#include <iostream>
#include <map>
#include <vector>

#include "rocfft.h"

// Returns 1 for single-precision, 2 for double precision
inline size_t PrecisionWidth(rocfft_precision precision)
{
    switch(precision)
    {
    case rocfft_precision_single:
        return 1;
    case rocfft_precision_double:
        return 2;
    default:
        assert(false);
        return 1;
    }
}

inline size_t Large1DThreshold(rocfft_precision precision)
{
    return 4096 / PrecisionWidth(precision);
}

#define MAX_WORK_GROUP_SIZE 1024

/* radix table: tell the FFT algorithms for size <= 4096 ; required by twiddle,
 * passes, and kernel*/
struct SpecRecord
{
    size_t length;
    size_t workGroupSize;
    size_t numTransforms;
    size_t numPasses;
    size_t radices[12]; // Setting upper limit of number of passes to 12
};

inline const std::vector<SpecRecord>& GetRecord()
{
    // clang-format off
    static const std::vector<SpecRecord> specRecord = {
        //  Length, WorkGroupSize (thread block size), NumTransforms , NumPasses,
        //  Radices
        //  vector<size_t> radices; NUmPasses = radices.size();
        //  Tuned for single precision on OpenCL stack; double precsion use the
        //  same table as single
        {4096, 256,  1, 3, {16, 16, 16, 0, 0, 0, 0, 0, 0, 0, 0, 0}}, // pow2
        {2048, 256,  1, 4, { 8,  8,  8, 4, 0, 0, 0, 0, 0, 0, 0, 0}},
        {1024, 128,  1, 4, { 8,  8,  4, 4, 0, 0, 0, 0, 0, 0, 0, 0}},
        { 512,  64,  1, 3, { 8,  8,  8, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        { 256,  64,  1, 4, { 4,  4,  4, 4, 0, 0, 0, 0, 0, 0, 0, 0}},
        { 128,  64,  4, 3, { 8,  4,  4, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {  64,  64,  4, 3, { 4,  4,  4, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {  32,  64, 16, 2, { 8,  4,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {  16,  64, 16, 2, { 4,  4,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {   8,  64, 32, 2, { 4,  2,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {   4,  64, 32, 2, { 2,  2,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
        {   2,  64, 64, 1, { 2,  0,  0, 0, 0, 0, 0, 0, 0, 0, 0, 0}},
    };
    // clang-format on

    return specRecord;
}

/* blockCompute table: used for large 1D kernels of size >= 8192 */
inline void GetBlockComputeTable(size_t N, size_t& bwd, size_t& wgs, size_t& lds)
{
    switch(N)
    {
    case 256:
        bwd = 8;
        wgs = 256;
        break;
    case 128:
        bwd = 8;
        wgs = 128;
        break;
    case 64:
        bwd = 16;
        wgs = 128;
        break;
    case 32:
        bwd = 32;
        wgs = 64;
        break;
    case 16:
        bwd = 64;
        wgs = 64;
        break;
    case 8:
        bwd = 128;
        wgs = 64;
        break;
    case 81:
        bwd = 9;
        wgs = 81;
        break;
    case 50:
        bwd = 10;
        wgs = 50;
        break;
    case 100:
        bwd = 5;
        wgs = 50;
        break;
    case 200:
        bwd = 10;
        wgs = 100;
        break;
    default:
        assert(0); // should not happen
    }
    lds = N * bwd;
}

/* =====================================================================
   Calculate grid and thread blocks (work groups, work items)
       in kernel generator if no predefined table
   input: MAX_WORK_GROUP_SIZE, length
   output: workGroupSize, numTrans,
=================================================================== */

inline void DetermineSizes(const size_t& length, size_t& workGroupSize, size_t& numTrans)
{
    assert(MAX_WORK_GROUP_SIZE >= 64);

    if(length == 1) // special case
    {
        workGroupSize = 64;
        numTrans      = 64;
        return;
    }

    size_t baseRadix[]   = {13, 11, 7, 5, 3, 2}; // list only supported primes
    size_t baseRadixSize = sizeof(baseRadix) / sizeof(baseRadix[0]);

    size_t                   l = length;
    std::map<size_t, size_t> primeFactorsExpanded;
    for(size_t r = 0; r < baseRadixSize; r++)
    {
        size_t rad = baseRadix[r];
        size_t e   = 1;
        while(!(l % rad))
        {
            l /= rad;
            e *= rad;
        }

        primeFactorsExpanded[rad] = e;
    }

    assert(l == 1); // Makes sure the number is composed of only supported primes

    if(primeFactorsExpanded[2] == length) // Length is pure power of 2
    {
        if(length >= 1024)
        {
            workGroupSize = (MAX_WORK_GROUP_SIZE >= 256) ? 256 : MAX_WORK_GROUP_SIZE;
            numTrans      = 1;
        }
        else if(length == 512)
        {
            workGroupSize = 64;
            numTrans      = 1;
        }
        else if(length >= 16)
        {
            workGroupSize = 64;
            numTrans      = 256 / length;
        }
        else
        {
            workGroupSize = 64;
            numTrans      = 128 / length;
        }
    }
    else if(primeFactorsExpanded[3] == length) // Length is pure power of 3
    {
        workGroupSize = (MAX_WORK_GROUP_SIZE >= 256) ? 243 : 27;
        numTrans      = length >= 3 * workGroupSize ? 1 : (3 * workGroupSize) / length;
    }
    else if(primeFactorsExpanded[5] == length) // Length is pure power of 5
    {
        workGroupSize = (MAX_WORK_GROUP_SIZE >= 128) ? 125 : 25;
        numTrans      = length >= 5 * workGroupSize ? 1 : (5 * workGroupSize) / length;
    }
    else if(primeFactorsExpanded[7] == length) // Length is pure power of 7
    {
        workGroupSize = 49;
        numTrans      = length >= 7 * workGroupSize ? 1 : (7 * workGroupSize) / length;
    }
    else if(primeFactorsExpanded[11] == length) // Length is pure power of 11
    {
        workGroupSize = 121;
        numTrans      = length >= 11 * workGroupSize ? 1 : (11 * workGroupSize) / length;
    }
    else if(primeFactorsExpanded[13] == length) // Length is pure power of 13
    {
        workGroupSize = 169;
        numTrans      = length >= 13 * workGroupSize ? 1 : (13 * workGroupSize) / length;
    }
    else
    {
        size_t leastNumPerWI    = 1; // least number of elements in one work item
        size_t maxWorkGroupSize = MAX_WORK_GROUP_SIZE; // maximum work group size desired

        if(primeFactorsExpanded[2] * primeFactorsExpanded[3] == length)
        {
            if(length % 12 == 0)
            {
                leastNumPerWI    = 12;
                maxWorkGroupSize = 128;
            }
            else
            {
                leastNumPerWI    = 6;
                maxWorkGroupSize = 256;
            }
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[5] == length)
        {
            // NB:
            //   We found the below config leastNumPerWI 10 and maxWorkGroupSize
            //   128 works well for 1D cases 100 or 10000. But for single
            //   precision, 20/64 config is still better(>=) for most of the
            //   cases, especially for cases like 200, 800 with outplace large
            //   batch run.
            if((length % 20 == 0) && (length != 100))
            {
                leastNumPerWI    = 20;
                maxWorkGroupSize = 64;
            }
            else
            {
                leastNumPerWI    = 10;
                maxWorkGroupSize = 128;
            }
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[7] == length)
        {
            leastNumPerWI    = 14;
            maxWorkGroupSize = 64;
        }
        else if(primeFactorsExpanded[3] * primeFactorsExpanded[5] == length)
        {
            leastNumPerWI    = 15;
            maxWorkGroupSize = 128;
        }
        else if(primeFactorsExpanded[3] * primeFactorsExpanded[7] == length)
        {
            leastNumPerWI    = 21;
            maxWorkGroupSize = 128;
        }
        else if(primeFactorsExpanded[5] * primeFactorsExpanded[7] == length)
        {
            leastNumPerWI    = 35;
            maxWorkGroupSize = 64;
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[3] * primeFactorsExpanded[5]
                == length)
        {
            leastNumPerWI    = 30;
            maxWorkGroupSize = 64;
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[3] * primeFactorsExpanded[7]
                == length)
        {
            leastNumPerWI    = 42;
            maxWorkGroupSize = 60;
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[5] * primeFactorsExpanded[7]
                == length)
        {
            leastNumPerWI    = 70;
            maxWorkGroupSize = 36;
        }
        else if(primeFactorsExpanded[3] * primeFactorsExpanded[5] * primeFactorsExpanded[7]
                == length)
        {
            leastNumPerWI    = 105;
            maxWorkGroupSize = 24;
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[11] == length)
        {
            leastNumPerWI    = 22;
            maxWorkGroupSize = 128;
        }
        else if(primeFactorsExpanded[2] * primeFactorsExpanded[13] == length)
        {
            leastNumPerWI    = 26;
            maxWorkGroupSize = 128;
        }
        else
        {
            leastNumPerWI    = 210;
            maxWorkGroupSize = 12;
        }

        if(maxWorkGroupSize > MAX_WORK_GROUP_SIZE)
            maxWorkGroupSize = MAX_WORK_GROUP_SIZE;
        assert(leastNumPerWI > 0 && length % leastNumPerWI == 0);

        for(size_t lnpi = leastNumPerWI; lnpi <= length; lnpi += leastNumPerWI)
        {
            if(length % lnpi != 0)
                continue;

            if(length / lnpi <= MAX_WORK_GROUP_SIZE)
            {
                leastNumPerWI = lnpi;
                break;
            }
        }

        numTrans      = maxWorkGroupSize / (length / leastNumPerWI);
        numTrans      = numTrans < 1 ? 1 : numTrans;
        workGroupSize = numTrans * (length / leastNumPerWI);
    }

    assert(workGroupSize <= MAX_WORK_GROUP_SIZE);
}

std::vector<size_t> GetRadices(size_t length);
void                GetWGSAndNT(size_t length, size_t& workGroupSize, size_t& numTransforms);

// Get the number of threads required for a 2D_SINGLE kernel
static size_t Get2DSingleThreadCount(size_t                                        length0,
                                     size_t                                        length1,
                                     std::function<void(size_t, size_t&, size_t&)> _GetWGSAndNT)
{
    size_t workGroupSize0;
    size_t numTransforms0;
    _GetWGSAndNT(length0, workGroupSize0, numTransforms0);
    size_t workGroupSize1;
    size_t numTransforms1;
    _GetWGSAndNT(length1, workGroupSize1, numTransforms1);

    size_t cnPerWI0 = (numTransforms0 * length0) / workGroupSize0;
    size_t cnPerWI1 = (numTransforms1 * length1) / workGroupSize1;

    size_t complexNumsPerTransform = length0 * length1;
    size_t numThreads0             = complexNumsPerTransform / cnPerWI0;
    size_t numThreads1             = complexNumsPerTransform / cnPerWI1;
    return std::max(numThreads0, numThreads1);
}

static void Add2DSingleSize(size_t                                        i,
                            size_t                                        j,
                            size_t                                        realSizeBytes,
                            size_t                                        elementSizeBytes,
                            size_t                                        ldsSizeBytes,
                            std::function<void(size_t, size_t&, size_t&)> _GetWGSAndNT,
                            std::vector<std::pair<size_t, size_t>>&       retval)
{
    // Make sure the LDS storage needed fits in the total LDS
    // available.
    //
    // 1.5x the space needs to be allocated - we currently need
    // to store both the semi-transformed data as well as
    // separate butterfly temp space (which works out to the
    // same size, but in reals)
    if((i * j * elementSizeBytes) + (i * j * realSizeBytes) <= ldsSizeBytes)
        // Also make sure we're not launching too many threads,
        // since each transform is done by a single workgroup
        if(Get2DSingleThreadCount(i, j, _GetWGSAndNT) < MAX_WORK_GROUP_SIZE)
            retval.push_back(std::make_pair(i, j));
}

// Available sizes for 2D single kernels, for a given size of LDS.
//
// Specify 0 for LDS size to assume maximum size in current hardware
// - this is meant to be used at compile time to decide what 2D
// single kernels to generate code for.
//
// At runtime you would pass the actual LDS size for the device.
static std::vector<std::pair<size_t, size_t>>
    Single2DSizes(size_t                                        ldsSizeBytes,
                  rocfft_precision                              precision,
                  std::function<void(size_t, size_t&, size_t&)> _GetWGSAndNT)
{
    std::vector<std::pair<size_t, size_t>> retval;
    // maximum amount of LDS we assume can exist, to put a limit on
    // what functions to generate
    static const size_t MAX_LDS_SIZE_BYTES = 64 * 1024;
    if(ldsSizeBytes == 0)
        ldsSizeBytes = MAX_LDS_SIZE_BYTES;
    else
        ldsSizeBytes = std::min(ldsSizeBytes, MAX_LDS_SIZE_BYTES);

    // size of each real
    size_t realSizeBytes = precision == rocfft_precision_single ? sizeof(float) : sizeof(double);
    // assume each element is complex, since that's what we need to
    // store temporarily during the transform
    size_t elementSizeBytes = 2 * realSizeBytes;

    // compute power-of-2 sizes

    // arbitrarily chosen max size that we want to generate code for
    static const size_t MAX_2D_POW2 = 512;
    static const size_t MIN_2D_POW2 = 4;
    for(size_t i = MAX_2D_POW2; i >= MIN_2D_POW2; i /= 2)
    {
        for(size_t j = MAX_2D_POW2; j >= MIN_2D_POW2; j /= 2)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }

    // compute power-of-3 sizes
    static const size_t MAX_2D_POW3 = 729;
    static const size_t MIN_2D_POW3 = 9;
    for(size_t i = MAX_2D_POW3; i >= MIN_2D_POW3; i /= 3)
    {
        for(size_t j = MAX_2D_POW3; j >= MIN_2D_POW3; j /= 3)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }

    // compute power-of-5 sizes
    static const size_t MAX_2D_POW5 = 625;
    static const size_t MIN_2D_POW5 = 25;
    for(size_t i = MAX_2D_POW5; i >= MIN_2D_POW5; i /= 5)
    {
        for(size_t j = MAX_2D_POW5; j >= MIN_2D_POW5; j /= 5)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }

    // mixed pow2-pow3 sizes
    for(size_t i = MAX_2D_POW2; i >= MIN_2D_POW2; i /= 2)
    {
        for(size_t j = MAX_2D_POW3; j >= MIN_2D_POW3; j /= 3)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }
    for(size_t i = MAX_2D_POW3; i >= MIN_2D_POW3; i /= 3)
    {
        for(size_t j = MAX_2D_POW2; j >= MIN_2D_POW2; j /= 2)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }

    // mixed pow3-pow5 sizes
    for(size_t i = MAX_2D_POW3; i >= MIN_2D_POW3; i /= 3)
    {
        for(size_t j = MAX_2D_POW5; j >= MIN_2D_POW5; j /= 5)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }
    for(size_t i = MAX_2D_POW5; i >= MIN_2D_POW5; i /= 5)
    {
        for(size_t j = MAX_2D_POW3; j >= MIN_2D_POW3; j /= 3)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }

    // mixed pow2-pow5 sizes
    for(size_t i = MAX_2D_POW2; i >= MIN_2D_POW2; i /= 2)
    {
        for(size_t j = MAX_2D_POW5; j >= MIN_2D_POW5; j /= 5)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }
    for(size_t i = MAX_2D_POW5; i >= MIN_2D_POW5; i /= 5)
    {
        for(size_t j = MAX_2D_POW2; j >= MIN_2D_POW2; j /= 2)
            Add2DSingleSize(
                i, j, realSizeBytes, elementSizeBytes, ldsSizeBytes, _GetWGSAndNT, retval);
    }
    return retval;
}

#endif // defined( RADIX_TABLE_H )
