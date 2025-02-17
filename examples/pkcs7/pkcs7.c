/* pkcs7.c
 *
 * Copyright (C) 2006-2022 wolfSSL Inc.
 *
 * This file is part of wolfTPM.
 *
 * wolfTPM is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * wolfTPM is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1335, USA
 */


#include <wolftpm/tpm2.h>
#include <wolftpm/tpm2_wrap.h>

#include <stdio.h>

#if !defined(WOLFTPM2_NO_WRAPPER) && defined(WOLFTPM_CRYPTOCB) && \
    defined(HAVE_PKCS7)

#include <hal/tpm_io.h>
#include <examples/tpm_test.h>
#include <examples/tpm_test_keys.h>
#include <examples/pkcs7/pkcs7.h>
#include <wolfssl/wolfcrypt/pkcs7.h>

/* Sign PKCS7 using TPM based key:
 * Must Run:
 * 1. `./examples/csr/csr`
 * 2. `./certs/certreq.sh`
 * 3. Results in `./certs/client-rsa-cert.der`
 */

/* The PKCS7 EX functions were added after v3.15.3 */
#include <wolfssl/version.h>
#if defined(LIBWOLFSSL_VERSION_HEX) && \
    LIBWOLFSSL_VERSION_HEX > 0x03015003
    #undef  ENABLE_PKCS7EX_EXAMPLE
    #define ENABLE_PKCS7EX_EXAMPLE
#endif

#ifndef MAX_PKCS7_SIZE
#define MAX_PKCS7_SIZE MAX_CONTEXT_SIZE
#endif

/******************************************************************************/
/* --- BEGIN TPM2 PKCS7 Example -- */
/******************************************************************************/

#ifdef ENABLE_PKCS7EX_EXAMPLE
/* Dummy Function to Get Data */
#define MY_DATA_CHUNKS  WOLFTPM2_MAX_BUFFER
#define MY_DATA_TOTAL  (1024 * 1024) + 12 /* odd remainder for test */
static int GetMyData(byte* buffer, word32 bufSz, word32 offset)
{
    int i;
    const word32 myDataTotal = MY_DATA_TOTAL;

    /* way to return total size */
    if (buffer == NULL)
        return myDataTotal;

    /* check for overflow */
    if (offset >= myDataTotal)
        return 0;

    /* check for remainder */
    if (bufSz > myDataTotal - offset)
        bufSz = myDataTotal - offset;

    /* populate dummy data */
    for (i=0; i<(int)bufSz; i++) {
        buffer[i] = (i & 0xff);
        /* in real case would populate data here */
    }

    return bufSz;
}

/* The wc_PKCS7_EncodeSignedData_ex and wc_PKCS7_VerifySignedData_ex functions
   were added in this PR https://github.com/wolfSSL/wolfssl/pull/1780. */
