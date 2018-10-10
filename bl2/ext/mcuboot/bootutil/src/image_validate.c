/*
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing,
 * software distributed under the License is distributed on an
 * "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
 * KIND, either express or implied.  See the License for the
 * specific language governing permissions and limitations
 * under the License.
 */

/*
 * Original code taken from mcuboot project at:
 * https://github.com/JuulLabs-OSS/mcuboot
 * Git SHA of the original version: 178be54bd6e5f035cc60e98205535682acd26e64
 * Modifications are Copyright (c) 2018-2019 Arm Limited.
 */

#include <assert.h>
#include <stddef.h>
#include <inttypes.h>
#include <string.h>

#include "flash_map/flash_map.h"
#include "bootutil/image.h"
#include "bootutil/sha256.h"
#include "bootutil/sign_key.h"

#ifdef MCUBOOT_SIGN_RSA
#include "mbedtls/rsa.h"
#endif

#include "mbedtls/asn1.h"

#include "bootutil_priv.h"

/*
 * Compute SHA256 over the image.
 */
static int
bootutil_img_hash(struct image_header *hdr, const struct flash_area *fap,
                  uint8_t *tmp_buf, uint32_t tmp_buf_sz,
                  uint8_t *hash_result, uint8_t *seed, int seed_len)
{
    bootutil_sha256_context sha256_ctx;
    uint32_t blk_sz;
    uint32_t size;
    uint32_t off;

    bootutil_sha256_init(&sha256_ctx);

    /* in some cases (split image) the hash is seeded with data from
     * the loader image */
    if (seed && (seed_len > 0)) {
        bootutil_sha256_update(&sha256_ctx, seed, seed_len);
    }

    /*
     * Hash is computed over image header and image itself. No TLV is
     * included ATM.
     */
    size = hdr->ih_img_size + hdr->ih_hdr_size;
    for (off = 0; off < size; off += blk_sz) {
        blk_sz = size - off;
        if (blk_sz > tmp_buf_sz) {
            blk_sz = tmp_buf_sz;
        }

#ifdef MCUBOOT_RAM_LOADING
        if (fap == NULL) { /* The image is in SRAM */
            memcpy(tmp_buf, (uint32_t *)(hdr->ih_load_addr + off), blk_sz);
        } else { /* The image is in flash */
#endif
            if(flash_area_read(fap, off, tmp_buf, blk_sz)) {
                return -1;
            }
#ifdef MCUBOOT_RAM_LOADING
        }
#endif

        bootutil_sha256_update(&sha256_ctx, tmp_buf, blk_sz);
    }
    bootutil_sha256_finish(&sha256_ctx, hash_result);

    return 0;
}

/*
 * Currently, we only support being able to verify one type of
 * signature, because there is a single verification function that we
 * call.  List the type of TLV we are expecting.  If we aren't
 * configured for any signature, don't define this macro.
 */
#if defined(MCUBOOT_SIGN_RSA)
#    define EXPECTED_SIG_TLV IMAGE_TLV_RSA2048_PSS
#    define EXPECTED_SIG_LEN(x) ((x) == 256) /* 2048 bits */
#endif

#ifdef EXPECTED_SIG_TLV
static int
bootutil_find_key(uint8_t *keyhash, uint8_t keyhash_len)
{
    bootutil_sha256_context sha256_ctx;
    int i;
    const struct bootutil_key *key;
    uint8_t hash[32];

    assert(keyhash_len <= 32);

    for (i = 0; i < bootutil_key_cnt; i++) {
        key = &bootutil_keys[i];
        bootutil_sha256_init(&sha256_ctx);
        bootutil_sha256_update(&sha256_ctx, key->key, *key->len);
        bootutil_sha256_finish(&sha256_ctx, hash);
        if (!memcmp(hash, keyhash, keyhash_len)) {
            return i;
        }
    }
    return -1;
}
#endif

