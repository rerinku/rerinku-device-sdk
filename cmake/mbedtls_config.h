/* mbedTLS configuration for the rerinku SDK: exactly what it uses, nothing
 * else (replaces mbedTLS's default configuration via MBEDTLS_CONFIG_FILE).
 *
 *   DTLS 1.2, client and server role   WebRTC DTLS-SRTP keying with the SFU
 *   TLS 1.2 client                     HTTPS (registration, WHIP, snapshot)
 *                                      and MQTT over TLS
 *   ECDSA P-256 key + self-signed cert the DTLS certificate (fingerprint auth)
 *   RSA / ECDSA certificate chains     verifying HTTPS/MQTTS servers
 *
 * Left out on purpose: TLS 1.3 (and with it the PSA crypto core), CBC/CCM/
 * ChaCha suites, PSK, SHA-1/MD5, session tickets, renegotiation, the error
 * string table. Every server and SFU the SDK talks to negotiates
 * ECDHE + AES-GCM over TLS/DTLS 1.2. */
#ifndef RERINKU_MBEDTLS_CONFIG_H
#define RERINKU_MBEDTLS_CONFIG_H

/* --- system ---------------------------------------------------------------- */
#define MBEDTLS_HAVE_ASM
#define MBEDTLS_HAVE_TIME
#define MBEDTLS_HAVE_TIME_DATE      /* certificate validity periods */
#define MBEDTLS_FS_IO               /* load the CA bundle from a file */
#define MBEDTLS_DEPRECATED_REMOVED
#define MBEDTLS_ERROR_STRERROR_DUMMY /* error codes in hex, no string table */
#define MBEDTLS_AES_ROM_TABLES      /* AES tables in .rodata, not in RAM */

/* --- random numbers ---------------------------------------------------------- */
#define MBEDTLS_ENTROPY_C
#define MBEDTLS_CTR_DRBG_C

/* --- hashes ----------------------------------------------------------------- */
#define MBEDTLS_MD_C
#define MBEDTLS_SHA256_C            /* TLS 1.2 PRF, fingerprints, signatures */
#define MBEDTLS_SHA384_C            /* *_SHA384 suites, P-384 chains */
#define MBEDTLS_SHA512_C

/* --- ciphers ----------------------------------------------------------------- */
#define MBEDTLS_AES_C
#define MBEDTLS_CIPHER_C
#define MBEDTLS_GCM_C

/* --- public key -------------------------------------------------------------- */
#define MBEDTLS_BIGNUM_C
#define MBEDTLS_ECP_C
#define MBEDTLS_ECP_DP_SECP256R1_ENABLED
#define MBEDTLS_ECP_DP_SECP384R1_ENABLED /* P-384 CA keys (e.g. ISRG Root X2) */
#define MBEDTLS_ECP_NIST_OPTIM
#define MBEDTLS_ECDH_C
#define MBEDTLS_ECDSA_C
#define MBEDTLS_RSA_C               /* most CA chains are RSA */
#define MBEDTLS_PKCS1_V15
#define MBEDTLS_PK_C
#define MBEDTLS_PK_PARSE_C
#define MBEDTLS_PK_WRITE_C
#define MBEDTLS_ASN1_PARSE_C
#define MBEDTLS_ASN1_WRITE_C
#define MBEDTLS_OID_C
#define MBEDTLS_BASE64_C
#define MBEDTLS_PEM_PARSE_C         /* PEM CA bundles */

/* --- X.509 ------------------------------------------------------------------- */
#define MBEDTLS_X509_USE_C
#define MBEDTLS_X509_CRT_PARSE_C
#define MBEDTLS_X509_CREATE_C
#define MBEDTLS_X509_CRT_WRITE_C    /* the self-signed DTLS certificate */

/* --- TLS / DTLS -------------------------------------------------------------- */
#define MBEDTLS_SSL_TLS_C
#define MBEDTLS_SSL_CLI_C
#define MBEDTLS_SSL_SRV_C           /* DTLS server role (answer "active") */
#define MBEDTLS_SSL_PROTO_TLS1_2
#define MBEDTLS_SSL_PROTO_DTLS
#define MBEDTLS_SSL_DTLS_ANTI_REPLAY
#define MBEDTLS_SSL_DTLS_HELLO_VERIFY /* the SFU's DTLS server sends cookies */
#define MBEDTLS_SSL_DTLS_SRTP
#define MBEDTLS_SSL_EXTENDED_MASTER_SECRET
#define MBEDTLS_SSL_SERVER_NAME_INDICATION
#define MBEDTLS_SSL_KEEP_PEER_CERTIFICATE
#define MBEDTLS_KEY_EXCHANGE_ECDHE_ECDSA_ENABLED
#define MBEDTLS_KEY_EXCHANGE_ECDHE_RSA_ENABLED

/* Servers may send full 16 KB records; what we send is small (HTTP requests,
 * MQTT packets, a DTLS handshake), and a JPEG upload is simply split. */
#define MBEDTLS_SSL_IN_CONTENT_LEN 16384
#define MBEDTLS_SSL_OUT_CONTENT_LEN 4096
#define MBEDTLS_SSL_DTLS_MAX_BUFFERING 8192

#endif /* RERINKU_MBEDTLS_CONFIG_H */
