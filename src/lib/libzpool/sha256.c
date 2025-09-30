/*
 * CDDL HEADER START
 *
 * The contents of this file are subject to the terms of the
 * Common Development and Distribution License (the "License").
 * You may not use this file except in compliance with the License.
 *
 * You can obtain a copy of the license at usr/src/OPENSOLARIS.LICENSE
 * or http://www.opensolaris.org/os/licensing.
 * See the License for the specific language governing permissions
 * and limitations under the License.
 *
 * When distributing Covered Code, include this CDDL HEADER in each
 * file and include the License file at usr/src/OPENSOLARIS.LICENSE.
 * If applicable, add the following below this CDDL HEADER, with the
 * fields enclosed by brackets "[]" replaced with your own identifying
 * information: Portions Copyright [yyyy] [name of copyright owner]
 *
 * CDDL HEADER END
 */
/*
 * Copyright 2009 Sun Microsystems, Inc.  All rights reserved.
 * Use is subject to license terms.
 */
#include <sys/zfs_context.h>
#include <sys/zio.h>
#include <openssl/sha.h>
#include <openssl/evp.h>
#include <openssl/opensslv.h>

#if OPENSSL_VERSION_NUMBER < 0x10100000L || defined(LIBRESSL_VERSION_NUMBER)
#define EVP_MD_CTX_new	EVP_MD_CTX_create
#define EVP_MD_CTX_free	EVP_MD_CTX_destroy
#endif

void
zio_checksum_SHA256(const void *buf, uint64_t size, zio_cksum_t *zcp)
{
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned int digest_len = 0;
    zio_cksum_t tmp;

    VERIFY(mdctx != NULL);
    VERIFY3S(EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL), ==, 1);
    VERIFY3S(EVP_DigestUpdate(mdctx, buf, size), ==, 1);
    VERIFY3S(EVP_DigestFinal_ex(mdctx, digest, &digest_len), ==, 1);
    EVP_MD_CTX_free(mdctx);

    ASSERT3U(digest_len, ==, SHA256_DIGEST_LENGTH);
    bcopy(digest, &tmp, sizeof (tmp));

    /*
     * A prior implementation of this function had a
     * private SHA256 implementation always wrote things out in
     * Big Endian and there wasn't a byteswap variant of it.
     * To preseve on disk compatibility we need to force that
     * behaviour.
     */
    zcp->zc_word[0] = BE_64(tmp.zc_word[0]);
    zcp->zc_word[1] = BE_64(tmp.zc_word[1]);
    zcp->zc_word[2] = BE_64(tmp.zc_word[2]);
    zcp->zc_word[3] = BE_64(tmp.zc_word[3]);
}
