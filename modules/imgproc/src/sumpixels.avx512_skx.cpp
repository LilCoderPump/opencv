// This file is part of OpenCV project.
// It is subject to the license terms in the LICENSE file found in the top-level directory
// of this distribution and at http://opencv.org/license.html.
//
// Copyright (C) 2019, Intel Corporation, all rights reserved.
#include "precomp.hpp"
#include "sumpixels.hpp"

namespace cv {
namespace { // Anonymous namespace to avoid exposing the implementation classes

//
// NOTE: Look at the bottom of the file for the entry-point function for external callers
//

// TODO: Add support for 1 channel input (WIP: currently hitting hardware glassjaw)
template<size_t num_channels> class IntegralCalculator;

template<size_t num_channels>
class IntegralCalculator  {
public:
    IntegralCalculator() {};


    void calculate_integral_avx512(const uchar *src, size_t _srcstep,
                                   double *sum,      size_t _sumstep,
                                   double *sqsum,    size_t _sqsumstep,
                                   int width, int height)
    {
        const int srcstep = (int)(_srcstep/sizeof(uchar));
        const int sumstep = (int)(_sumstep/sizeof(double));
        const int sqsumstep = (int)(_sqsumstep/sizeof(double));
        const int ops_per_line = width * num_channels;

        // Clear the first line of the sum as per spec (see integral documentation)
        // Also adjust the index of sum and sqsum to be at the real 0th element
        // and not point to the border pixel so it stays in sync with the src pointer
        memset( sum, 0, (ops_per_line+num_channels)*sizeof(double));
        sum += num_channels;

        if (sqsum) {
            memset( sqsum, 0, (ops_per_line+num_channels)*sizeof(double));
            sqsum += num_channels;
        }

        // Now calculate the integral over the whole image one line at a time
        for(int y = 0; y < height; y++) {
            const uchar * src_line    = &src[y*srcstep];
            double      * sum_above   = &sum[y*sumstep];
            double      * sum_line    = &sum_above[sumstep];
            double      * sqsum_above = (sqsum) ? &sqsum[y*sqsumstep]     : NULL;
            double      * sqsum_line  = (sqsum) ? &sqsum_above[sqsumstep] : NULL;

            calculate_integral_for_line(src_line, sum_line, sum_above, sqsum_line, sqsum_above, ops_per_line);

        }
    }

    static CV_ALWAYS_INLINE
    void calculate_integral_for_line(const uchar *srcs,
                                     double *sums, double *sums_above,
                                     double *sqsums, double *sqsums_above,
                                     int num_ops_in_line)
    {
        __m512i sum_accumulator   = _mm512_setzero_si512();  // holds rolling sums for the line
        __m512i sqsum_accumulator = _mm512_setzero_si512();  // holds rolling sqsums for the line

        // The first element on each line must be zeroes as per spec (see integral documentation)
        zero_out_border_pixel(sums, sqsums);

        // Do all 64 byte chunk operations then do the last bits that don't fit in a 64 byte chunk
        aligned_integral(     srcs, sums, sums_above, sqsums, sqsums_above, sum_accumulator, sqsum_accumulator, num_ops_in_line);
        post_aligned_integral(srcs, sums, sums_above, sqsums, sqsums_above, sum_accumulator, sqsum_accumulator, num_ops_in_line);

    }


    static CV_ALWAYS_INLINE
    void zero_out_border_pixel(double *sums, double *sqsums)
    {
        // Note the negative index is because the sums/sqsums pointers point to the first real pixel
        // after the border pixel so we have to look backwards
        _mm512_mask_storeu_epi64(&sums[-num_channels], (1<<num_channels)-1, _mm512_setzero_si512());
        if (sqsums)
            _mm512_mask_storeu_epi64(&sqsums[-num_channels], (1<<num_channels)-1, _mm512_setzero_si512());
    }


