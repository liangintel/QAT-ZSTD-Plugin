/***************************************************************************
 *
 *   BSD LICENSE
 *
 *   Copyright(c) 2023 Intel Corporation. All rights reserved.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 *
 ***************************************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <time.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

#ifndef ZSTD_STATIC_LINKING_ONLY
#define ZSTD_STATIC_LINKING_ONLY
#endif
#include "zstd.h"
#include "zstd_errors.h"
#include "qatseqprod.h"

// #define QATZIP_ENABLED  1
// #define XFC_ENABLED  1

#define uint64_t size_t
#ifdef XFC_ENABLED
#include "xFastCompress.h"
#endif

#include "qatzip.h"
#include "qz_utils.h"
#include "libdeflate.h"

#define NANOSEC (1000000000ULL) /* 1 second */
#define NANOUSEC (1000) /* 1 usec */
#define MB (1000000)   /* 1MB */
#define BUCKET_NUM  200
#define DEFAULT_CHUNK_SIZE (32 * 1024)
#define ZSTD_AUTO     0
#define ZSTD_ENABLED  1
#define ZSTD_DISABLED 2


#ifndef MIN
#define MIN(a,b) ((a)<(b)?(a):(b))
#endif

#define DISPLAY(...)  fprintf(stderr, __VA_ARGS__)

#define GETTIME(now) {clock_gettime(CLOCK_MONOTONIC, &now);};
#define GETDIFFTIME(start_ticks, end_ticks) (1000000000ULL*( end_ticks.tv_sec - start_ticks.tv_sec ) + ( end_ticks.tv_nsec - start_ticks.tv_nsec ))

typedef enum {
    SOFTWARE_ENGINE = 0,
    QAT_ZSTD_ENGINE = 1,
    QATZIP_ENGINE = 2,
    XFASTCOMPRESS_ENGINE = 3,
    LIBDEFLATE_ENGINE = 4
} engine_t;

const char* get_engine_name(engine_t eng) {
    switch(eng) {
    case SOFTWARE_ENGINE: return "Software ZSTD"; break;
    case QAT_ZSTD_ENGINE: return "QAT ZSTD"; break;
    case QATZIP_ENGINE: return "QATzip Deflate"; break;
    case XFASTCOMPRESS_ENGINE: return "xFastCompress Deflate"; break;
    case LIBDEFLATE_ENGINE: return "libdeflate"; break;
    default: return "Engine Name Error!!!"; break;
    }
    return "Engine Name Error!!!";
}

typedef struct {
    size_t chunkSize; /* the max chunk size of ZSTD_compress2 */
    size_t srcSize;  /* Input file size */
    unsigned cLevel; /* Compression Level 1 - 12 */
    unsigned nbIterations; /* Number test loops, default is 1 */
    char benchMode; /* 0: software compression, 1: QAT compression*/
    char searchForExternalRepcodes; /* 0: auto 1: enable, 2: disable */
    const unsigned char *srcBuffer; /* Input data point */
} threadArgs_t;

typedef struct {
    size_t bucketValue[BUCKET_NUM];
    size_t bucket[BUCKET_NUM];
    size_t maxBucketValue;
    size_t minBucketValue;
    int bucketCount;
    size_t num;
    size_t sum;
    size_t min;
    size_t max;
} HistogramStat_t;

static HistogramStat_t compHistogram;
static HistogramStat_t decompHistogram;
static pthread_barrier_t g_threadBarrier1, g_threadBarrier2;
static size_t g_threadNum = 0;

static void initHistorgram(HistogramStat_t *historgram)
{
    historgram->bucketValue[0] = 1000;
    historgram->minBucketValue = historgram->bucketValue[0];
    historgram->bucketCount = 1;
    for (int i = 1; i < BUCKET_NUM; i++) {
        if (historgram->bucketValue[i] > 0XFFFFFFFF) {
            break;
        }
        historgram->bucketValue[i] = historgram->bucketValue[i - 1] * 1.05;
        historgram->maxBucketValue = historgram->bucketValue[i];
        historgram->bucketCount++;
    }
    memset(historgram->bucket, 0, sizeof(size_t) * BUCKET_NUM);
    historgram->num = 0;
    historgram->sum = 0;
    historgram->min = 0xFFFFFFFF;
    historgram->max = 0;
}

