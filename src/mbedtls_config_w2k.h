/* mbedTLS 3.4 configuration for wmpmeta: a TLS 1.2 client only.
 *
 * Based on claude-win2k's config. Windows 2000 constraints:
 *   - No BCryptGenRandom (Vista+): platform entropy is off and tls.c adds a
 *     CryptGenRandom source instead.
 *   - msvcrt.dll has no gmtime_s/_gmtime64_s: tls.c supplies gmtime_r.
 *   - No getaddrinfo in ws2_32 (XP+): net_sockets.c is not built.
 *
 * TLS 1.3 is left out: in mbedTLS 3.4 it needs the PSA crypto core, which is
 * not thread-safe, and the proxy runs one thread per connection.
 *
 * Unlike claude-win2k this talks to arbitrary servers, so it also carries
 * plain-RSA key exchange and CBC suites for older ones.
 */
#ifndef TLSPROXY_MBEDTLS_CONFIG_H
#define TLSPROXY_MBEDTLS_CONFIG_H

/* System */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE
#define MBEDTLS_PLATFORM_C
#define MBEDTLS_PLATFORM_SNPRINTF_MACRO  snprintf
#define MBEDTLS_PLATFORM_VSNPRINTF_MACRO vsnprintf
#define MBEDTLS_PLATFORM_GMTIME_R_ALT
#define MBEDTLS_NO_PLATFORM_ENTROPY
#define MBEDTLS_DEPRECATED_REMOVED

/* Randomness */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* Symmetric crypto and hashes. AES_ROM_TABLES also avoids the lazy global
 * table setup, which would race between connection threads. */
#define MBEDTLS_AES_C
#define MBEDTLS_AES_ROM_TABLES
#define MBEDTLS_GCM_C
#define MBEDTLS_CIPHER_MODE_CBC
#define MBEDTLS_CHACHA20_C
#define MBEDTLS_POLY1305_C
#define MBEDTLS_CHACHAPOLY_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_MD_C
#define MBEDTLS_MD5_C
#define MBEDTLS_SHA1_C
#define MBEDTLS_SHA224_C
#define MBEDTLS_SHA256_C
#define MBEDTLS_SHA384_C
#define MBEDTLS_SHA512_C

/* Public key */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_OID_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_RSA_C
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PKCS1_V21
#define MBEDTLS_ECP_C
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED
#define MBEDTLS_ECP_DP_SECP521R1_ENABLED
#define MBEDTLS_ECP_DP_CURVE25519_ENABLED
#define MBEDTLS_ECP_NIST_OPTIM

/* Certificates */
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C

/* TLS */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_ENCRYPT_THEN_MAC
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_RSA_ENABLED

/* Servers may send full 16 KB records. */
#define MBEDTLS_SSL_IN_CONTENT_LEN  16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 16384

#define MBEDTLS_ERROR_C

#endif