    static CV_ALWAYS_INLINE
    void aligned_integral(const uchar *&srcs,
                          double *&sums,  double *&sums_above,
                          double *&sqsum, double *&sqsum_above,
                          __m512i &sum_accumulator, __m512i &sqsum_accumulator,
                          int num_ops_in_line)
    {
        // This function handles full 64 byte chunks of the source data at a time until it gets to the part of
        // the line that no longer contains a full 64 byte chunk.  Other code will handle the last part.

        const int num_chunks = num_ops_in_line >> 6;  // quick int divide by 64

        for (int index_64byte_chunk = 0; index_64byte_chunk < num_chunks; index_64byte_chunk++){
            integral_64_operations_avx512((__m512i *) srcs,
                                          (__m512i *) sums,  (__m512i *) sums_above,
                                          (__m512i *) sqsum, (__m512i *) sqsum_above,
                                          0xFFFFFFFFFFFFFFFF, sum_accumulator, sqsum_accumulator);
            srcs+=64; sums+=64; sums_above+=64;
            if (sqsum){ sqsum+= 64; sqsum_above+=64; }
        }
    }


    static CV_ALWAYS_INLINE
    void post_aligned_integral(const uchar *srcs,
                               const double *sums,   const double *sums_above,
                               const double *sqsum,  const double *sqsum_above,
                               __m512i &sum_accumulator, __m512i &sqsum_accumulator,
                               int num_ops_in_line)
    {
        // This function handles the last few straggling operations that are not a full chunk of 64 operations
        // We use the same algorithm, but we calculate a different operation mask using (num_ops % 64).

        const unsigned int num_operations = (unsigned int) num_ops_in_line & 0x3F;  // Quick int modulo 64

        if (num_operations > 0) {
            __mmask64 operation_mask = (1ULL << num_operations) - 1ULL;

            integral_64_operations_avx512((__m512i *) srcs, (__m512i *) sums, (__m512i *) sums_above,
                                          (__m512i *) sqsum, (__m512i *) sqsum_above,
                                          operation_mask, sum_accumulator, sqsum_accumulator);
        }
    }


    static CV_ALWAYS_INLINE
    void integral_64_operations_avx512(const __m512i *srcs,
                                       __m512i *sums,       const __m512i *sums_above,
                                       __m512i *sqsums,     const __m512i *sqsums_above,
                                       __mmask64 data_mask,
                                       __m512i &sum_accumulator, __m512i &sqsum_accumulator)
    {
       __m512i src_64byte_chunk = read_64_bytes(srcs, data_mask);

        while (data_mask) {
            __m128i src_16bytes = extract_lower_16bytes(src_64byte_chunk);

            __m512i src_longs_lo = convert_lower_8bytes_to_longs(src_16bytes);
            __m512i src_longs_hi = convert_lower_8bytes_to_longs(shift_right_8_bytes(src_16bytes));

            // Calculate integral for the sum on the 8 lanes at a time
            integral_8_operations(src_longs_lo, sums_above, data_mask, sums, sum_accumulator);
            integral_8_operations(src_longs_hi, sums_above+1, data_mask>>8, sums+1, sum_accumulator);

            if (sqsums) {
                __m512i squared_source_lo = square_m512(src_longs_lo);
                __m512i squared_source_hi = square_m512(src_longs_hi);

                integral_8_operations(squared_source_lo, sqsums_above, data_mask, sqsums, sqsum_accumulator);
                integral_8_operations(squared_source_hi, sqsums_above+1, data_mask>>8, sqsums+1, sqsum_accumulator);
                sqsums += 2;
                sqsums_above+=2;
            }

            // Prepare for next iteration of loop
            // shift source to align next 16 bytes to lane 0, shift the mask, and advance the pointers
            sums += 2;
            sums_above += 2;
            data_mask = data_mask >> 16;
            src_64byte_chunk = shift_right_16_bytes(src_64byte_chunk);

        }

    }


    static CV_ALWAYS_INLINE
    void integral_8_operations(const __m512i src_longs, const __m512i *above_values_ptr, __mmask64 data_mask,
                               __m512i *results_ptr, __m512i &accumulator)
     {
        // NOTE that the calculate_integral function referenced here must be implemented in the templated
        // derivatives because the algorithm depends heavily on the number of channels in the image
        //
        _mm512_mask_storeu_pd(
                results_ptr,   // Store the result here
                data_mask,     // Using the data mask to avoid overrunning the line
                calculate_integral( // Writing the value of the integral derived from:
                        src_longs,                                           // input data
                        _mm512_maskz_loadu_pd(data_mask, above_values_ptr),  // and the results from line above
                        accumulator                                          // keeping track of the accumulator
                )
        );
    }


    // The calculate_integral function referenced here must be implemented in the templated derivatives
    // because the algorithm depends heavily on the number of channels in the image
    // This is the incomplete definition (just the prototype) here.
    //
    static CV_ALWAYS_INLINE
    __m512d calculate_integral(__m512i src_longs, const __m512d above_values, __m512i &accumulator);