static int PKCS7_SignVerifyEx(WOLFTPM2_DEV* dev, int tpmDevId, WOLFTPM2_BUFFER* der)
{
    int rc;
    PKCS7 pkcs7;
    wc_HashAlg       hash;
    enum wc_HashType hashType = WC_HASH_TYPE_SHA256;
    byte             hashBuf[TPM_SHA256_DIGEST_SIZE];
    word32           hashSz = wc_HashGetDigestSize(hashType);
    byte outputHead[MAX_PKCS7_SIZE], outputFoot[MAX_PKCS7_SIZE];
    int outputHeadSz, outputFootSz;
    byte dataChunk[MY_DATA_CHUNKS];
    word32 dataChunkSz, offset = 0;
#if !defined(NO_FILESYSTEM) && !defined(NO_WRITE_TEMP_FILES)
    XFILE pemFile;
#endif

    XMEMSET(&pkcs7, 0, sizeof(pkcs7));

    /* calculate hash for content */
    rc = wc_HashInit(&hash, hashType);
    if (rc == 0) {
        do {
            dataChunkSz = GetMyData(dataChunk, sizeof(dataChunk), offset);
            if (dataChunkSz == 0)
                break;

            rc = wc_HashUpdate(&hash, hashType, dataChunk, dataChunkSz);
            offset += dataChunkSz;
        } while (rc == 0);

        if (rc == 0) {
            rc = wc_HashFinal(&hash, hashType, hashBuf);
        }
        wc_HashFree(&hash, hashType);
    }
    if (rc != 0)
       goto exit;
    dataChunkSz = GetMyData(NULL, 0, 0); /* get total size */

    /* Generate and verify PKCS#7 files containing data using TPM key */
    rc = wc_PKCS7_Init(&pkcs7, NULL, tpmDevId);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_InitWithCert(&pkcs7, der->buffer, der->size);
    if (rc != 0) goto exit;

    pkcs7.content = NULL; /* not used */
    pkcs7.contentSz = dataChunkSz;
    pkcs7.encryptOID = RSAk;
    pkcs7.hashOID = SHA256h;
    pkcs7.rng = wolfTPM2_GetRng(dev);

    outputHeadSz = (int)sizeof(outputHead);
    outputFootSz = (int)sizeof(outputFoot);

    rc = wc_PKCS7_EncodeSignedData_ex(&pkcs7, hashBuf, hashSz,
        outputHead, (word32*)&outputHeadSz,
        outputFoot, (word32*)&outputFootSz);
    if (rc != 0) goto exit;

    wc_PKCS7_Free(&pkcs7);

    printf("PKCS7 Header %d\n", outputHeadSz);
    TPM2_PrintBin(outputHead, outputHeadSz);

    printf("PKCS7 Footer %d\n", outputFootSz);
    TPM2_PrintBin(outputFoot, outputFootSz);

#if !defined(NO_FILESYSTEM) && !defined(NO_WRITE_TEMP_FILES)
    pemFile = XFOPEN("./examples/pkcs7/pkcs7tpmsignedex.p7s", "wb");
    if (pemFile != XBADFILE) {

        /* Header */
        rc = (int)XFWRITE(outputHead, 1, outputHeadSz, pemFile);
        if (rc != outputHeadSz) {
            XFCLOSE(pemFile);
            rc = -1; goto exit;
        }

        /* Body - Data */
        do {
            dataChunkSz = GetMyData(dataChunk, sizeof(dataChunk), offset);
            if (dataChunkSz == 0)
                break;

            rc = (int)XFWRITE(dataChunk, 1, dataChunkSz, pemFile);
            if (rc != (int)dataChunkSz) {
                XFCLOSE(pemFile);
                rc = -1; goto exit;
            }

            offset += dataChunkSz;
        } while (rc == 0);
        dataChunkSz = GetMyData(NULL, 0, 0); /* get total size */

        /* Footer */
        rc = (int)XFWRITE(outputFoot, 1, outputFootSz, pemFile);
        if (rc != outputFootSz) {
            XFCLOSE(pemFile);
            rc = -1; goto exit;
        }

        XFCLOSE(pemFile);
    }
#endif

    /* Test verify with TPM */
    rc = wc_PKCS7_Init(&pkcs7, NULL, tpmDevId);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_InitWithCert(&pkcs7, NULL, 0);
    if (rc != 0) goto exit;

    pkcs7.contentSz = dataChunkSz;
    rc = wc_PKCS7_VerifySignedData_ex(&pkcs7, hashBuf, hashSz,
        outputHead, outputHeadSz, outputFoot, outputFootSz);
    if (rc != 0) goto exit;

    wc_PKCS7_Free(&pkcs7);

    printf("PKCS7 Container Verified (using TPM)\n");

    /* Test verify with software */
    rc = wc_PKCS7_Init(&pkcs7, NULL, INVALID_DEVID);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_InitWithCert(&pkcs7, NULL, 0);
    if (rc != 0) goto exit;
    pkcs7.contentSz = dataChunkSz;
    rc = wc_PKCS7_VerifySignedData_ex(&pkcs7, hashBuf, hashSz,
        outputHead, outputHeadSz, outputFoot, outputFootSz);
    if (rc != 0) goto exit;
    wc_PKCS7_Free(&pkcs7);

    printf("PKCS7 Container Verified (using software)\n");

exit:
    return rc;
}
#endif /* ENABLE_PKCS7EX_EXAMPLE */

