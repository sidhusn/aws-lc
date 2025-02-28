// Copyright Amazon.com, Inc. or its affiliates. All Rights Reserved.
// SPDX-License-Identifier: Apache-2.0

#include <openssl/crypto.h>
#include <openssl/service_indicator.h>
#include "internal.h"

const char* awslc_version_string(void) {
  return AWSLC_VERSION_STRING;
}

int is_fips_build(void) {
#if defined(AWSLC_FIPS)
  return 1;
#else
  return 0;
#endif
}

#if defined(AWSLC_FIPS)

#define STATE_UNLOCKED 0

// fips_service_indicator_state is a thread-local structure that stores the
// state of the FIPS service indicator.
struct fips_service_indicator_state {
  // lock_state records the number of times the indicator has been locked.
  // When it is zero (i.e. |STATE_UNLOCKED|) then the indicator can be updated.
  uint64_t lock_state;
  // counter is the indicator state. It is incremented when an approved service
  // completes.
  uint64_t counter;
};

// service_indicator_get returns a pointer to the |fips_service_indicator_state|
// for the current thread. It returns NULL on error.
//
// FIPS 140-3 requires that the module should provide the service indicator
// for approved services irrespective of whether the user queries it or not.
// Hence, it is lazily initialized in any call to an approved service.
static struct fips_service_indicator_state * service_indicator_get(void) {
  struct fips_service_indicator_state *indicator = CRYPTO_get_thread_local(
      AWSLC_THREAD_LOCAL_FIPS_SERVICE_INDICATOR_STATE);

  if (indicator == NULL) {
    indicator = OPENSSL_malloc(sizeof(struct fips_service_indicator_state));
    if (indicator == NULL) {
      OPENSSL_PUT_ERROR(CRYPTO, ERR_R_MALLOC_FAILURE);
      return NULL;
    }

    indicator->lock_state = STATE_UNLOCKED;
    indicator->counter = 0;

    if (!CRYPTO_set_thread_local(
            AWSLC_THREAD_LOCAL_FIPS_SERVICE_INDICATOR_STATE, indicator,
            OPENSSL_free)) {
      OPENSSL_PUT_ERROR(CRYPTO, ERR_R_INTERNAL_ERROR);
      return NULL;
    }
  }

  return indicator;
}

static uint64_t service_indicator_get_counter(void) {
  struct fips_service_indicator_state *indicator = service_indicator_get();
  if (indicator == NULL) {
    return 0;
  }
  return indicator->counter;
}

uint64_t FIPS_service_indicator_before_call(void) {
  return service_indicator_get_counter();
}

uint64_t FIPS_service_indicator_after_call(void) {
  return service_indicator_get_counter();
}

void FIPS_service_indicator_update_state(void) {
  struct fips_service_indicator_state *indicator = service_indicator_get();
  if (indicator && indicator->lock_state == STATE_UNLOCKED) {
    indicator->counter++;
  }
}

// |FIPS_service_indicator_lock_state| and |FIPS_service_indicator_unlock_state|
// should not under/overflow in normal operation. They are still checked and
// errors added to facilitate testing in service_indicator_test.cc This should
// only happen if lock/unlock are called in an incorrect order or multiple times
// in the same function.
void FIPS_service_indicator_lock_state(void) {
  struct fips_service_indicator_state *indicator = service_indicator_get();
  if (indicator == NULL) {
    return;
  }

  // |FIPS_service_indicator_lock_state| and
  // |FIPS_service_indicator_unlock_state| should not under/overflow in normal
  // operation. They are still checked and errors added to facilitate testing in
  // service_indicator_test.cc. This should only happen if lock/unlock are
  // called in an incorrect order or multiple times in the same function.
  const uint64_t new_state = indicator->lock_state + 1;
  if (new_state < indicator->lock_state) {
    // Overflow. This would imply that our call stack length has exceeded a
    // |uint64_t| which impossible on a 64-bit system.
    abort();
  }

  indicator->lock_state = new_state;
}

void FIPS_service_indicator_unlock_state(void) {
  struct fips_service_indicator_state *indicator = service_indicator_get();
  if (indicator == NULL) {
    return;
  }

  if (indicator->lock_state == 0) {
    abort();
  }

  indicator->lock_state--;
}

void AEAD_GCM_verify_service_indicator(const EVP_AEAD_CTX *ctx) {
  // We only have support for 128 bit and 256 bit keys for AES-GCM. AES-GCM is
  // approved only with an internal IV, see SP 800-38D Sec 8.2.2.
  // Note: |EVP_AEAD_key_length| returns the length in bytes.
  const size_t key_len = EVP_AEAD_key_length(ctx->aead);
  if (key_len == 16 || key_len == 32) {
    FIPS_service_indicator_update_state();
  }
}