static int getBucketIndex(HistogramStat_t *historgram, size_t value)
{
    for (int bucketIndex = 0; bucketIndex < BUCKET_NUM; bucketIndex++) {
        if (value < historgram->bucketValue[bucketIndex]) {
            return bucketIndex;
        }
    }
    return BUCKET_NUM - 1;
}

static void bucketAdd(HistogramStat_t *historgram, size_t value)
{
    int bucketIndex = getBucketIndex(historgram, value);
    __sync_fetch_and_add(&historgram->bucket[bucketIndex], 1);

    __sync_fetch_and_add(&historgram->sum, value);
    __sync_fetch_and_add(&historgram->num, 1);
    if (value < historgram->min)
        __sync_lock_test_and_set(&historgram->min, value);
    if (value > historgram->max)
        __sync_lock_test_and_set(&historgram->max, value);
}

static double percentile(HistogramStat_t *historgram, double p)
{
    double threshold = historgram->num * (p / 100.0);
    size_t cumulative_sum = 0;
    for (int index = 0; index < historgram->bucketCount; index++) {
        size_t bucket_count = historgram->bucket[index];
        cumulative_sum += bucket_count;
        if (cumulative_sum >= threshold) {
            size_t left_point = (index == 0) ? 0 : historgram->bucketValue[index - 1];
            size_t right_point =  historgram->bucketValue[index];
            size_t left_sum = cumulative_sum - bucket_count;
            size_t right_sum = cumulative_sum;
            double pos = 0;
            size_t right_left_diff = right_sum - left_sum;
            if (right_left_diff != 0) {
                pos = (threshold - left_sum) / right_left_diff;
            }
            double r = left_point + (right_point - left_point) * pos;
            if (r < historgram->min)
                r = historgram->min;
            if (r > historgram->max)
                r = historgram->max;
            return r;
        }
    }
    return historgram->max;
}

static int usage(const char *exe)
{
    DISPLAY("Usage:\n");
    DISPLAY("      %s [arg] filename\n", exe);
    DISPLAY("Options:\n");
    DISPLAY("    -t#       Set maximum threads [1 - 128] (default: 1)\n");
    DISPLAY("    -l#       Set iteration loops [1 - 1000000](default: 1)\n");
    DISPLAY("    -c#       Set chunk size (default: 32K)\n");
    DISPLAY("    -E#       Auto/enable/disable searchForExternalRepcodes(0: auto; 1: enable; 2: disable; default: auto)\n");
    DISPLAY("    -L#       Set compression level [1 - 12] (default: 1)\n");
    DISPLAY("    -m#       Benchmark mode, 0:software compression; 1:QAT ZSTD compression(default: 1) 2:QATzip deflate 3:xFastCompress 4:libdeflate \n");
    DISPLAY("    -h/H      Print this help message\n");
    return 0;
}

/* this function to convert string to unsigned int,
 * the string MUST BE starting with numeric and can be
 * end with "K" or "M". Such as:
 * if input string is "128K" and output will be 131072.
 * if input string is "65536" and output will be 65536.
 */
static unsigned stringToU32(const char **s)
{
    unsigned value = 0;
    while ((**s >= '0') && (**s <= '9')) {
        if (value > ((((unsigned)(-1)) / 10) - 1)) {
            DISPLAY("ERROR: numeric value is too large\n");
            exit(1);
        }
        value *= 10;
        value += (unsigned)(**s - '0');
        (*s)++ ;
    }
    if ((**s == 'K') || (**s == 'M')) {
        if (value > ((unsigned)(-1)) >> 10) {
            DISPLAY("ERROR: numeric value is too large\n");
            exit(1);
        }
        value <<= 10;
        if (**s == 'M') {
            if (value > ((unsigned)(-1)) >> 10) {
                DISPLAY("ERROR: numeric value is too large\n");
                exit(1);
            }
            value <<= 10;
        }
        (*s)++;
    }
    return value;
}