static int PKCS7_SignVerify(WOLFTPM2_DEV* dev, int tpmDevId, WOLFTPM2_BUFFER* der)
{
    int rc;
    PKCS7 pkcs7;
    byte  data[] = "My encoded DER cert.";
    byte output[MAX_PKCS7_SIZE];
    int outputSz;
#if !defined(NO_FILESYSTEM) && !defined(NO_WRITE_TEMP_FILES)
    XFILE pemFile;
#endif

    XMEMSET(&pkcs7, 0, sizeof(pkcs7));

    /* Generate and verify PKCS#7 files containing data using TPM key */
    rc = wc_PKCS7_Init(&pkcs7, NULL, tpmDevId);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_InitWithCert(&pkcs7, der->buffer, der->size);
    if (rc != 0) goto exit;

    pkcs7.content = data;
    pkcs7.contentSz = (word32)sizeof(data);
    pkcs7.encryptOID = RSAk;
    pkcs7.hashOID = SHA256h;
    pkcs7.rng = wolfTPM2_GetRng(dev);

    rc = wc_PKCS7_EncodeSignedData(&pkcs7, output, sizeof(output));
    if (rc <= 0) goto exit;
    wc_PKCS7_Free(&pkcs7);
    outputSz = rc;

    printf("PKCS7 Signed Container %d\n", outputSz);
    TPM2_PrintBin(output, outputSz);

#if !defined(NO_FILESYSTEM) && !defined(NO_WRITE_TEMP_FILES)
    pemFile = XFOPEN("./examples/pkcs7/pkcs7tpmsigned.p7s", "wb");
    if (pemFile != XBADFILE) {
        rc = (int)XFWRITE(output, 1, outputSz, pemFile);
        XFCLOSE(pemFile);
        if (rc != outputSz) {
            rc = -1; goto exit;
        }
    }
#endif

    /* Test verify with TPM */
    rc = wc_PKCS7_Init(&pkcs7, NULL, tpmDevId);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_InitWithCert(&pkcs7, NULL, 0);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_VerifySignedData(&pkcs7, output, outputSz);
    if (rc != 0) goto exit;
    wc_PKCS7_Free(&pkcs7);

    printf("PKCS7 Container Verified (using TPM)\n");

    /* Test verify with software */
    rc = wc_PKCS7_Init(&pkcs7, NULL, INVALID_DEVID);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_InitWithCert(&pkcs7, NULL, 0);
    if (rc != 0) goto exit;
    rc = wc_PKCS7_VerifySignedData(&pkcs7, output, outputSz);
    if (rc != 0) goto exit;
    wc_PKCS7_Free(&pkcs7);

    printf("PKCS7 Container Verified (using software)\n");

exit:
    return rc;
}