#ifdef MCUBOOT_RAM_LOADING
/* Check the hash of an image after it has been copied to SRAM */
int
bootutil_check_hash_after_loading(struct image_header *hdr)
{
    uint32_t off;
    uint32_t end;
    int sha256_valid = 0;
    struct image_tlv_info info;
    struct image_tlv tlv;
    uint8_t tmp_buf[BOOT_TMPBUF_SZ];
    uint8_t hash[32] = {0};
    int rc;
    uint32_t load_address;
    uint32_t tlv_sz;

    rc = bootutil_img_hash(hdr, NULL, tmp_buf, BOOT_TMPBUF_SZ, hash, NULL, 0);

    if (rc) {
        return rc;
    }

    load_address = (uint32_t) hdr->ih_load_addr;

    /* The TLVs come after the image. */
    off = hdr->ih_img_size + hdr->ih_hdr_size;

    info = *((struct image_tlv_info *)(load_address + off));

    if (info.it_magic != IMAGE_TLV_INFO_MAGIC) {
        return -1;
    }
    end = off + info.it_tlv_tot;
    off += sizeof(info);

    /*
     * Traverse through all of the TLVs, performing any checks we know
     * and are able to do.
     */
    for (; off < end; off += sizeof(tlv) + tlv.it_len) {
        tlv = *((struct image_tlv *)(load_address + off));
        tlv_sz = sizeof(tlv);

        if (tlv.it_type == IMAGE_TLV_SHA256) {
            /*
             * Verify the SHA256 image hash. This must always be present.
             */
            if (tlv.it_len != sizeof(hash)) {
                return -1;
            }

            if (memcmp(hash, (uint32_t *)(load_address + off + tlv_sz),
                       sizeof(hash))) {
                return -1;
            }

            sha256_valid = 1;
        }
    }

    if (!sha256_valid) {
        return -1;
    }

    return 0;
}
#endif /* MCUBOOT_RAM_LOADING */

/*
 * Verify the integrity of the image.
 * Return non-zero if image could not be validated/does not validate.
 */
int
bootutil_img_validate(struct image_header *hdr, const struct flash_area *fap,
                      uint8_t *tmp_buf, uint32_t tmp_buf_sz,
                      uint8_t *seed, int seed_len, uint8_t *out_hash)
{
    uint32_t off;
    uint32_t end;
    int sha256_valid = 0;
    struct image_tlv_info info;
#ifdef EXPECTED_SIG_TLV
    int valid_signature = 0;
    int key_id = -1;
#endif
    struct image_tlv tlv;
    uint8_t buf[256];
    uint8_t hash[32] = {0};
    int rc;

    rc = bootutil_img_hash(hdr, fap, tmp_buf, tmp_buf_sz, hash,
                           seed, seed_len);
    if (rc) {
        return rc;
    }

    if (out_hash) {
        memcpy(out_hash, hash, 32);
    }

    /* The TLVs come after the image. */
    /* After image there are TLVs. */
    off = hdr->ih_img_size + hdr->ih_hdr_size;

    rc = flash_area_read(fap, off, &info, sizeof(info));
    if (rc) {
        return rc;
    }
    if (info.it_magic != IMAGE_TLV_INFO_MAGIC) {
        return -1;
    }
    end = off + info.it_tlv_tot;
    off += sizeof(info);

    /*
     * Traverse through all of the TLVs, performing any checks we know
     * and are able to do.
     */
    for (; off < end; off += sizeof(tlv) + tlv.it_len) {
        rc = flash_area_read(fap, off, &tlv, sizeof(tlv));
        if (rc) {
            return rc;
        }

        if (tlv.it_type == IMAGE_TLV_SHA256) {
            /*
             * Verify the SHA256 image hash.  This must always be
             * present.
             */
            if (tlv.it_len != sizeof(hash)) {
                return -1;
            }
            rc = flash_area_read(fap, off + sizeof(tlv), buf, sizeof(hash));
            if (rc) {
                return rc;
            }
            if (memcmp(hash, buf, sizeof(hash))) {
                return -1;
            }

            sha256_valid = 1;
#ifdef EXPECTED_SIG_TLV
        } else if (tlv.it_type == IMAGE_TLV_KEYHASH) {
            /*
             * Determine which key we should be checking.
             */
            if (tlv.it_len > 32) {
                return -1;
            }
            rc = flash_area_read(fap, off + sizeof(tlv), buf, tlv.it_len);
            if (rc) {
                return rc;
            }
            key_id = bootutil_find_key(buf, tlv.it_len);
            /*
             * The key may not be found, which is acceptable.  There
             * can be multiple signatures, each preceded by a key.
             */
        } else if (tlv.it_type == EXPECTED_SIG_TLV) {
            /* Ignore this signature if it is out of bounds. */
            if (key_id < 0 || key_id >= bootutil_key_cnt) {
                key_id = -1;
                continue;
            }
            if (!EXPECTED_SIG_LEN(tlv.it_len) || tlv.it_len > sizeof(buf)) {
                return -1;
            }
            rc = flash_area_read(fap, off + sizeof(tlv), buf, tlv.it_len);
            if (rc) {
                return -1;
            }
            rc = bootutil_verify_sig(hash, sizeof(hash), buf, tlv.it_len,
                                     key_id);
            if (rc == 0) {
                valid_signature = 1;
            }
            key_id = -1;
#endif
        }
    }

    if (!sha256_valid) {
        return -1;
    }

#ifdef EXPECTED_SIG_TLV
    if (!valid_signature) {
        return -1;
    }
#endif

    return 0;
}