#define QZ_INIT_FAIL(rc)          (QZ_OK != rc     &&  \
                                   QZ_DUPLICATE != rc)

int qzInitSetupsession(QzSession_T *sess, int sw_backup)
{
    int status = QZ_OK;
    // int sw_backup = 1;

    status = qzInit(sess, sw_backup);
    if (QZ_INIT_FAIL(status)) {
        return status;
    }

    QzSessionParamsDeflate_T params;
    status = qzGetDefaultsDeflate(&params);
    if (status < 0) {
        QZ_ERROR("Get defaults params error with error: %d\n", status);
        return QZ_FAIL;
    }

    params.data_fmt = QZ_DEFLATE_GZIP_EXT;

    //params.huffman_hdr = QZ_STATIC_HDR;
    params.huffman_hdr = QZ_DYNAMIC_HDR;
    params.common_params.comp_lvl = 6;
    params.common_params.comp_algorithm = QZ_DEFLATE;
    params.common_params.hw_buff_sz = 65536;
    params.common_params.polling_mode = QZ_BUSY_POLLING;
    // params.common_params.req_cnt_thrshold = QZ_REQ_THRESHOLD_DEFAULT;
    // params.common_params.max_forks = arg->max_forks;
    params.common_params.sw_backup = sw_backup;

    status = qzSetupSessionDeflate(sess, &params);
    if (status < 0) {
        QZ_ERROR("Session setup failed with error: %d\n", status);
        return QZ_FAIL;
    }

    return QZ_OK;
}

typedef enum {
    ZSTD = 1, 
    QATZIP = 2,
    X_FAST_COMPRESS_DEFLATE = 3
} algorithm_t;