void AEAD_CCM_verify_service_indicator(const EVP_AEAD_CTX *ctx) {
  // Only 128 bit keys with 32 bit tag lengths are approved for AES-CCM.
  // Note: |EVP_AEAD_key_length| returns the length in bytes.
  if (EVP_AEAD_key_length(ctx->aead) == 16 && ctx->tag_len == 4) {
    FIPS_service_indicator_update_state();
  }
}

void AES_CMAC_verify_service_indicator(const CMAC_CTX *ctx) {
  // Only 128 and 256 bit keys are approved for AES-CMAC.
  // Note: |key_len| is the length in bytes.
  const size_t key_len = ctx->cipher_ctx.key_len;
  if (key_len == 16 || key_len == 32) {
    FIPS_service_indicator_update_state();
  }
}

// is_ec_fips_approved returns one if the curve corresponding to the given NID
// is FIPS approved, and zero otherwise.
static int is_ec_fips_approved(int curve_nid) {
  switch (curve_nid) {
    case NID_secp224r1:
    case NID_X9_62_prime256v1:
    case NID_secp384r1:
    case NID_secp521r1:
      return 1;
    default:
      return 0;
  }
}

// is_md_fips_approved_for_signing returns one if the given message digest type
// is FIPS approved for signing, and zero otherwise.
// TODO (CryptoAlg-1212): FIPS validate SHA512/256 for signing.
static int is_md_fips_approved_for_signing(int md_type) {
  switch (md_type) {
    case NID_sha224:
    case NID_sha256:
    case NID_sha384:
    case NID_sha512:
      return 1;
    default:
      return 0;
  }
}

// is_md_fips_approved_for_verifying returns one if the given message digest
// type is FIPS approved for verifying, and zero otherwise.
// TODO (CryptoAlg-1212): FIPS validate SHA512/256 for verifying.
static int is_md_fips_approved_for_verifying(int md_type) {
  switch (md_type) {
    case NID_sha1:
    case NID_sha224:
    case NID_sha256:
    case NID_sha384:
    case NID_sha512:
      return 1;
    default:
      return 0;
  }
}

static void evp_md_ctx_verify_service_indicator(const EVP_MD_CTX *ctx,
                                                int rsa_1024_ok,
                                                int (*md_ok)(int md_type)) {
  if (EVP_MD_CTX_md(ctx) == NULL) {
    // Signature schemes without a prehash are currently never FIPS approved.
    goto err;
  }

  EVP_PKEY_CTX *const pctx = ctx->pctx;
  const EVP_PKEY *const pkey = EVP_PKEY_CTX_get0_pkey(pctx);
  const int pkey_type = EVP_PKEY_id(pkey);
  const int md_type = EVP_MD_CTX_type(ctx);

  if (pkey_type == EVP_PKEY_RSA || pkey_type == EVP_PKEY_RSA_PSS) {
    // Message digest used in the private key should be of the same type
    // as the given one, so we extract the MD type from the |EVP_PKEY|
    // and compare it with the type in |ctx|.
    const EVP_MD *pctx_md;
    if (!EVP_PKEY_CTX_get_signature_md(pctx, &pctx_md)) {
      goto err;
    }
    if (EVP_MD_type(pctx_md) != md_type) {
      goto err;
    }

    int padding;
    if (!EVP_PKEY_CTX_get_rsa_padding(pctx, &padding)) {
      goto err;
    }
    if (padding == RSA_PKCS1_PSS_PADDING) {
      int salt_len;
      const EVP_MD *mgf1_md;
      if (!EVP_PKEY_CTX_get_rsa_pss_saltlen(pctx, &salt_len) ||
          !EVP_PKEY_CTX_get_rsa_mgf1_md(pctx, &mgf1_md) ||
          (salt_len != -1 && salt_len != (int)EVP_MD_size(pctx_md)) ||
          EVP_MD_type(mgf1_md) != md_type) {
        // Only PSS where saltLen == hashLen is tested with ACVP. Cases with
        // non-standard padding functions are also excluded.
        goto err;
      }
    }

    // The approved RSA key sizes for signing are 2048, 3072 and 4096 bits.
    // Note: |EVP_PKEY_size| returns the size in bytes.
    size_t pkey_size = EVP_PKEY_size(ctx->pctx->pkey);

    // Check if the MD type and the RSA key size are approved.
    if (md_ok(md_type) &&
        ((rsa_1024_ok && pkey_size == 128) || pkey_size == 256 ||
         pkey_size == 384 || pkey_size == 512)) {
      FIPS_service_indicator_update_state();
    }
  } else if (pkey_type == EVP_PKEY_EC) {
    // Check if the MD type and the elliptic curve are approved.
    int curve_nid = EC_GROUP_get_curve_name(pkey->pkey.ec->group);
    if (md_ok(md_type) && is_ec_fips_approved(curve_nid)) {
      FIPS_service_indicator_update_state();
    }
  }

 err:
  // Ensure that junk errors aren't left on the queue.
  ERR_clear_error();
}