int TPM2_PKCS7_Example(void* userCtx)
{
    return TPM2_PKCS7_ExampleArgs(userCtx, 0, NULL);
}
int TPM2_PKCS7_ExampleArgs(void* userCtx, int argc, char *argv[])
{
    int rc;
    WOLFTPM2_DEV dev;
    WOLFTPM2_KEY storageKey;
    WOLFTPM2_KEY rsaKey;
    TPMT_PUBLIC publicTemplate;
    TpmCryptoDevCtx tpmCtx;
    int tpmDevId;
    WOLFTPM2_BUFFER der;
#if !defined(NO_FILESYSTEM) && !defined(NO_WRITE_TEMP_FILES)
    XFILE derFile;
#endif

    (void)argc;
    (void)argv;

    printf("TPM2 PKCS7 Example\n");

    XMEMSET(&der, 0, sizeof(der));
    XMEMSET(&rsaKey, 0, sizeof(rsaKey));
    XMEMSET(&storageKey, 0, sizeof(storageKey));

    /* Init the TPM2 device */
    rc = wolfTPM2_Init(&dev, TPM2_IoCb, userCtx);
    if (rc != 0) return rc;

    /* Setup the wolf crypto device callback */
    XMEMSET(&tpmCtx, 0, sizeof(tpmCtx));
#ifndef NO_RSA
    tpmCtx.rsaKey = &rsaKey;
#endif
    rc = wolfTPM2_SetCryptoDevCb(&dev, wolfTPM2_CryptoDevCb, &tpmCtx, &tpmDevId);
    if (rc < 0) goto exit;

    /* get SRK */
    rc = getPrimaryStoragekey(&dev, &storageKey, TPM_ALG_RSA);
    if (rc != 0) goto exit;

    /* Create/Load RSA key for PKCS7 signing */
    rc = wolfTPM2_GetKeyTemplate_RSA(&publicTemplate,
                    TPMA_OBJECT_sensitiveDataOrigin | TPMA_OBJECT_userWithAuth |
                    TPMA_OBJECT_decrypt | TPMA_OBJECT_sign | TPMA_OBJECT_noDA);
    if (rc != 0) goto exit;

    rc = getRSAkey(&dev,
                   &storageKey,
                   &rsaKey,
                   NULL,
                   tpmDevId,
                   (byte*)gKeyAuth, sizeof(gKeyAuth)-1,
                   &publicTemplate);
    if (rc != 0) goto exit;
    wolfTPM2_SetAuthHandle(&dev, 0, &rsaKey.handle);


    /* load DER certificate for TPM key (obtained by running
        `./examples/csr/csr` and `./certs/certreq.sh`) */
#if !defined(NO_FILESYSTEM) && !defined(NO_WRITE_TEMP_FILES)
    derFile = XFOPEN("./certs/client-rsa-cert.der", "rb");
    if (derFile != XBADFILE) {
        XFSEEK(derFile, 0, XSEEK_END);
        der.size = (int)XFTELL(derFile);
        XREWIND(derFile);
        if (der.size > (int)sizeof(der.buffer)) {
            rc = BUFFER_E;
        }
        else {
            rc = (int)XFREAD(der.buffer, 1, der.size, derFile);
            rc = (rc == der.size) ? 0 : -1;
        }
        XFCLOSE(derFile);
        if (rc != 0) goto exit;
    }
#endif


    /* PKCS 7 sign/verify example */
    rc = PKCS7_SignVerify(&dev, tpmDevId, &der);
    if (rc != 0) goto exit;

#ifdef ENABLE_PKCS7EX_EXAMPLE
    /* PKCS 7 large data sign/verify example */
    rc = PKCS7_SignVerifyEx(&dev, tpmDevId, &der);
    if (rc != 0) goto exit;
#endif

exit:

    if (rc != 0) {
        printf("Failure 0x%x: %s\n", rc, wolfTPM2_GetRCString(rc));
    }

    wolfTPM2_UnloadHandle(&dev, &rsaKey.handle);

    wolfTPM2_Cleanup(&dev);

    return rc;
}

/******************************************************************************/
/* --- END TPM2 PKCS7 Example -- */
/******************************************************************************/

#endif /* !WOLFTPM2_NO_WRAPPER && WOLFTPM_CRYPTOCB && HAVE_PKCS7 */

#ifndef NO_MAIN_DRIVER
int main(int argc, char *argv[])
{
    int rc = -1;

#if !defined(WOLFTPM2_NO_WRAPPER) && defined(WOLFTPM_CRYPTOCB) && \
    defined(HAVE_PKCS7)
    rc = TPM2_PKCS7_ExampleArgs(NULL, argc, argv);
#else
    (void)argc;
    (void)argv;

    printf("Wrapper/PKCS7/CryptoDev code not compiled in\n");
    printf("Build wolfssl with ./configure --enable-pkcs7 --enable-cryptocb\n");
#endif

    return rc;
}
#endif /* !NO_MAIN_DRIVER */