void *benchmark(void *args)
{
    threadArgs_t *threadArgs = (threadArgs_t *)args;
    algorithm_t algorithm = X_FAST_COMPRESS_DEFLATE;
    size_t rc = 0, threadNum;
    unsigned loops;
    int verifyResult = 0; /* 1: pass, 0: fail */
    size_t *chunkSizes = NULL; /* The array of chunk size */
    size_t *compSizes = NULL; /* The array of compressed size */
    size_t nanosec = 0;
    size_t compNanosecSum = 0, decompNanosecSum = 0;
    double compSpeed = 0, decompSpeed = 0, ratio = 0;
    size_t csCount, nbChunk, destSize, cSize, dcSize;
    struct timespec startTicks, endTicks;
    unsigned char *destBuffer = NULL, *decompBuffer = NULL;
    const unsigned char *srcBuffer = threadArgs->srcBuffer;
    size_t srcSize = threadArgs->srcSize;
    size_t chunkSize = threadArgs->chunkSize;
    unsigned nbIterations = threadArgs->nbIterations;
    unsigned cLevel = threadArgs->cLevel;
    ZSTD_CCtx *const zc = ZSTD_createCCtx();
    ZSTD_DCtx *const zdc = ZSTD_createDCtx();
    void *matchState = NULL;
    int setUpStatus = 0, compressStatus = 0;

    //QATzip for deflate
    QzSession_T sess = {0};

    //xFastCompress
#ifdef XFC_ENABLED
    xfc_t xfc;
#endif

    //libdeflate
    struct libdeflate_compressor *libdeflate1 = libdeflate_alloc_compressor(1);
    struct libdeflate_decompressor *libdeflate2 = libdeflate_alloc_decompressor();

    csCount = srcSize / chunkSize + (srcSize % chunkSize ? 1 : 0);
    chunkSizes = (size_t *)malloc(csCount * sizeof(size_t));
    compSizes = (size_t *)malloc(csCount * sizeof(size_t));
    assert(chunkSizes && compSizes);
    size_t tmpSize = srcSize;
    for (nbChunk = 0; nbChunk < csCount; nbChunk++) {
        chunkSizes[nbChunk] = MIN(tmpSize, chunkSize);
        tmpSize -= chunkSizes[nbChunk];
    }

    destSize = ZSTD_compressBound(srcSize);
    destBuffer = (unsigned char *)malloc(destSize);
    decompBuffer = (unsigned char *)malloc(srcSize);
    assert(destBuffer != NULL);

    if (threadArgs->benchMode == QAT_ZSTD_ENGINE) {
        QZSTD_startQatDevice();
        matchState = QZSTD_createSeqProdState();
        ZSTD_registerSequenceProducer(zc, matchState, qatSequenceProducer);
    } else {
        ZSTD_registerSequenceProducer(zc, NULL, NULL);
    }
    if (threadArgs->benchMode == QATZIP_ENGINE) {
        int rc2 = qzInitSetupsession(&sess, 1);
        if (rc2 != QZ_OK && rc2 != QZ_DUPLICATE) {
            pthread_exit((void *)"qzInit failed");
        }
    } else if (threadArgs->benchMode == XFASTCOMPRESS_ENGINE) {
#ifdef XFC_ENABLED
        if(ECORD_SECCESS != xfc_alloc_compressor(&xfc))
            pthread_exit((void *)"xfc_alloc_compressor failed.");
#endif
    }

    if (threadArgs->searchForExternalRepcodes == ZSTD_ENABLED) {
        rc = ZSTD_CCtx_setParameter(zc, ZSTD_c_searchForExternalRepcodes,
                                    ZSTD_ps_enable);
    } else if (threadArgs->searchForExternalRepcodes == ZSTD_DISABLED) {
        rc = ZSTD_CCtx_setParameter(zc, ZSTD_c_searchForExternalRepcodes,
                                    ZSTD_ps_disable);
    } else {
        rc = ZSTD_CCtx_setParameter(zc, ZSTD_c_searchForExternalRepcodes, ZSTD_ps_auto);
    }
    if (ZSTD_isError(rc)) {
        DISPLAY("Fail to set parameter ZSTD_c_searchForExternalRepcodes\n");
        goto setupend;
    }

    rc = ZSTD_CCtx_setParameter(zc, ZSTD_c_compressionLevel, cLevel);
    if (ZSTD_isError(rc)) {
        DISPLAY("Fail to set parameter ZSTD_c_compressionLevel\n");
        goto setupend;
    }

    setUpStatus = 1;

    /* warm up */
    for (loops = 0; loops < 1; loops++) {
        unsigned char *tmpDecompBuffer = decompBuffer;
        unsigned char *tmpDestBuffer = destBuffer;
        const unsigned char *tmpSrcBuffer = srcBuffer;
        size_t tmpDestSize = destSize;
        for (nbChunk = 0; nbChunk < csCount; nbChunk++) {
            if (threadArgs->benchMode == SOFTWARE_ENGINE || threadArgs->benchMode == QAT_ZSTD_ENGINE) {
                cSize = ZSTD_compress2(zc, tmpDestBuffer, tmpDestSize, tmpSrcBuffer,
                                    chunkSizes[nbChunk]);
            }

#ifdef XFC_ENABLED
            if (threadArgs->benchMode == XFASTCOMPRESS_ENGINE) {
                const char* in_buf = tmpSrcBuffer;
                char* out_buf = tmpDestBuffer;
                int in_len = chunkSizes[nbChunk];
                int out_len = in_len;   //liang: should be larger
                int ret = xfc_compress(xfc, in_buf, in_len, &out_buf, &out_len, 1, 0);
            }
#endif

            if (threadArgs->benchMode == QATZIP_ENGINE) {
                unsigned int in_sz = chunkSizes[nbChunk];
                unsigned int out_sz = tmpDestSize;
                int ret = qzCompress(&sess, tmpSrcBuffer, &in_sz, tmpDestBuffer, &out_sz, 1);
                if (ret != QZ_OK) {
                    QZ_ERROR("ERROR: Compression FAILED with return value: %d\n", ret);
                }
                cSize = out_sz;

                in_sz = cSize;
                out_sz = chunkSizes[nbChunk];
                ret = qzDecompress(&sess, tmpDestBuffer, &in_sz, tmpDecompBuffer, &out_sz);
                if (ret != QZ_OK) {
                    QZ_ERROR("ERROR: DeCompression FAILED with return value: %d\n", ret);
                }
            }

            tmpDestBuffer += cSize;
            tmpDestSize -= cSize;
            tmpSrcBuffer += chunkSizes[nbChunk];
            tmpDecompBuffer += chunkSizes[nbChunk];
        }
    }

setupend:

    /* Waiting all threads */
    pthread_barrier_wait(&g_threadBarrier1);
    if (!setUpStatus) {
        goto compressend;
    }

    /* Start compression benchmark */
    for (loops = 0; loops < nbIterations; loops++) {
        unsigned char *tmpDestBuffer = destBuffer;
        const unsigned char *tmpSrcBuffer = srcBuffer;
        size_t tmpDestSize = destSize;
        for (nbChunk = 0; nbChunk < csCount; nbChunk++) {
            GETTIME(startTicks);
            if (threadArgs->benchMode == SOFTWARE_ENGINE || threadArgs->benchMode == QAT_ZSTD_ENGINE) {
                cSize = ZSTD_compress2(zc, tmpDestBuffer, tmpDestSize, tmpSrcBuffer,
                                    chunkSizes[nbChunk]);
            }

#ifdef XFC_ENABLED
            // xfastcompress deflate
            if (threadArgs->benchMode == XFASTCOMPRESS_ENGINE)
            {
                const char* in_buf = tmpSrcBuffer;
                char* out_buf = tmpDestBuffer;
                int in_len = chunkSizes[nbChunk];
                int out_len = in_len;   //liang: should be larger

                // int ret = batch_compress(&in_buf, &in_len, &out_buf, &out_len, 1, 1, 0);
                int ret = xfc_compress(xfc, in_buf, in_len, &out_buf, &out_len, 1, 0);
                if(ret) {
                    printf("xfc_compress return failer.");
                }
            }
#endif

            // QATzip deflate
            if (threadArgs->benchMode == QATZIP_ENGINE) {
                unsigned int in_sz = chunkSizes[nbChunk];
                unsigned int out_sz = tmpDestSize;
                int ret = qzCompress(&sess, tmpSrcBuffer, &in_sz, tmpDestBuffer, &out_sz, 1);
                if (ret != QZ_OK) {
                    QZ_ERROR("ERROR: Compression FAILED with return value: %d\n", ret);
                }
                cSize = out_sz;
            }

            if (threadArgs->benchMode == LIBDEFLATE_ENGINE) {
                cSize = libdeflate_gzip_compress(libdeflate1, tmpSrcBuffer, chunkSizes[nbChunk], tmpDestBuffer, tmpDestSize);
            }

            GETTIME(endTicks);
            if (ZSTD_isError(cSize)) {
                DISPLAY("Compress failed\n");
                goto compressend;
            }
            tmpDestBuffer += cSize;
            tmpDestSize -= cSize;
            tmpSrcBuffer += chunkSizes[nbChunk];
            compSizes[nbChunk] = cSize;
            nanosec = GETDIFFTIME(startTicks, endTicks);
            bucketAdd(&compHistogram, nanosec);
            compNanosecSum += nanosec;
        }
    }

    cSize = 0;
    for (nbChunk = 0; nbChunk < csCount; nbChunk++) {
        cSize += compSizes[nbChunk];
    }

    /* Verify the compression result */
    {
        if (threadArgs->benchMode == SOFTWARE_ENGINE || threadArgs->benchMode == QAT_ZSTD_ENGINE) {
            rc = ZSTD_decompress(decompBuffer, srcSize, destBuffer, cSize);
            if (rc != srcSize) {
                DISPLAY("Decompressed size is not equal to source size\n");
                goto compressend;
            }
        }

        if (threadArgs->benchMode == QATZIP_ENGINE) {
            unsigned int in_sz = cSize;
            unsigned int out_sz = srcSize;
            int ret = qzDecompress(&sess, destBuffer, &in_sz, decompBuffer, &out_sz);
            if (ret != QZ_OK) {
                QZ_ERROR("ERROR: DeCompression FAILED with return value: %d\n", ret);
                goto compressend;
            }
        }

        if (threadArgs->benchMode == LIBDEFLATE_ENGINE) {
            size_t out_sz = 0;
            int ret1 = libdeflate_gzip_decompress(libdeflate2, 
                        destBuffer, cSize, 
                        decompBuffer, srcSize, &out_sz);
            if(LIBDEFLATE_SUCCESS != ret1) {
                DISPLAY("libdeflate Decompress failed. ret=%d.\n", ret1);
            }
        }

#ifdef XFC_ENABLED
        if (threadArgs->benchMode == XFASTCOMPRESS_ENGINE) {
            const char* in_buf = destBuffer;
            char* out_buf = decompBuffer;
            int in_len = cSize;
            int out_len = srcSize;   //liang: should be larger
            int ret = xfc_compress(xfc, in_buf, in_len, &out_buf, &out_len, 0, 0);
        }
#endif
        /* Compare original buffer with decompress output */
        if (!memcmp(decompBuffer, srcBuffer, srcSize)) {
            verifyResult = 1;
        } else {
            verifyResult = 0;
            QZ_ERROR("ERROR: Decompressed != src\n");
            goto compressend;
        }
    }

    compressStatus = 1;

compressend:

    pthread_barrier_wait(&g_threadBarrier2);
    if (!setUpStatus || !compressStatus) {
        goto exit;
    }
    /* Start decompression benchmark */
    for (loops = 0; loops < nbIterations; loops++) {
        unsigned char *tmpDestBuffer = decompBuffer;
        const unsigned char *tmpSrcBuffer = destBuffer;
        const unsigned char *tmpOriBuffer = srcBuffer;
        size_t tmpDestSize = srcSize;
        for (nbChunk = 0; nbChunk < csCount; nbChunk++) {
            GETTIME(startTicks);
            if (threadArgs->benchMode == SOFTWARE_ENGINE || threadArgs->benchMode == QAT_ZSTD_ENGINE) {
                dcSize = ZSTD_decompressDCtx(zdc, tmpDestBuffer, tmpDestSize, tmpSrcBuffer,
                                            compSizes[nbChunk]);
            }

            //QATzip deflate
            if (threadArgs->benchMode == QATZIP_ENGINE) {
                unsigned int in_sz = compSizes[nbChunk];
                unsigned int out_sz = chunkSizes[nbChunk];
                // QZ_ERROR("before in_sz=%d, out_sz=%d\n", in_sz, out_sz);
                int ret = qzDecompress(&sess, tmpSrcBuffer, &in_sz, tmpDestBuffer, &out_sz);
                if (ret != QZ_OK) {
                    QZ_ERROR("ERROR: DeCompression FAILED with return value: %d\n", ret);
                    // goto done;
                }
                dcSize = out_sz;
                // QZ_ERROR("after in_sz=%d, out_sz=%d\n", in_sz, out_sz);
            }

#ifdef XFC_ENABLED
            if (threadArgs->benchMode == XFASTCOMPRESS_ENGINE) {
                // const char* in_buf = destBuffer;
                // char* out_buf = decompBuffer;
                // int in_len = cSize;
                // int out_len = srcSize;   //liang: should be larger
                // int ret = xfc_compress(xfc, in_buf, in_len, &out_buf, &out_len, 0, 0);
            }
#endif
            if (threadArgs->benchMode == LIBDEFLATE_ENGINE) {
                if(LIBDEFLATE_SUCCESS != libdeflate_gzip_decompress(libdeflate2, 
                            tmpSrcBuffer, compSizes[nbChunk], 
                            tmpDestBuffer, chunkSizes[nbChunk], &dcSize)) {
                    DISPLAY("libdeflate Decompress failed\n");
                }
            }

            GETTIME(endTicks);
            /* Compare original buffer with decompress output */
            if (memcmp(tmpDestBuffer, tmpOriBuffer, dcSize)) {
                QZ_ERROR("ERROR: Decompressed != src, nbChunk=%ld\n", nbChunk);
            }

            if (ZSTD_isError(dcSize)) {
                DISPLAY("Decompress failed\n");
                goto exit;
            }
            tmpDestBuffer += dcSize;
            tmpDestSize -= dcSize;
            tmpSrcBuffer += compSizes[nbChunk];
            tmpOriBuffer += dcSize;
            nanosec = GETDIFFTIME(startTicks, endTicks);
            bucketAdd(&decompHistogram, nanosec);
            decompNanosecSum += nanosec;
        }
    }

    /*Get current thread num */
    threadNum = __sync_add_and_fetch(&g_threadNum, 1);

    ratio = (double) cSize / (double)srcSize;
    compSpeed = (double)(srcSize * nbIterations) / ((double)compNanosecSum /
                NANOSEC);
    decompSpeed = (double)(srcSize * nbIterations) / ((double)decompNanosecSum /
                  NANOSEC);
    DISPLAY("Engine: [%s]\n", get_engine_name(threadArgs->benchMode));
    DISPLAY("Thread %lu: Compression: %lu -> %lu, Throughput: Comp: %5.f MB/s, Decomp: %5.f MB/s, Compression Ratio: %2.2f%%, %s\n",
            threadNum, srcSize, cSize, (double) compSpeed / MB, (double) decompSpeed / MB,
            ratio * 100,
            verifyResult ? "PASS" : "FAIL");
exit:
    // QATzip deflate
#ifdef QATZIP_ENABLED
    (void)qzTeardownSession(&sess);
    qzClose(&sess);
#endif

#ifdef XFC_ENABLED
    // xFastCompress Deflate
    //free_all_resources();
#endif

    // libdeflate
    libdeflate_free_compressor(libdeflate1);
    libdeflate_free_decompressor(libdeflate2);

    ZSTD_freeCCtx(zc);
    ZSTD_freeDCtx(zdc);
    if (threadArgs->benchMode == QAT_ZSTD_ENGINE && matchState) {
        QZSTD_freeSeqProdState(matchState);
    }
    if (chunkSizes) {
        free(chunkSizes);
    }
    if (compSizes) {
        free(compSizes);
    }
    if (destBuffer) {
        free(destBuffer);
    }
    if (decompBuffer) {
        free(decompBuffer);
    }
    return NULL;
}