// Updates the service indicator if the elliptic curve contained in |eckey|
// is FIPS approved.
void EC_KEY_keygen_verify_service_indicator(const EC_KEY *eckey) {
  if (is_ec_fips_approved(EC_GROUP_get_curve_name(eckey->group))) {
    FIPS_service_indicator_update_state();
  }
}

void ECDH_verify_service_indicator(const EC_KEY *ec_key) {
  if (is_ec_fips_approved(EC_GROUP_get_curve_name(EC_KEY_get0_group(ec_key)))) {
    FIPS_service_indicator_update_state();
  }
}

void EVP_PKEY_keygen_verify_service_indicator(const EVP_PKEY *pkey) {
  if (pkey->type == EVP_PKEY_RSA || pkey->type== EVP_PKEY_RSA_PSS) {
    // 2048, 3072 and 4096 bit keys are approved for RSA key generation.
    // EVP_PKEY_size returns the size of the key in bytes.
    // Note: |EVP_PKEY_size| returns the length in bytes.
    size_t key_size = EVP_PKEY_size(pkey);
    switch (key_size) {
      case 256:
      case 384:
      case 512:
        FIPS_service_indicator_update_state();
        break;
      default:
        break;
    }
  } else if (pkey->type == EVP_PKEY_EC) {
    // Note: even though the function is called |EC_GROUP_get_curve_name|
    // it actually returns the NID of the curve rather than the name.
    int curve_nid = EC_GROUP_get_curve_name(pkey->pkey.ec->group);
    if (is_ec_fips_approved(curve_nid)) {
      FIPS_service_indicator_update_state();
    }
  }
}

void EVP_Cipher_verify_service_indicator(const EVP_CIPHER_CTX *ctx) {
  switch (EVP_CIPHER_CTX_nid(ctx)) {
    case NID_aes_128_ecb:
    case NID_aes_192_ecb:
    case NID_aes_256_ecb:

    case NID_aes_128_cbc:
    case NID_aes_192_cbc:
    case NID_aes_256_cbc:

    case NID_aes_128_ctr:
    case NID_aes_192_ctr:
    case NID_aes_256_ctr:
      FIPS_service_indicator_update_state();
  }
}

void EVP_DigestVerify_verify_service_indicator(const EVP_MD_CTX *ctx) {
  evp_md_ctx_verify_service_indicator(ctx, /*rsa_1024_ok=*/1,
                                      is_md_fips_approved_for_verifying);
}

void EVP_DigestSign_verify_service_indicator(const EVP_MD_CTX *ctx) {
  evp_md_ctx_verify_service_indicator(ctx, /*rsa_1024_ok=*/0,
                                      is_md_fips_approved_for_signing);
}

// TODO (CryptoAlg-1212): FIPS validate SHA512/256 for HMAC.
void HMAC_verify_service_indicator(const EVP_MD *evp_md) {
  // HMAC with SHA1, SHA224, SHA256, SHA384, and SHA512 are approved.
  switch (evp_md->type){
    case NID_sha1:
    case NID_sha224:
    case NID_sha256:
    case NID_sha384:
    case NID_sha512:
      FIPS_service_indicator_update_state();
      break;
    default:
      break;
  }
}

void TLSKDF_verify_service_indicator(const EVP_MD *dgst) {
  // HMAC-MD5, HMAC-SHA1, and HMAC-MD5/HMAC-SHA1 (both used concurrently) are
  // approved for use in the KDF in TLS 1.0/1.1.
  // HMAC-SHA{256, 384, 512} are approved for use in the KDF in TLS 1.2.
  // These Key Derivation functions are to be used in the context of the TLS
  // protocol.
  switch (dgst->type) {
    case NID_md5:
    case NID_sha1:
    case NID_md5_sha1:
    case NID_sha256:
    case NID_sha384:
    case NID_sha512:
      FIPS_service_indicator_update_state();
      break;
    default:
      break;
  }
}

#else

uint64_t FIPS_service_indicator_before_call(void) { return 0; }

uint64_t FIPS_service_indicator_after_call(void) {
  // One is returned so that the return value is always greater than zero, the
  // return value of |FIPS_service_indicator_before_call|. This makes everything
  // report as "approved" in non-FIPS builds.
  return 1;
}

#endif // AWSLC_FIPS