    static CV_ALWAYS_INLINE
    __m512i read_64_bytes(const __m512i *srcs, __mmask64 data_mask)  {
        return _mm512_maskz_loadu_epi8(data_mask, srcs);
    }


    static CV_ALWAYS_INLINE
    __m128i extract_lower_16bytes(__m512i src_64byte_chunk) {
        return _mm512_extracti64x2_epi64(src_64byte_chunk, 0x0);
    }


    static CV_ALWAYS_INLINE
    __m512i convert_lower_8bytes_to_longs(__m128i src_16bytes)  {
        return _mm512_cvtepu8_epi64(src_16bytes);
    }


    static CV_ALWAYS_INLINE
    __m512i square_m512(__m512i src_longs) {
        return _mm512_mullo_epi64(src_longs, src_longs);
    }


    static CV_ALWAYS_INLINE
    __m128i shift_right_8_bytes(__m128i src_16bytes)  {
        return _mm_maskz_compress_epi64(2, src_16bytes);
    }



    static CV_ALWAYS_INLINE
    __m512i shift_right_16_bytes(__m512i src_64byte_chunk)  {
        return _mm512_maskz_compress_epi64(0xFC, src_64byte_chunk);
    }


};


//============================================================================================================
// This the only section that needs to change with respect to algorithm based on the number of channels
// It is responsible for returning the calculation of 8 lanes worth of the integral and returning in the
// accumulated sums in the accumulator parameter (NOTE: accumulator is an input and output parameter)
//
// The function prototype that needs to be implemented is:
//
//     __m512d calculate_integral(__m512i src_longs, const __m512d above_values, __m512i &accumulator){ ... }
//
// Description of parameters:
//   INPUTS:
//      src_longs   : 8 lanes worth of the source bytes converted to 64 bit integers
//      above_values: 8 lanes worth of the result values from the line above (See the integral spec)
//      accumulator : 8 lanes worth of sums from the previous iteration
//                    IMPORTANT NOTE: This parameter is both an INPUT AND OUTPUT parameter to this function
//
//   OUTPUTS:
//      return value: The actual integral value for all 8 lanes which is defined by the spec as
//                    the sum of all channel values to the left of a given pixel plus the result
//                    written to the line directly above the current line.
//      accumulator:  This is an input and and output.  This parameter should be left with the accumulated
//                    sums for the current 8 lanes keeping all entries in the proper lane (do not shuffle it)
//
// Below here is the channel specific implementation
//

//========================================
//   2 Channel Integral Implementation
//========================================
template<>
CV_ALWAYS_INLINE
__m512d IntegralCalculator < 2 > ::calculate_integral(__m512i src_longs, const __m512d above_values, __m512i &accumulator)
{
    __m512i carryover_idxs = _mm512_set_epi64(7, 6, 7, 6, 7, 6, 7, 6);

    // Align data to prepare for the adds:
    //    shifts data left by 3 and 6 qwords(lanes) and gets rolling sum in all lanes
    //   Vertical LANES  :   76543210
    //   src_longs       :   HGFEDCBA
    //   shift2lanes     : + FEDCBA
    //   shift4lanes     : + DCBA
    //   shift6lanes     : + BA
    //   carry_over_idxs : + 76767676  (index position of result from previous iteration)
    //                     = integral
    __m512i shift2lanes = _mm512_maskz_expand_epi64(0xFC, src_longs);
    __m512i shift4lanes = _mm512_maskz_expand_epi64(0xF0, src_longs);
    __m512i shift6lanes = _mm512_maskz_expand_epi64(0xC0, src_longs);
    __m512i carry_over  = _mm512_permutex2var_epi64(accumulator, carryover_idxs, accumulator);

    // Add all values in tree form for perf ((0+2) + (4+6))
    __m512i sum_shift_02  = _mm512_add_epi64(src_longs,    shift2lanes);
    __m512i sum_shift_46  = _mm512_add_epi64(shift4lanes,  shift6lanes);
    __m512i sum_all       = _mm512_add_epi64(sum_shift_02, sum_shift_46);
    accumulator           = _mm512_add_epi64(sum_all,      carry_over);

    // Convert to packed double and add to the line above to get the true integral value
    __m512d accumulator_pd = _mm512_cvtepu64_pd(accumulator);
    __m512d integral_pd    = _mm512_add_pd(accumulator_pd, above_values);
    return integral_pd;
}

//========================================
//   3 Channel Integral Implementation
//========================================
template<>
CV_ALWAYS_INLINE
__m512d IntegralCalculator < 3 > ::calculate_integral(__m512i src_longs, const __m512d above_values, __m512i &accumulator)
{
    __m512i carryover_idxs = _mm512_set_epi64(6, 5, 7, 6, 5, 7, 6, 5);

    // Align data to prepare for the adds:
    //    shifts data left by 3 and 6 qwords(lanes) and gets rolling sum in all lanes
    //   Vertical LANES:     76543210
    //   src_longs       :   HGFEDCBA
    //   shit3lanes      : + EDCBA
    //   shift6lanes     : + BA
    //   carry_over_idxs : + 65765765  (index position of result from previous iteration)
    //                     = integral
    __m512i shift3lanes = _mm512_maskz_expand_epi64(0xF8, src_longs);
    __m512i shift6lanes = _mm512_maskz_expand_epi64(0xC0, src_longs);
    __m512i carry_over    = _mm512_permutex2var_epi64(accumulator, carryover_idxs, accumulator);

    // Do the adds in tree form
    __m512i sum_shift_03 = _mm512_add_epi64(src_longs,    shift3lanes);
    __m512i sum_rest     = _mm512_add_epi64(shift6lanes,  carry_over);
    accumulator          = _mm512_add_epi64(sum_shift_03, sum_rest);

    // Convert to packed double and add to the line above to get the true integral value
    __m512d accumulator_pd = _mm512_cvtepu64_pd(accumulator);
    __m512d integral_pd    = _mm512_add_pd(accumulator_pd, above_values);
    return integral_pd;
}


//========================================
//   4 Channel Integral Implementation
//========================================
template<>
CV_ALWAYS_INLINE
__m512d IntegralCalculator < 4 > ::calculate_integral(__m512i src_longs, const __m512d above_values, __m512i &accumulator)
{
    __m512i carryover_idxs = _mm512_set_epi64(7, 6, 5, 4, 7, 6, 5, 4);

    // Align data to prepare for the adds:
    //    shifts data left by 3 and 6 qwords(lanes) and gets rolling sum in all lanes
    //   Vertical LANES:     76543210
    //   src_longs       :   HGFEDCBA
    //   shit4lanes      : + DCBA
    //   carry_over_idxs : + 76547654  (index position of result from previous iteration)
    //                     = integral
    __m512i shifted4lanes = _mm512_maskz_expand_epi64(0xF0, src_longs);
    __m512i carry_over    = _mm512_permutex2var_epi64(accumulator, carryover_idxs, accumulator);

    // Add data pixels and carry over from last iteration
    __m512i sum_shift_04 = _mm512_add_epi64(src_longs,    shifted4lanes);
    accumulator          = _mm512_add_epi64(sum_shift_04, carry_over);

    // Convert to packed double and add to the line above to get the true integral value
    __m512d accumulator_pd = _mm512_cvtepu64_pd(accumulator);
    __m512d integral_pd    = _mm512_add_pd(accumulator_pd, above_values);
    return integral_pd;
}


} // end of anonymous namespace

namespace opt_AVX512_SKX {

// This is the implementation for the external callers interface entry point.
// It should be the only function called into this file from outside
// Any new implementations should be directed from here
void calculate_integral_avx512(const uchar *src,   size_t _srcstep,
                               double      *sum,   size_t _sumstep,
                               double      *sqsum, size_t _sqsumstep,
                               int width, int height, int cn)
{
    switch(cn){
        case 2: {
            IntegralCalculator<2> calculator;
            calculator.calculate_integral_avx512(src, _srcstep, sum, _sumstep, sqsum, _sqsumstep, width, height);
            break;
        }
        case 3: {
            IntegralCalculator<3> calculator;
            calculator.calculate_integral_avx512(src, _srcstep, sum, _sumstep, sqsum, _sqsumstep, width, height);
            break;
        }
        case 4: {
            IntegralCalculator<4> calculator;
            calculator.calculate_integral_avx512(src, _srcstep, sum, _sumstep, sqsum, _sqsumstep, width, height);
        }
    }
}


} // end namespace opt_AVX512_SXK
} // end namespace cv