int main(int argc, const char **argv)
{
    int argNb, threadNb;
    int nbThreads = 1;
    pthread_t threads[2048];
    size_t srcSize, bytesRead;
    unsigned char *srcBuffer = NULL;
    const char *fileName = NULL;
    int inputFile = -1;
    threadArgs_t threadArgs;

    if (argc < 2)
        return usage(argv[0]);

    /* Set default value */
    threadArgs.chunkSize = DEFAULT_CHUNK_SIZE;
    threadArgs.nbIterations = 1;
    threadArgs.cLevel = 1;
    threadArgs.benchMode = QAT_ZSTD_ENGINE;
    threadArgs.searchForExternalRepcodes = ZSTD_AUTO;

    for (argNb = 1; argNb < argc; argNb++) {
        const char *arg = argv[argNb];
        if (arg[0] == '-') {
            arg++;
            while (arg[0] != 0) {
                switch (arg[0]) {
                /* Display help message */
                case 'h':
                case 'H':
                    return usage(argv[0]);
                /* Set maximum threads */
                case 't':
                    arg++;
                    nbThreads = stringToU32(&arg);
                    if (nbThreads > 2048) {
                        DISPLAY("Invalid thread parameter, maximum is 2048\n");
                        return -1;
                    }
                    break;
                /* Set chunk size */
                case 'c':
                    arg++;
                    threadArgs.chunkSize = stringToU32(&arg);
                    break;
                /* Set iterations */
                case 'l':
                    arg++;
                    threadArgs.nbIterations = stringToU32(&arg);
                    break;
                /* Set benchmark mode */
                case 'm':
                    arg++;
                    threadArgs.benchMode = stringToU32(&arg);
                    break;
                /* Set searchForExternalRepcodes */
                case 'E':
                    arg++;
                    threadArgs.searchForExternalRepcodes = stringToU32(&arg);
                    if (threadArgs.searchForExternalRepcodes != ZSTD_AUTO &&
                        threadArgs.searchForExternalRepcodes != ZSTD_ENABLED &&
                        threadArgs.searchForExternalRepcodes != ZSTD_DISABLED) {
                        DISPLAY("Invalid searchForExternalRepcodes parameter\n");
                        return usage(argv[0]);
                    }
                    break;
                /* Set compression level */
                case 'L':
                    arg++;
                    threadArgs.cLevel = stringToU32(&arg);
                    break;
                /* Unknown argument */
                default :
                    return usage(argv[0]);
                }
            }
            continue;
        }
        if (!fileName) {
            fileName = arg;
            continue;
        }
    }
    if (!fileName) {
        return usage(argv[0]);
    }

    /* Load input file */
    inputFile = open(fileName, O_RDONLY);
    if (inputFile < 0) {
        DISPLAY("Cannot open input file: %s\n", fileName);
        return -1;
    }
    srcSize = lseek(inputFile, 0, SEEK_END);
    lseek(inputFile, 0, SEEK_SET);
    srcBuffer = (unsigned char *)malloc(srcSize);
    assert(srcBuffer != NULL);

    bytesRead = 0;
    while (bytesRead != srcSize) {
        bytesRead += read(inputFile, srcBuffer + bytesRead, srcSize - bytesRead);
    }
    assert(bytesRead == srcSize);

    threadArgs.srcBuffer = srcBuffer;
    threadArgs.srcSize = srcSize;
    initHistorgram(&compHistogram);
    initHistorgram(&decompHistogram);

    pthread_barrier_init(&g_threadBarrier1, NULL, nbThreads);
    pthread_barrier_init(&g_threadBarrier2, NULL, nbThreads);
    for (threadNb = 0; threadNb < nbThreads; threadNb++) {
        pthread_create(&threads[threadNb], NULL, benchmark, &threadArgs);
    }

    for (threadNb = 0; threadNb < nbThreads; threadNb++) {
        pthread_join(threads[threadNb], NULL);
    }

    if (compHistogram.num != 0) {
        /* Display Latency statistics */
        DISPLAY("-----------------------------------------------------------\n");
        DISPLAY("Latency Percentiles: P25: %4.2f us, P50: %4.2f us, P75: %4.2f us, P99: %4.2f us, Avg: %4.2f us\n",
                percentile(&compHistogram, 25) / NANOUSEC,
                percentile(&compHistogram, 50) / NANOUSEC,
                percentile(&compHistogram, 75) / NANOUSEC,
                percentile(&compHistogram, 99) / NANOUSEC,
                (double)(compHistogram.sum / compHistogram.num / NANOUSEC));

#ifdef DISPLAY_HISTOGRAM
        DISPLAY("Latency histogram(nanosec): count: %lu\n", compHistogram.num);
        size_t cumulativeSum = 0;
        for (int i = 0; i < compHistogram.bucketCount; i++) {
            if (compHistogram.bucket[i] != 0) {
                cumulativeSum += compHistogram.bucket[i];
                DISPLAY("[%10lu, %10lu] %10lu %7.2f%% %7.2f%%\n",
                        i == 0 ? 0 : compHistogram.bucketValue[i - 1], compHistogram.bucketValue[i],
                        compHistogram.bucket[i],
                        (double)compHistogram.bucket[i] * 100 / compHistogram.num,
                        (double)cumulativeSum * 100 / compHistogram.num);
            }
        }
#endif
    }

    if (decompHistogram.num != 0) {
        /* Display Latency statistics */
        DISPLAY("-----------------------------------------------------------\n");
        DISPLAY("Latency Percentiles: P25: %4.2f us, P50: %4.2f us, P75: %4.2f us, P99: %4.2f us, Avg: %4.2f us\n",
                percentile(&decompHistogram, 25) / NANOUSEC,
                percentile(&decompHistogram, 50) / NANOUSEC,
                percentile(&decompHistogram, 75) / NANOUSEC,
                percentile(&decompHistogram, 99) / NANOUSEC,
                (double)(decompHistogram.sum / decompHistogram.num / NANOUSEC));
    }

    pthread_barrier_destroy(&g_threadBarrier1);
    pthread_barrier_destroy(&g_threadBarrier2);
    if (threadArgs.benchMode == QAT_ZSTD_ENGINE) {
        QZSTD_stopQatDevice();
    }
    close(inputFile);
    free(srcBuffer);
    return 0;
}
