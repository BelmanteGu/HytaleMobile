// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

// SecureTransport, SecTrust, SecCertificate, SecKey and in-memory keychains for
// System.Net.Security and System.Security.Cryptography on macOS.
//
// TLS sessions run in the platform SSLEngine and certificate chains are validated
// with the platform trust store, both through org.hyprism.game.TlsBridge over JNI.
// The client validates every peer chain itself after the handshake (SSLCopyPeerTrust,
// then SecTrustCreateWithCertificates and SecTrustEvaluateWithError), as on macOS,
// so the engine does not reject certificates on its own.
//
// The command line loader has no Java VM: handshakes and chain evaluation fail there.

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "macho.h"
#include "objc.h"

#ifdef HYPRISM_MACHO_LIBRARY
#include <jni.h>
extern void *SDL_GetAndroidJNIEnv(void);
#endif

typedef int32_t OSStatus;

enum {
    noErr = 0,
    errSecUnimplemented = -4,
    errSecParam = -50,
    errSSLProtocol = -9800,
    errSSLWouldBlock = -9803,
    errSSLClosedGraceful = -9805,
    errSSLClosedAbort = -9806,
    errSSLInternal = -9810,
    errSSLPeerHandshakeFail = -9824,
    errSecCertificateExpired = -67818,
    errSecNotTrusted = -67843,
};

enum { kSSLServerSide = 0, kSSLClientSide = 1 };
enum { kSSLIdle, kSSLHandshake, kSSLConnected, kSSLClosed, kSSLAborted };
enum { kSecTrustResultInvalid = 0, kSecTrustResultUnspecified = 4, kSecTrustResultRecoverableTrustFailure = 5 };

// SSLProtocol values.
enum { kTLSProtocol1 = 4, kTLSProtocol11 = 7, kTLSProtocol12 = 8, kTLSProtocol13 = 10 };

// TlsBridge.handshake results and TlsBridge.verify statuses.
enum { TLS_DONE = 0, TLS_NEED_INPUT = 1, TLS_FAILED = -1 };
enum { TRUST_OK = 0, TRUST_UNTRUSTED = 1, TRUST_EXPIRED = 2 };

typedef OSStatus (*ssl_io_fn)(const void *connection, void *data, size_t *length);

struct sec_certificate {
    struct cf_object base;
    struct cf_data *der;
};

struct sec_trust {
    struct cf_object base;
    struct cf_array *certs;   // as given, leaf first
    struct cf_array *anchors; // custom roots, or NULL for the platform store
    struct cf_array *chain;   // built chain after evaluation
    int evaluated;
    int status;
};

struct ssl_context {
    struct cf_object base;
    pthread_mutex_t lock;
    const void *connection;
    ssl_io_fn read, write;
    char *peer_name;
    struct cf_array *alpn;
    int min_version, max_version;
    int state;
    int version;
    uint16_t cipher;
    struct cf_string *alpn_selected;
    struct cf_array *peer_certs;
    void *bridge; // TlsBridge global reference
};

static _Thread_local char t_tls_error[256];

static void set_error(const char *message)
{
    snprintf(t_tls_error, sizeof t_tls_error, "%s", message ? message : "");
}

// ---------------------------------------------------------------------------
// Certificates

static struct sec_certificate *certificate_from_der(const void *der, size_t len)
{
    if (!der || len < 2 || ((const uint8_t *)der)[0] != 0x30)
        return NULL; // DER SEQUENCE
    struct sec_certificate *c = cf_alloc("SecCertificate", sizeof *c);
    c->der = cf_data_create(der, len);
    return c;
}

static struct sec_certificate *SecCertificateCreateWithData(void *alloc, struct cf_data *data)
{
    (void)alloc;
    return cf_type_of(data) == CF_TYPE_DATA ? certificate_from_der(data->bytes, data->length) : NULL;
}

static struct cf_data *SecCertificateCopyData(struct sec_certificate *c)
{
    return cf_type_of(c) == CF_TYPE_SEC_CERTIFICATE ? cf_retain(c->der) : NULL;
}

static uint32_t SecCertificateGetTypeID(void) { return CF_TYPE_SEC_CERTIFICATE; }
static uint32_t SecIdentityGetTypeID(void) { return CF_TYPE_SEC_IDENTITY; }
static uint32_t SecKeyGetTypeID(void) { return CF_TYPE_SEC_KEY; }

static void *new_policy(void) { return cf_alloc("SecPolicy", sizeof(struct cf_object)); }
static void *SecPolicyCreateBasicX509(void) { return new_policy(); }
static void *SecPolicyCreateRevocation(unsigned long flags) { (void)flags; return new_policy(); }

// ---------------------------------------------------------------------------
// Java bridge

#ifdef HYPRISM_MACHO_LIBRARY

static jclass g_bridge;
static void key_bridge_init(JNIEnv *env);
static jmethodID m_new, m_handshake, m_read, m_write, m_take, m_closed, m_error, m_protocol, m_alpn, m_cipher,
    m_peers, m_verify, m_summary;

/// Caches TlsBridge on a thread whose class loader sees the app classes.
void security_init_java(void *penv)
{
    JNIEnv *env = penv;
    if (!env)
        return;
    jclass c = (*env)->FindClass(env, "org/hyprism/game/TlsBridge");
    if (!c) {
        (*env)->ExceptionClear(env);
        macho_log("tls", "TlsBridge not found; TLS is unavailable");
        return;
    }
    m_new = (*env)->GetMethodID(env, c, "<init>", "(Ljava/lang/String;[Ljava/lang/String;[Ljava/lang/String;)V");
    m_handshake = (*env)->GetMethodID(env, c, "handshake", "([B)I");
    m_read = (*env)->GetMethodID(env, c, "read", "([BI)[B");
    m_write = (*env)->GetMethodID(env, c, "write", "([B)Z");
    m_take = (*env)->GetMethodID(env, c, "takeOutput", "()[B");
    m_closed = (*env)->GetMethodID(env, c, "isClosed", "()Z");
    m_error = (*env)->GetMethodID(env, c, "error", "()Ljava/lang/String;");
    m_protocol = (*env)->GetMethodID(env, c, "protocol", "()Ljava/lang/String;");
    m_alpn = (*env)->GetMethodID(env, c, "alpn", "()Ljava/lang/String;");
    m_cipher = (*env)->GetMethodID(env, c, "cipherSuite", "()I");
    m_peers = (*env)->GetMethodID(env, c, "peerCertificates", "()[[B");
    m_verify = (*env)->GetStaticMethodID(env, c, "verify", "([[B[[B[I)[[B");
    m_summary = (*env)->GetStaticMethodID(env, c, "subjectSummary", "([B)Ljava/lang/String;");
    if ((*env)->ExceptionCheck(env)) {
        (*env)->ExceptionDescribe(env);
        (*env)->ExceptionClear(env);
        macho_log("tls", "TlsBridge is missing methods; TLS is unavailable");
        return;
    }
    g_bridge = (*env)->NewGlobalRef(env, c);
    key_bridge_init(env);
}

// Attaches the calling thread on first use; SDL detaches it when the thread exits.
static JNIEnv *java_env(void)
{
    return g_bridge ? SDL_GetAndroidJNIEnv() : NULL;
}

static int java_failed(JNIEnv *env)
{
    if (!(*env)->ExceptionCheck(env))
        return 0;
    (*env)->ExceptionDescribe(env);
    (*env)->ExceptionClear(env);
    return 1;
}

static jbyteArray java_bytes(JNIEnv *env, const void *bytes, size_t len)
{
    jbyteArray a = (*env)->NewByteArray(env, (jsize)len);
    if (a && len)
        (*env)->SetByteArrayRegion(env, a, 0, (jsize)len, bytes);
    return a;
}

static void java_copy_string(JNIEnv *env, jstring s, char *out, size_t size)
{
    out[0] = 0;
    if (!s)
        return;
    const char *utf = (*env)->GetStringUTFChars(env, s, NULL);
    if (utf) {
        snprintf(out, size, "%s", utf);
        (*env)->ReleaseStringUTFChars(env, s, utf);
    }
}

static jobjectArray java_strings(JNIEnv *env, const char *const *items, size_t count)
{
    jclass string_class = (*env)->FindClass(env, "java/lang/String");
    jobjectArray a = (*env)->NewObjectArray(env, (jsize)count, string_class, NULL);
    for (size_t i = 0; i < count; i++)
        (*env)->SetObjectArrayElement(env, a, (jsize)i, (*env)->NewStringUTF(env, items[i]));
    return a;
}

static jobjectArray java_certificates(JNIEnv *env, struct cf_array *certs)
{
    jclass bytes_class = (*env)->FindClass(env, "[B");
    jobjectArray a = (*env)->NewObjectArray(env, (jsize)certs->count, bytes_class, NULL);
    for (size_t i = 0; i < certs->count; i++) {
        struct sec_certificate *c = certs->items[i];
        if (cf_type_of(c) == CF_TYPE_SEC_CERTIFICATE)
            (*env)->SetObjectArrayElement(env, a, (jsize)i, java_bytes(env, c->der->bytes, c->der->length));
    }
    return a;
}

static struct cf_array *certificates_from_java(JNIEnv *env, jobjectArray a)
{
    struct cf_array *certs = cf_array_create();
    jsize n = a ? (*env)->GetArrayLength(env, a) : 0;
    for (jsize i = 0; i < n; i++) {
        jbyteArray der = (*env)->GetObjectArrayElement(env, a, i);
        jsize len = der ? (*env)->GetArrayLength(env, der) : 0;
        jbyte *bytes = der ? (*env)->GetByteArrayElements(env, der, NULL) : NULL;
        struct sec_certificate *c = bytes ? certificate_from_der(bytes, (size_t)len) : NULL;
        if (bytes)
            (*env)->ReleaseByteArrayElements(env, der, bytes, JNI_ABORT);
        if (c) {
            cf_array_append(certs, c);
            cf_release(c);
        }
        (*env)->DeleteLocalRef(env, der);
    }
    return certs;
}

#endif

// ---------------------------------------------------------------------------
// Trust

static OSStatus SecTrustCreateWithCertificates(void *certs, void *policies, struct sec_trust **out)
{
    (void)policies;
    if (!out || !certs)
        return errSecParam;
    struct sec_trust *t = cf_alloc("SecTrust", sizeof *t);
    t->certs = cf_array_create();
    if (cf_type_of(certs) == CF_TYPE_ARRAY) {
        struct cf_array *a = certs;
        for (size_t i = 0; i < a->count; i++)
            if (cf_type_of(a->items[i]) == CF_TYPE_SEC_CERTIFICATE)
                cf_array_append(t->certs, a->items[i]);
    } else if (cf_type_of(certs) == CF_TYPE_SEC_CERTIFICATE) {
        cf_array_append(t->certs, certs);
    }
    *out = t;
    return noErr;
}

static OSStatus SecTrustSetAnchorCertificates(struct sec_trust *t, struct cf_array *anchors)
{
    if (cf_type_of(t) != CF_TYPE_SEC_TRUST)
        return errSecParam;
    cf_release(t->anchors);
    t->anchors = cf_retain(anchors);
    t->evaluated = 0;
    return noErr;
}

static OSStatus SecTrustSetVerifyDate(struct sec_trust *t, void *date)
{
    (void)date; // the platform trust manager validates at the current time
    return cf_type_of(t) == CF_TYPE_SEC_TRUST ? noErr : errSecParam;
}

static OSStatus SecTrustSetNetworkFetchAllowed(struct sec_trust *t, bool allowed)
{
    (void)allowed;
    return cf_type_of(t) == CF_TYPE_SEC_TRUST ? noErr : errSecParam;
}

static void evaluate(struct sec_trust *t)
{
    if (t->evaluated)
        return;
    t->evaluated = 1;
    t->status = TRUST_UNTRUSTED;
    cf_release(t->chain);
    t->chain = cf_retain(t->certs);
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = java_env();
    if (!env || !t->certs->count)
        return;
    (*env)->PushLocalFrame(env, 32);
    jobjectArray certs = java_certificates(env, t->certs);
    jobjectArray anchors = t->anchors ? java_certificates(env, t->anchors) : NULL;
    jintArray status = (*env)->NewIntArray(env, 1);
    jobjectArray built = (*env)->CallStaticObjectMethod(env, g_bridge, m_verify, certs, anchors, status);
    if (!java_failed(env)) {
        jint verdict = TRUST_UNTRUSTED;
        (*env)->GetIntArrayRegion(env, status, 0, 1, &verdict);
        t->status = verdict;
        if (verdict == TRUST_OK && built) {
            cf_release(t->chain);
            t->chain = certificates_from_java(env, built);
        }
    }
    (*env)->PopLocalFrame(env, NULL);
#endif
}

static OSStatus trust_status(const struct sec_trust *t)
{
    return t->status == TRUST_OK ? noErr : t->status == TRUST_EXPIRED ? errSecCertificateExpired : errSecNotTrusted;
}

static bool SecTrustEvaluateWithError(struct sec_trust *t, struct cf_error **error)
{
    if (error)
        *error = NULL;
    if (cf_type_of(t) != CF_TYPE_SEC_TRUST)
        return false;
    evaluate(t);
    if (t->status != TRUST_OK && error)
        *error = cf_error_create("NSOSStatusErrorDomain", trust_status(t),
                                 t->status == TRUST_EXPIRED ? "certificate is not valid at this time"
                                                            : "certificate is not trusted");
    return t->status == TRUST_OK;
}

static OSStatus SecTrustGetTrustResult(struct sec_trust *t, uint32_t *result)
{
    if (cf_type_of(t) != CF_TYPE_SEC_TRUST || !result)
        return errSecParam;
    *result = !t->evaluated ? kSecTrustResultInvalid
              : t->status == TRUST_OK ? kSecTrustResultUnspecified
                                      : kSecTrustResultRecoverableTrustFailure;
    return noErr;
}

static struct cf_array *trust_certificates(struct sec_trust *t)
{
    return t->evaluated && t->chain ? t->chain : t->certs;
}

static long SecTrustGetCertificateCount(struct sec_trust *t)
{
    return cf_type_of(t) == CF_TYPE_SEC_TRUST ? (long)trust_certificates(t)->count : 0;
}

static void *SecTrustGetCertificateAtIndex(struct sec_trust *t, long index)
{
    if (cf_type_of(t) != CF_TYPE_SEC_TRUST)
        return NULL;
    struct cf_array *certs = trust_certificates(t);
    return index >= 0 && (size_t)index < certs->count ? certs->items[index] : NULL;
}

// {"TrustResultDetails": [one dictionary per chain element]}, where each key names
// a failed check (see AppleCryptoNative_X509ChainGetStatusAtIndex in .NET).
static struct cf_dictionary *SecTrustCopyResult(struct sec_trust *t)
{
    if (cf_type_of(t) != CF_TYPE_SEC_TRUST)
        return NULL;
    struct cf_array *certs = trust_certificates(t);
    struct cf_array *details = cf_array_create();
    for (size_t i = 0; i < certs->count; i++) {
        struct cf_dictionary *element = cf_dictionary_create();
        const char *failure = NULL;
        if (t->status == TRUST_EXPIRED && i == 0)
            failure = "TemporalValidity";
        else if (t->status == TRUST_UNTRUSTED && i == certs->count - 1)
            failure = "AnchorTrusted";
        if (failure) {
            struct cf_string *key = cf_string_from_cstr(failure);
            cf_dictionary_set(element, key, &cf_boolean_false);
            cf_release(key);
        }
        cf_array_append(details, element);
        cf_release(element);
    }
    struct cf_dictionary *result = cf_dictionary_create();
    struct cf_string *key = cf_string_from_cstr("TrustResultDetails");
    cf_dictionary_set(result, key, details);
    cf_release(key);
    cf_release(details);
    return result;
}

static struct cf_string *SecCertificateCopySubjectSummary(struct sec_certificate *c)
{
    if (cf_type_of(c) != CF_TYPE_SEC_CERTIFICATE)
        return NULL;
    char summary[256] = "";
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = java_env();
    if (env) {
        (*env)->PushLocalFrame(env, 8);
        jstring s = (*env)->CallStaticObjectMethod(env, g_bridge, m_summary, java_bytes(env, c->der->bytes, c->der->length));
        if (!java_failed(env))
            java_copy_string(env, s, summary, sizeof summary);
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    return summary[0] ? cf_string_from_cstr(summary) : NULL;
}

// ---------------------------------------------------------------------------
// SecureTransport

static struct ssl_context *SSLCreateContext(void *alloc, int side, int type)
{
    (void)alloc; (void)type;
    if (side != kSSLClientSide) {
        macho_log("tls", "server-side TLS is not supported");
        return NULL;
    }
    struct ssl_context *c = cf_alloc("SSLContext", sizeof *c);
    pthread_mutex_init(&c->lock, NULL);
    return c;
}

#define CONTEXT(c) if (cf_type_of(c) != CF_TYPE_SSL_CONTEXT) return errSecParam

static OSStatus SSLSetConnection(struct ssl_context *c, const void *connection)
{
    CONTEXT(c);
    c->connection = connection;
    return noErr;
}

static OSStatus SSLSetIOFuncs(struct ssl_context *c, ssl_io_fn read, ssl_io_fn write)
{
    CONTEXT(c);
    c->read = read;
    c->write = write;
    return noErr;
}

static OSStatus SSLSetPeerDomainName(struct ssl_context *c, const char *name, size_t len)
{
    CONTEXT(c);
    free(c->peer_name);
    c->peer_name = name && len ? strndup(name, len) : NULL;
    return noErr;
}

static OSStatus SSLGetPeerDomainNameLength(struct ssl_context *c, size_t *len)
{
    CONTEXT(c);
    *len = c->peer_name ? strlen(c->peer_name) : 0;
    return noErr;
}

static OSStatus SSLSetProtocolVersionMin(struct ssl_context *c, int version)
{
    CONTEXT(c);
    c->min_version = version;
    return noErr;
}

static OSStatus SSLSetProtocolVersionMax(struct ssl_context *c, int version)
{
    CONTEXT(c);
    c->max_version = version;
    return noErr;
}

static OSStatus SSLSetALPNProtocols(struct ssl_context *c, struct cf_array *protocols)
{
    CONTEXT(c);
    cf_release(c->alpn);
    c->alpn = cf_retain(protocols);
    return noErr;
}

static OSStatus SSLCopyALPNProtocols(struct ssl_context *c, struct cf_array **out)
{
    CONTEXT(c);
    *out = NULL;
    if (c->alpn_selected) {
        *out = cf_array_create();
        cf_array_append(*out, c->alpn_selected);
    }
    return noErr;
}

static OSStatus SSLSetSessionOption(struct ssl_context *c, int option, bool value)
{
    (void)option; (void)value; // break-on-auth: the client validates after the handshake anyway
    CONTEXT(c);
    return noErr;
}

static OSStatus SSLSetEnabledCiphers(struct ssl_context *c, const uint16_t *ciphers, size_t count)
{
    (void)ciphers; (void)count; // the platform defaults apply
    CONTEXT(c);
    return noErr;
}

static OSStatus SSLSetCertificate(struct ssl_context *c, struct cf_array *certs)
{
    CONTEXT(c);
    if (certs)
        macho_log("tls", "client certificates are not supported");
    return noErr;
}

static OSStatus SSLSetClientSideAuthenticate(struct ssl_context *c, int auth) { (void)c; (void)auth; return errSecUnimplemented; }
static OSStatus SSLSetCertificateAuthorities(struct ssl_context *c, void *certs, bool replace) { (void)c; (void)certs; (void)replace; return errSecUnimplemented; }

static OSStatus SSLCopyDistinguishedNames(struct ssl_context *c, struct cf_array **out)
{
    CONTEXT(c);
    *out = NULL;
    return noErr;
}

static OSStatus SSLGetSessionState(struct ssl_context *c, int *state)
{
    CONTEXT(c);
    *state = c->state;
    return noErr;
}

static OSStatus SSLGetNegotiatedProtocolVersion(struct ssl_context *c, int *version)
{
    CONTEXT(c);
    *version = c->version;
    return noErr;
}

// SSLCipherSuite is 16 bits on arm64.
static OSStatus SSLGetNegotiatedCipher(struct ssl_context *c, uint16_t *cipher)
{
    CONTEXT(c);
    *cipher = c->cipher;
    return noErr;
}

static OSStatus SSLCopyPeerTrust(struct ssl_context *c, struct sec_trust **out)
{
    CONTEXT(c);
    *out = NULL;
    if (!c->peer_certs || !c->peer_certs->count)
        return errSSLProtocol;
    return SecTrustCreateWithCertificates(c->peer_certs, NULL, out);
}

#ifdef HYPRISM_MACHO_LIBRARY

// Pulls everything the client has buffered from the network.
static OSStatus read_available(struct ssl_context *c, uint8_t **out, size_t *out_len)
{
    size_t capacity = 0, len = 0;
    uint8_t *buf = NULL;
    for (;;) {
        if (capacity - len < 16384) {
            capacity = capacity ? capacity * 2 : 32768;
            buf = realloc(buf, capacity);
        }
        size_t n = capacity - len;
        OSStatus status = c->read(c->connection, buf + len, &n);
        if (status != noErr && status != errSSLWouldBlock) {
            free(buf);
            return status;
        }
        len += n;
        if (status == errSSLWouldBlock || n == 0)
            break;
    }
    *out = buf;
    *out_len = len;
    return noErr;
}

static jbyteArray take_input(JNIEnv *env, struct ssl_context *c, OSStatus *status, size_t *len)
{
    uint8_t *bytes = NULL;
    *len = 0;
    *status = read_available(c, &bytes, len);
    jbyteArray input = *status == noErr && *len ? java_bytes(env, bytes, *len) : NULL;
    free(bytes);
    return input;
}

static OSStatus flush_output(JNIEnv *env, struct ssl_context *c)
{
    jbyteArray out = (*env)->CallObjectMethod(env, c->bridge, m_take);
    if (java_failed(env))
        return errSSLInternal;
    jsize n = out ? (*env)->GetArrayLength(env, out) : 0;
    OSStatus status = noErr;
    if (n > 0) {
        jbyte *bytes = (*env)->GetByteArrayElements(env, out, NULL);
        size_t len = (size_t)n;
        status = c->write(c->connection, bytes, &len);
        (*env)->ReleaseByteArrayElements(env, out, bytes, JNI_ABORT);
    }
    (*env)->DeleteLocalRef(env, out);
    return status;
}

static void bridge_error(JNIEnv *env, struct ssl_context *c)
{
    jstring s = (*env)->CallObjectMethod(env, c->bridge, m_error);
    if (!java_failed(env))
        java_copy_string(env, s, t_tls_error, sizeof t_tls_error);
    macho_log("tls", "%s: %s", c->peer_name ? c->peer_name : "?", t_tls_error);
}

static OSStatus create_bridge(JNIEnv *env, struct ssl_context *c)
{
    static const struct { int version; const char *name; } k_versions[] = {
        { kTLSProtocol1, "TLSv1" }, { kTLSProtocol11, "TLSv1.1" },
        { kTLSProtocol12, "TLSv1.2" }, { kTLSProtocol13, "TLSv1.3" },
    };
    const char *protocols[4], *alpn[16];
    size_t protocol_count = 0, alpn_count = 0;
    if (c->min_version || c->max_version)
        for (size_t i = 0; i < 4; i++)
            if ((!c->min_version || k_versions[i].version >= c->min_version) &&
                (!c->max_version || k_versions[i].version <= c->max_version))
                protocols[protocol_count++] = k_versions[i].name;
    for (size_t i = 0; c->alpn && i < c->alpn->count && alpn_count < 16; i++)
        if (cf_type_of(c->alpn->items[i]) == CF_TYPE_STRING)
            alpn[alpn_count++] = ((struct cf_string *)c->alpn->items[i])->utf8;

    jstring host = c->peer_name ? (*env)->NewStringUTF(env, c->peer_name) : NULL;
    jobject bridge = (*env)->NewObject(env, g_bridge, m_new, host, java_strings(env, alpn, alpn_count),
                                       protocol_count ? java_strings(env, protocols, protocol_count) : NULL);
    if (java_failed(env) || !bridge) {
        set_error("could not create the TLS engine");
        return errSSLInternal;
    }
    c->bridge = (*env)->NewGlobalRef(env, bridge);
    return noErr;
}

static void read_session(JNIEnv *env, struct ssl_context *c)
{
    char text[64];
    java_copy_string(env, (*env)->CallObjectMethod(env, c->bridge, m_protocol), text, sizeof text);
    c->version = strcmp(text, "TLSv1.3") == 0   ? kTLSProtocol13
                 : strcmp(text, "TLSv1.2") == 0 ? kTLSProtocol12
                 : strcmp(text, "TLSv1.1") == 0 ? kTLSProtocol11
                 : strcmp(text, "TLSv1") == 0   ? kTLSProtocol1
                                                : 0;
    c->cipher = (uint16_t)(*env)->CallIntMethod(env, c->bridge, m_cipher);
    java_copy_string(env, (*env)->CallObjectMethod(env, c->bridge, m_alpn), text, sizeof text);
    cf_release(c->alpn_selected);
    c->alpn_selected = text[0] ? cf_string_from_cstr(text) : NULL;
    cf_release(c->peer_certs);
    c->peer_certs = certificates_from_java(env, (*env)->CallObjectMethod(env, c->bridge, m_peers));
    java_failed(env);
}

#endif

static OSStatus SSLHandshake(struct ssl_context *c)
{
    CONTEXT(c);
    if (c->state == kSSLConnected)
        return noErr;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = java_env();
    if (!env || !c->read || !c->write) {
        set_error("TLS is unavailable");
        return errSSLInternal;
    }
    pthread_mutex_lock(&c->lock);
    (*env)->PushLocalFrame(env, 64);
    OSStatus result = c->bridge ? noErr : create_bridge(env, c);
    c->state = result == noErr ? kSSLHandshake : kSSLAborted;
    while (result == noErr) {
        size_t received;
        jbyteArray input = take_input(env, c, &result, &received);
        if (result != noErr)
            break;
        jint step = (*env)->CallIntMethod(env, c->bridge, m_handshake, input);
        if (java_failed(env))
            step = TLS_FAILED;
        (*env)->DeleteLocalRef(env, input);
        if ((result = flush_output(env, c)) != noErr)
            break;
        if (step == TLS_DONE) {
            read_session(env, c);
            c->state = kSSLConnected;
            break;
        }
        if (step == TLS_FAILED) {
            bridge_error(env, c);
            c->state = kSSLAborted;
            result = errSSLPeerHandshakeFail;
            break;
        }
        if (!received)
            result = errSSLWouldBlock; // the client must fetch more from the network
    }
    (*env)->PopLocalFrame(env, NULL);
    pthread_mutex_unlock(&c->lock);
    return result;
#else
    set_error("TLS needs the Android host");
    return errSSLInternal;
#endif
}

static OSStatus SSLRead(struct ssl_context *c, void *data, size_t len, size_t *processed)
{
    CONTEXT(c);
    *processed = 0;
    if (c->state == kSSLClosed)
        return errSSLClosedGraceful;
    if (c->state != kSSLConnected)
        return errSSLClosedAbort;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = java_env();
    pthread_mutex_lock(&c->lock);
    (*env)->PushLocalFrame(env, 16);
    size_t received;
    OSStatus result;
    jbyteArray input = take_input(env, c, &result, &received);
    if (result == noErr) {
        jbyteArray plain = (*env)->CallObjectMethod(env, c->bridge, m_read, input, (jint)(len > 0x7fffffff ? 0x7fffffff : len));
        if (java_failed(env) || !plain) {
            bridge_error(env, c);
            c->state = kSSLAborted;
            result = errSSLProtocol;
        } else {
            jsize n = (*env)->GetArrayLength(env, plain);
            (*env)->GetByteArrayRegion(env, plain, 0, n, data);
            *processed = (size_t)n;
            result = flush_output(env, c);
            if (result == noErr && n == 0) {
                if ((*env)->CallBooleanMethod(env, c->bridge, m_closed)) {
                    c->state = kSSLClosed;
                    result = errSSLClosedGraceful;
                } else {
                    result = errSSLWouldBlock;
                }
            }
        }
    }
    (*env)->PopLocalFrame(env, NULL);
    pthread_mutex_unlock(&c->lock);
    return result;
#else
    (void)data; (void)len;
    return errSSLInternal;
#endif
}

static OSStatus SSLWrite(struct ssl_context *c, const void *data, size_t len, size_t *processed)
{
    CONTEXT(c);
    *processed = 0;
    if (c->state != kSSLConnected)
        return c->state == kSSLClosed ? errSSLClosedGraceful : errSSLClosedAbort;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = java_env();
    pthread_mutex_lock(&c->lock);
    (*env)->PushLocalFrame(env, 16);
    OSStatus result = noErr;
    jboolean ok = (*env)->CallBooleanMethod(env, c->bridge, m_write, java_bytes(env, data, len));
    if (java_failed(env) || !ok) {
        bridge_error(env, c);
        c->state = kSSLAborted;
        result = errSSLProtocol;
    } else if ((result = flush_output(env, c)) == noErr) {
        *processed = len;
    }
    (*env)->PopLocalFrame(env, NULL);
    pthread_mutex_unlock(&c->lock);
    return result;
#else
    (void)data; (void)len;
    return errSSLInternal;
#endif
}

// ---------------------------------------------------------------------------
// Keys, identities and keychains
//
// System.Security.Cryptography generates keys with SecKeyCreateRandomKey, signs
// with SecKeyCreateSignature, and pairs a certificate with its private key by
// importing the key into a temporary keychain and querying the identity back
// (AppleCryptoNative_X509CopyWithPrivateKey). Keys hold Apple's external
// representation; KeyBridge does the cryptography with the platform provider.
// Keychains only live in memory.

enum {
    errSecNoSuchKeychain = -25294,
    errSecItemNotFound = -25300,
    errSecUnknownFormat = -25257,
    errSecNoTrustSettings = -25263,
};
enum { kSecFormatX509Cert = 9 };
enum { kSecItemTypePrivateKey = 1, kSecItemTypePublicKey = 2, kSecItemTypeCertificate = 4 };

struct sec_keychain {
    struct cf_object base;
    struct cf_array *keys;
    struct cf_array *certs;
};

struct sec_key {
    struct cf_object base;
    int ec;
    int is_private;
    int bits;
    struct cf_data *repr;
    struct sec_keychain *keychain; // not retained; the keychain holds the key
};

struct sec_identity {
    struct cf_object base;
    struct sec_certificate *cert;
    struct sec_key *key;
};

#define SEC_STRING(var, text)                                                                                 \
    static struct cf_string var##_string = { { &g_constant_string_class, 1, 0 }, (char *)text, sizeof text - 1 }; \
    static void *var = &var##_string;

SEC_STRING(kSecAttrKeyType, "type")
SEC_STRING(kSecAttrKeySizeInBits, "bsiz")
SEC_STRING(kSecAttrKeyClass, "kcls")
SEC_STRING(kSecAttrKeyClassPublic, "0")
SEC_STRING(kSecAttrKeyClassPrivate, "1")
SEC_STRING(kSecAttrKeyTypeRSA, "42")
SEC_STRING(kSecAttrKeyTypeEC, "73")
SEC_STRING(kSecAttrIsExtractable, "extr")
SEC_STRING(kSecAttrIsSensitive, "sens")
SEC_STRING(kSecClass, "class")
SEC_STRING(kSecClassKey, "keys")
SEC_STRING(kSecClassCertificate, "cert")
SEC_STRING(kSecClassIdentity, "idnt")
SEC_STRING(kSecReturnRef, "r_Ref")
SEC_STRING(kSecMatchLimit, "m_Limit")
SEC_STRING(kSecMatchLimitAll, "m_LimitAll")
SEC_STRING(kSecMatchSearchList, "m_SearchList")
SEC_STRING(kSecMatchItemList, "m_ItemList")
SEC_STRING(kSecUseDataProtectionKeychain, "nleg")
SEC_STRING(kSecInputIsAttributeName, "InputIs")
SEC_STRING(kSecInputIsDigest, "InputIsDigest")
SEC_STRING(kSecTransformInputAttributeName, "INPUT")
SEC_STRING(kSecKeyAlgorithmECDSASignatureDigestX962, "algid:sign:ECDSA:digest-X962")
SEC_STRING(kSecKeyAlgorithmRSASignatureRaw, "algid:sign:RSA:raw")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1, "algid:sign:RSA:digest-PKCS1v15:SHA1")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256, "algid:sign:RSA:digest-PKCS1v15:SHA256")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA384, "algid:sign:RSA:digest-PKCS1v15:SHA384")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512, "algid:sign:RSA:digest-PKCS1v15:SHA512")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPSSSHA1, "algid:sign:RSA:digest-PSS:SHA1:SHA1:20")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPSSSHA256, "algid:sign:RSA:digest-PSS:SHA256:SHA256:32")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPSSSHA384, "algid:sign:RSA:digest-PSS:SHA384:SHA384:48")
SEC_STRING(kSecKeyAlgorithmRSASignatureDigestPSSSHA512, "algid:sign:RSA:digest-PSS:SHA512:SHA512:64")

enum { kSecFormatOpenSSL = 1, kSecFormatWrappedPKCS8 = 5 };

// SecItemImportExportKeyParameters; only the passphrase is read.
struct key_params {
    uint32_t version;
    uint32_t flags;
    struct cf_string *passphrase;
};

static void set_cf_error(struct cf_error **error, long code, const char *message)
{
    if (error)
        *error = cf_error_create("NSOSStatusErrorDomain", code, message);
    macho_log("sec", "%s (%ld)", message, code);
}

static int64_t cf_number_int(const void *o)
{
    const struct cf_number *n = o;
    if (cf_type_of(n) != CF_TYPE_NUMBER)
        return 0;
    return n->is_float ? (int64_t)n->d : n->i;
}

static struct sec_key *new_key(int ec, int is_private, int bits, const void *repr, size_t len)
{
    struct sec_key *k = cf_alloc("SecKey", sizeof *k);
    k->ec = ec;
    k->is_private = is_private;
    k->bits = bits;
    k->repr = cf_data_create(repr, len);
    return k;
}

static int ec_bits_for_field(size_t n) { return n == 32 ? 256 : n == 48 ? 384 : n == 66 ? 521 : (int)n * 8; }

#ifdef HYPRISM_MACHO_LIBRARY
static jclass g_key_bridge;
static jmethodID m_key_generate, m_key_rsa_public, m_key_rsa_bits, m_key_sign, m_key_cert_public, m_key_export_epki,
    m_key_import_epki, m_key_export_spki;

static void key_bridge_init(JNIEnv *env)
{
    jclass c = (*env)->FindClass(env, "org/hyprism/game/KeyBridge");
    if (!c) {
        (*env)->ExceptionClear(env);
        macho_log("sec", "KeyBridge not found; key generation is unavailable");
        return;
    }
    m_key_generate = (*env)->GetStaticMethodID(env, c, "generate", "(ZI)[B");
    m_key_rsa_public = (*env)->GetStaticMethodID(env, c, "rsaPublicFromPrivate", "([B)[B");
    m_key_rsa_bits = (*env)->GetStaticMethodID(env, c, "rsaBits", "([BZ)I");
    m_key_sign = (*env)->GetStaticMethodID(env, c, "sign", "(ZI[BLjava/lang/String;[B)[B");
    m_key_cert_public = (*env)->GetStaticMethodID(env, c, "certificatePublicKey", "([B[I)[B");
    m_key_export_epki = (*env)->GetStaticMethodID(env, c, "exportEncryptedPkcs8", "(ZI[BLjava/lang/String;)[B");
    m_key_import_epki = (*env)->GetStaticMethodID(env, c, "importEncryptedPkcs8", "([BLjava/lang/String;[I)[B");
    m_key_export_spki = (*env)->GetStaticMethodID(env, c, "exportSpki", "(ZI[B)[B");
    if (java_failed(env))
        return;
    g_key_bridge = (*env)->NewGlobalRef(env, c);
}

static struct cf_data *data_from_java(JNIEnv *env, jbyteArray a)
{
    if (!a)
        return NULL;
    jsize n = (*env)->GetArrayLength(env, a);
    jbyte *bytes = (*env)->GetByteArrayElements(env, a, NULL);
    struct cf_data *d = cf_data_create(bytes, (size_t)n);
    (*env)->ReleaseByteArrayElements(env, a, bytes, JNI_ABORT);
    return d;
}
#endif

static struct sec_key *SecKeyCreateRandomKey(struct cf_dictionary *attributes, struct cf_error **error)
{
    if (error)
        *error = NULL;
    void *type = cf_dictionary_get(attributes, kSecAttrKeyType);
    int ec = cf_equal(type, kSecAttrKeyTypeEC);
    int bits = (int)cf_number_int(cf_dictionary_get(attributes, kSecAttrKeySizeInBits));
    if (!ec && !cf_equal(type, kSecAttrKeyTypeRSA)) {
        set_cf_error(error, errSecParam, "SecKeyCreateRandomKey: unsupported key type");
        return NULL;
    }
    struct sec_key *key = NULL;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env) {
        (*env)->PushLocalFrame(env, 8);
        jbyteArray repr = (*env)->CallStaticObjectMethod(env, g_key_bridge, m_key_generate, (jboolean)ec, (jint)bits);
        if (!java_failed(env) && repr) {
            struct cf_data *d = data_from_java(env, repr);
            key = new_key(ec, 1, bits, d->bytes, d->length);
            cf_release(d);
        }
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    if (!key)
        set_cf_error(error, errSecUnimplemented, "SecKeyCreateRandomKey failed");
    else
        macho_log("sec", "generated %s %d key", ec ? "EC" : "RSA", bits);
    return key;
}

static struct sec_key *SecKeyCopyPublicKey(struct sec_key *key)
{
    if (cf_type_of(key) != CF_TYPE_SEC_KEY)
        return NULL;
    if (!key->is_private)
        return cf_retain(key);
    if (key->ec) {
        size_t n = (key->repr->length - 1) / 3;
        return new_key(1, 0, key->bits, key->repr->bytes, 1 + 2 * n);
    }
    struct sec_key *pub = NULL;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env) {
        (*env)->PushLocalFrame(env, 8);
        jbyteArray repr = (*env)->CallStaticObjectMethod(env, g_key_bridge, m_key_rsa_public,
                                                         java_bytes(env, key->repr->bytes, key->repr->length));
        if (!java_failed(env) && repr) {
            struct cf_data *d = data_from_java(env, repr);
            pub = new_key(0, 0, key->bits, d->bytes, d->length);
            cf_release(d);
        }
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    return pub;
}

static struct cf_data *SecKeyCopyExternalRepresentation(struct sec_key *key, struct cf_error **error)
{
    if (error)
        *error = NULL;
    return cf_type_of(key) == CF_TYPE_SEC_KEY ? cf_retain(key->repr) : NULL;
}

static struct cf_dictionary *SecKeyCopyAttributes(struct sec_key *key)
{
    if (cf_type_of(key) != CF_TYPE_SEC_KEY)
        return NULL;
    struct cf_dictionary *d = cf_dictionary_create();
    struct cf_number *bits = cf_number_from_int(key->bits);
    cf_dictionary_set(d, kSecAttrKeyType, key->ec ? kSecAttrKeyTypeEC : kSecAttrKeyTypeRSA);
    cf_dictionary_set(d, kSecAttrKeySizeInBits, bits);
    cf_dictionary_set(d, kSecAttrKeyClass, key->is_private ? kSecAttrKeyClassPrivate : kSecAttrKeyClassPublic);
    cf_dictionary_set(d, kSecAttrIsExtractable, &cf_boolean_true);
    cf_dictionary_set(d, kSecAttrIsSensitive, &cf_boolean_false);
    cf_release(bits);
    return d;
}

static size_t SecKeyGetBlockSize(struct sec_key *key)
{
    return cf_type_of(key) == CF_TYPE_SEC_KEY ? (size_t)(key->bits + 7) / 8 : 0;
}

// Bit length of a PKCS#1 RSA key in DER, or 0 when the key cannot be read.
static int rsa_bits_of(const struct cf_data *data, int is_private)
{
    int bits = 0;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env) {
        (*env)->PushLocalFrame(env, 8);
        bits = (*env)->CallStaticIntMethod(env, g_key_bridge, m_key_rsa_bits,
                                           java_bytes(env, data->bytes, data->length), (jboolean)is_private);
        if (java_failed(env))
            bits = 0;
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    return bits;
}

static struct sec_key *SecKeyCreateWithData(struct cf_data *data, struct cf_dictionary *attributes, struct cf_error **error)
{
    if (error)
        *error = NULL;
    if (cf_type_of(data) != CF_TYPE_DATA || !data->length) {
        set_cf_error(error, errSecParam, "SecKeyCreateWithData: no data");
        return NULL;
    }
    int ec = cf_equal(cf_dictionary_get(attributes, kSecAttrKeyType), kSecAttrKeyTypeEC);
    int is_private = cf_equal(cf_dictionary_get(attributes, kSecAttrKeyClass), kSecAttrKeyClassPrivate);
    int bits = 0;
    if (ec) {
        if (data->bytes[0] != 4) {
            set_cf_error(error, errSecParam, "SecKeyCreateWithData: not an uncompressed EC point");
            return NULL;
        }
        bits = ec_bits_for_field((data->length - 1) / (is_private ? 3 : 2));
    } else {
        bits = rsa_bits_of(data, is_private);
        if (!bits) {
            set_cf_error(error, errSecParam, "SecKeyCreateWithData: unreadable RSA key");
            return NULL;
        }
    }
    return new_key(ec, is_private, bits, data->bytes, data->length);
}

static struct cf_data *SecKeyCreateSignature(struct sec_key *key, struct cf_string *algorithm, struct cf_data *data,
                                             struct cf_error **error)
{
    if (error)
        *error = NULL;
    if (cf_type_of(key) != CF_TYPE_SEC_KEY || !key->is_private || cf_type_of(algorithm) != CF_TYPE_STRING ||
        cf_type_of(data) != CF_TYPE_DATA) {
        set_cf_error(error, errSecParam, "SecKeyCreateSignature: bad arguments");
        return NULL;
    }
    struct cf_data *signature = NULL;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env) {
        (*env)->PushLocalFrame(env, 8);
        jbyteArray out = (*env)->CallStaticObjectMethod(
            env, g_key_bridge, m_key_sign, (jboolean)key->ec, (jint)key->bits,
            java_bytes(env, key->repr->bytes, key->repr->length), (*env)->NewStringUTF(env, algorithm->utf8),
            java_bytes(env, data->bytes, data->length));
        if (!java_failed(env))
            signature = data_from_java(env, out);
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    if (!signature)
        set_cf_error(error, errSecUnimplemented, "SecKeyCreateSignature failed");
    return signature;
}

// The public key of a certificate, or NULL.
static struct sec_key *certificate_public_key(struct sec_certificate *cert)
{
    struct sec_key *key = NULL;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env && cf_type_of(cert) == CF_TYPE_SEC_CERTIFICATE) {
        (*env)->PushLocalFrame(env, 8);
        jintArray info = (*env)->NewIntArray(env, 2);
        jbyteArray repr = (*env)->CallStaticObjectMethod(env, g_key_bridge, m_key_cert_public,
                                                         java_bytes(env, cert->der->bytes, cert->der->length), info);
        if (!java_failed(env) && repr) {
            jint values[2];
            (*env)->GetIntArrayRegion(env, info, 0, 2, values);
            struct cf_data *d = data_from_java(env, repr);
            key = new_key(values[0], 0, values[1], d->bytes, d->length);
            cf_release(d);
        }
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    return key;
}

static struct sec_key *SecCertificateCopyKey(struct sec_certificate *cert) { return certificate_public_key(cert); }

static OSStatus SecIdentityCopyCertificate(struct sec_identity *identity, struct sec_certificate **out)
{
    if (cf_type_of(identity) != CF_TYPE_SEC_IDENTITY || !out)
        return errSecParam;
    *out = cf_retain(identity->cert);
    return noErr;
}

static OSStatus SecIdentityCopyPrivateKey(struct sec_identity *identity, struct sec_key **out)
{
    if (cf_type_of(identity) != CF_TYPE_SEC_IDENTITY || !out)
        return errSecParam;
    *out = cf_retain(identity->key);
    return noErr;
}

static struct sec_keychain *new_keychain(void)
{
    struct sec_keychain *k = cf_alloc("SecKeychain", sizeof *k);
    k->keys = cf_array_create();
    k->certs = cf_array_create();
    return k;
}

static OSStatus SecKeychainCreate(const char *path, uint32_t password_len, const void *password, bool prompt,
                                  void *access, struct sec_keychain **out)
{
    (void)path; (void)password_len; (void)password; (void)prompt; (void)access;
    if (!out)
        return errSecParam;
    *out = new_keychain();
    return noErr;
}

static OSStatus SecKeychainOpen(const char *path, struct sec_keychain **out)
{
    (void)path;
    if (!out)
        return errSecParam;
    *out = new_keychain();
    return noErr;
}

static OSStatus SecKeychainCopyDefault(struct sec_keychain **out)
{
    static struct sec_keychain *g_default;
    if (!out)
        return errSecParam;
    if (!g_default)
        g_default = new_keychain();
    *out = cf_retain(g_default);
    return noErr;
}

static OSStatus SecKeychainDelete(struct sec_keychain *k) { (void)k; return noErr; }
static OSStatus SecKeychainUnlock(struct sec_keychain *k, uint32_t len, const void *password, bool use)
{
    (void)k; (void)len; (void)password; (void)use;
    return noErr;
}
static OSStatus SecKeychainSetSettings(struct sec_keychain *k, const void *settings) { (void)k; (void)settings; return noErr; }

static OSStatus SecKeychainItemCopyKeychain(struct sec_key *item, struct sec_keychain **out)
{
    if (!out)
        return errSecParam;
    *out = NULL;
    if (cf_type_of(item) == CF_TYPE_SEC_KEY && item->keychain) {
        *out = cf_retain(item->keychain);
        return noErr;
    }
    return errSecNoSuchKeychain;
}

static OSStatus SecCertificateAddToKeychain(struct sec_certificate *cert, struct sec_keychain *keychain)
{
    if (cf_type_of(cert) != CF_TYPE_SEC_CERTIFICATE)
        return errSecParam;
    if (cf_type_of(keychain) == CF_TYPE_SEC_KEYCHAIN)
        cf_array_append(keychain->certs, cert);
    return noErr;
}

#ifdef HYPRISM_MACHO_LIBRARY
static const char *passphrase_of(const struct key_params *params)
{
    return params && cf_type_of(params->passphrase) == CF_TYPE_STRING ? params->passphrase->utf8 : "";
}
#endif

// Private keys export as a PBES2 EncryptedPrivateKeyInfo (kSecFormatWrappedPKCS8) and
// public keys as a SubjectPublicKeyInfo, which is what .NET parses back. The same
// export moves a key into the temporary keychain (AddKeyToKeychain).
static OSStatus SecItemExport(void *item, uint32_t format, uint32_t flags, const struct key_params *params,
                              struct cf_data **out)
{
    (void)flags;
    if (!out)
        return errSecParam;
    *out = NULL;
    if (cf_type_of(item) == CF_TYPE_SEC_CERTIFICATE && (format == kSecFormatX509Cert || format == 0)) {
        *out = cf_retain(((struct sec_certificate *)item)->der);
        return noErr;
    }
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env && cf_type_of(item) == CF_TYPE_SEC_KEY) {
        struct sec_key *k = item;
        (*env)->PushLocalFrame(env, 8);
        jbyteArray repr = java_bytes(env, k->repr->bytes, k->repr->length);
        jbyteArray der = k->is_private
                             ? (*env)->CallStaticObjectMethod(env, g_key_bridge, m_key_export_epki, (jboolean)k->ec,
                                                              (jint)k->bits, repr,
                                                              (*env)->NewStringUTF(env, passphrase_of(params)))
                             : (*env)->CallStaticObjectMethod(env, g_key_bridge, m_key_export_spki, (jboolean)k->ec,
                                                              (jint)k->bits, repr);
        if (!java_failed(env))
            *out = data_from_java(env, der);
        (*env)->PopLocalFrame(env, NULL);
        if (*out)
            return noErr;
    }
#endif
    macho_log("sec", "SecItemExport: unsupported item or format %u", format);
    return errSecUnimplemented;
}

static struct sec_key *import_encrypted_pkcs8(const struct cf_data *data, const struct key_params *params)
{
    struct sec_key *key = NULL;
#ifdef HYPRISM_MACHO_LIBRARY
    JNIEnv *env = g_key_bridge ? java_env() : NULL;
    if (env) {
        (*env)->PushLocalFrame(env, 8);
        jintArray info = (*env)->NewIntArray(env, 2);
        jbyteArray repr = (*env)->CallStaticObjectMethod(env, g_key_bridge, m_key_import_epki,
                                                         java_bytes(env, data->bytes, data->length),
                                                         (*env)->NewStringUTF(env, passphrase_of(params)), info);
        if (!java_failed(env) && repr) {
            jint values[2];
            (*env)->GetIntArrayRegion(env, info, 0, 2, values);
            struct cf_data *d = data_from_java(env, repr);
            key = new_key(values[0], 1, values[1], d->bytes, d->length);
            cf_release(d);
        }
        (*env)->PopLocalFrame(env, NULL);
    }
#endif
    return key;
}

static int base64_value(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

// Appends every certificate of a PEM sequence to items.
static void import_pem_certificates(const uint8_t *data, size_t len, struct cf_array *items)
{
    static const char begin[] = "-----BEGIN CERTIFICATE-----", end_marker[] = "-----END CERTIFICATE-----";
    char *text = strndup((const char *)data, len);
    for (char *p = strstr(text, begin); p; p = strstr(p, begin)) {
        p += sizeof begin - 1;
        char *end = strstr(p, end_marker);
        if (!end)
            break;
        uint8_t *der = malloc((size_t)(end - p));
        size_t n = 0;
        unsigned acc = 0;
        int bits = 0;
        for (char *q = p; q < end; q++) {
            int v = base64_value(*q);
            if (v < 0)
                continue;
            acc = (acc << 6) | (unsigned)v;
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                der[n++] = (uint8_t)(acc >> bits);
            }
        }
        struct sec_certificate *cert = certificate_from_der(der, n);
        if (cert) {
            cf_array_append(items, cert);
            cf_release(cert);
        }
        free(der);
        p = end;
    }
    free(text);
}

static OSStatus SecItemImport(struct cf_data *data, void *extension, uint32_t *format, uint32_t *type, uint32_t flags,
                              const struct key_params *params, struct sec_keychain *keychain, struct cf_array **out)
{
    (void)extension; (void)flags;
    if (out)
        *out = NULL;
    if (cf_type_of(data) != CF_TYPE_DATA)
        return errSecParam;
    struct cf_array *items = cf_array_create();
    const uint8_t *b = data->bytes;
    size_t len = data->length;
    if ((format && *format == kSecFormatWrappedPKCS8) ||
        (type && *type == kSecItemTypePrivateKey && !(format && *format == kSecFormatOpenSSL))) {
        struct sec_key *k = import_encrypted_pkcs8(data, params);
        if (k) {
            if (cf_type_of(keychain) == CF_TYPE_SEC_KEYCHAIN) {
                k->keychain = keychain;
                cf_array_append(keychain->keys, k);
            }
            cf_array_append(items, k);
            cf_release(k);
            if (type)
                *type = kSecItemTypePrivateKey;
        }
    } else if (format && *format == kSecFormatOpenSSL && len > 1 && b[0] == 0x30) {
        // Unencrypted PKCS#1 RSAPrivateKey.
        int bits = rsa_bits_of(data, 1);
        struct sec_key *k = bits ? new_key(0, 1, bits, b, len) : NULL;
        if (k) {
            if (cf_type_of(keychain) == CF_TYPE_SEC_KEYCHAIN) {
                k->keychain = keychain;
                cf_array_append(keychain->keys, k);
            }
            cf_array_append(items, k);
            cf_release(k);
            if (type)
                *type = kSecItemTypePrivateKey;
        }
    } else if (len > 10 && memcmp(b, "-----BEGIN", 10) == 0) {
        import_pem_certificates(b, len, items);
        if (type)
            *type = kSecItemTypeCertificate;
    } else if (len > 1 && b[0] == 0x30 && (!format || *format == kSecFormatX509Cert || *format == 0)) {
        struct sec_certificate *cert = certificate_from_der(b, len);
        if (cert) {
            cf_array_append(items, cert);
            cf_release(cert);
        }
        if (type)
            *type = kSecItemTypeCertificate;
    }
    if (!items->count) {
        cf_release(items);
        macho_log("sec", "SecItemImport: unsupported data (format %u, %zu bytes)", format ? *format : 0, len);
        return errSecUnknownFormat;
    }
    if (format && !*format)
        *format = kSecFormatX509Cert;
    if (out)
        *out = items;
    else
        cf_release(items);
    return noErr;
}

static int keys_match(struct sec_key *private_key, struct sec_key *public_key)
{
    if (private_key->ec != public_key->ec)
        return 0;
    struct sec_key *pub = SecKeyCopyPublicKey(private_key);
    int match = pub && pub->repr->length == public_key->repr->length &&
                memcmp(pub->repr->bytes, public_key->repr->bytes, pub->repr->length) == 0;
    cf_release(pub);
    return match;
}

// Finds the identity for a certificate among the keys of the searched keychains.
static struct sec_identity *find_identity(struct sec_certificate *cert, struct cf_array *keychains)
{
    struct sec_key *cert_key = certificate_public_key(cert);
    if (!cert_key)
        return NULL;
    struct sec_identity *identity = NULL;
    for (size_t i = 0; keychains && !identity && i < keychains->count; i++) {
        struct sec_keychain *k = keychains->items[i];
        if (cf_type_of(k) != CF_TYPE_SEC_KEYCHAIN)
            continue;
        for (size_t j = 0; !identity && j < k->keys->count; j++) {
            struct sec_key *key = k->keys->items[j];
            if (key->is_private && keys_match(key, cert_key)) {
                identity = cf_alloc("SecIdentity", sizeof *identity);
                identity->cert = cf_retain(cert);
                identity->key = cf_retain(key);
            }
        }
    }
    cf_release(cert_key);
    return identity;
}

static OSStatus SecItemCopyMatching(struct cf_dictionary *query, void **result)
{
    if (result)
        *result = NULL;
    if (!cf_equal(cf_dictionary_get(query, kSecClass), kSecClassIdentity))
        return errSecItemNotFound;
    struct cf_array *certs = cf_dictionary_get(query, kSecMatchItemList);
    struct cf_array *keychains = cf_dictionary_get(query, kSecMatchSearchList);
    int all = cf_equal(cf_dictionary_get(query, kSecMatchLimit), kSecMatchLimitAll);
    struct cf_array *found = cf_array_create();
    for (size_t i = 0; cf_type_of(certs) == CF_TYPE_ARRAY && i < certs->count; i++) {
        if (cf_type_of(certs->items[i]) != CF_TYPE_SEC_CERTIFICATE)
            continue;
        struct sec_identity *identity = find_identity(certs->items[i], cf_type_of(keychains) == CF_TYPE_ARRAY ? keychains : NULL);
        if (identity) {
            cf_array_append(found, identity);
            cf_release(identity);
        }
    }
    if (!found->count) {
        cf_release(found);
        return errSecItemNotFound;
    }
    if (result)
        *result = all ? (void *)cf_retain(found) : cf_retain(found->items[0]);
    cf_release(found);
    return noErr;
}

static OSStatus SecItemDelete(struct cf_dictionary *query) { (void)query; return noErr; }

// No user trust settings exist on Android; the platform store decides trust.
static OSStatus SecTrustSettingsCopyCertificates(int domain, struct cf_array **out)
{
    (void)domain;
    if (out)
        *out = NULL;
    return errSecNoTrustSettings;
}

static OSStatus SecTrustSettingsCopyTrustSettings(struct sec_certificate *cert, int domain, struct cf_array **out)
{
    (void)cert; (void)domain;
    if (out)
        *out = NULL;
    return errSecNoTrustSettings;
}

// ---------------------------------------------------------------------------
// Errors, proxies and lifetime

static struct cf_string *SecCopyErrorMessageString(OSStatus status, void *reserved)
{
    (void)reserved;
    const char *text;
    switch (status) {
    case errSecNotTrusted: text = "The certificate is not trusted."; break;
    case errSecCertificateExpired: text = "The certificate is not valid at this time."; break;
    case errSSLPeerHandshakeFail:
    case errSSLProtocol:
    case errSSLInternal: text = t_tls_error[0] ? t_tls_error : "TLS failure."; break;
    case errSSLClosedAbort: text = "The TLS connection was closed."; break;
    default: text = NULL; break;
    }
    char message[320];
    snprintf(message, sizeof message, "%s (OSStatus %d)", text ? text : "Security error", status);
    return cf_string_from_cstr(message);
}

// No system proxy on Android; the client then connects directly.
static struct cf_dictionary *CFNetworkCopySystemProxySettings(void) { return cf_dictionary_create(); }

void security_destroy(struct cf_object *o)
{
    switch (cf_type_of(o)) {
    case CF_TYPE_SEC_CERTIFICATE:
        cf_release(((struct sec_certificate *)o)->der);
        break;
    case CF_TYPE_SEC_TRUST: {
        struct sec_trust *t = (struct sec_trust *)o;
        cf_release(t->certs);
        cf_release(t->anchors);
        cf_release(t->chain);
        break;
    }
    case CF_TYPE_SEC_KEY:
        cf_release(((struct sec_key *)o)->repr);
        break;
    case CF_TYPE_SEC_IDENTITY:
        cf_release(((struct sec_identity *)o)->cert);
        cf_release(((struct sec_identity *)o)->key);
        break;
    case CF_TYPE_SEC_KEYCHAIN:
        cf_release(((struct sec_keychain *)o)->keys);
        cf_release(((struct sec_keychain *)o)->certs);
        break;
    case CF_TYPE_SSL_CONTEXT: {
        struct ssl_context *c = (struct ssl_context *)o;
#ifdef HYPRISM_MACHO_LIBRARY
        JNIEnv *env = c->bridge ? java_env() : NULL;
        if (env)
            (*env)->DeleteGlobalRef(env, c->bridge);
#endif
        free(c->peer_name);
        cf_release(c->alpn);
        cf_release(c->alpn_selected);
        cf_release(c->peer_certs);
        pthread_mutex_destroy(&c->lock);
        break;
    }
    default:
        break;
    }
}

#define F(name) { "_" #name, (void *)name }
const struct shim security_shims[] = {
    F(SecCertificateCreateWithData), F(SecCertificateCopyData), F(SecCertificateGetTypeID),
    F(SecCertificateCopySubjectSummary), F(SecIdentityGetTypeID), F(SecKeyGetTypeID),
    F(SecPolicyCreateBasicX509), F(SecPolicyCreateRevocation),
    F(SecTrustCreateWithCertificates), F(SecTrustSetAnchorCertificates), F(SecTrustSetVerifyDate),
    F(SecTrustSetNetworkFetchAllowed), F(SecTrustEvaluateWithError), F(SecTrustGetTrustResult),
    F(SecTrustGetCertificateCount), F(SecTrustGetCertificateAtIndex), F(SecTrustCopyResult),
    F(SecCopyErrorMessageString),
    F(SSLCreateContext), F(SSLSetConnection), F(SSLSetIOFuncs), F(SSLSetPeerDomainName),
    F(SSLGetPeerDomainNameLength), F(SSLSetProtocolVersionMin), F(SSLSetProtocolVersionMax),
    F(SSLSetALPNProtocols), F(SSLCopyALPNProtocols), F(SSLSetSessionOption), F(SSLSetEnabledCiphers),
    F(SSLSetCertificate), F(SSLSetClientSideAuthenticate), F(SSLSetCertificateAuthorities),
    F(SSLCopyDistinguishedNames), F(SSLGetSessionState), F(SSLGetNegotiatedProtocolVersion),
    F(SSLGetNegotiatedCipher), F(SSLCopyPeerTrust), F(SSLHandshake), F(SSLRead), F(SSLWrite),
    F(CFNetworkCopySystemProxySettings),
    F(SecKeyCreateRandomKey), F(SecKeyCopyPublicKey), F(SecKeyCopyExternalRepresentation), F(SecKeyCopyAttributes),
    F(SecKeyGetBlockSize), F(SecKeyCreateWithData), F(SecKeyCreateSignature), F(SecCertificateCopyKey),
    F(SecIdentityCopyCertificate), F(SecIdentityCopyPrivateKey), F(SecKeychainCreate), F(SecKeychainOpen),
    F(SecKeychainCopyDefault), F(SecKeychainDelete), F(SecKeychainUnlock), F(SecKeychainSetSettings),
    F(SecKeychainItemCopyKeychain), F(SecCertificateAddToKeychain), F(SecItemExport), F(SecItemImport),
    F(SecItemCopyMatching), F(SecItemDelete), F(SecTrustSettingsCopyCertificates), F(SecTrustSettingsCopyTrustSettings),
#define K(name) { "_" #name, &name }
    K(kSecAttrKeyType), K(kSecAttrKeySizeInBits), K(kSecAttrKeyClass), K(kSecAttrKeyClassPublic),
    K(kSecAttrKeyClassPrivate), K(kSecAttrKeyTypeRSA), { "_kSecAttrKeyTypeEC", &kSecAttrKeyTypeEC },
    { "_kSecAttrKeyTypeECSECPrimeRandom", &kSecAttrKeyTypeEC }, K(kSecAttrIsExtractable), K(kSecAttrIsSensitive),
    K(kSecClass), K(kSecClassKey), K(kSecClassCertificate), K(kSecClassIdentity), K(kSecReturnRef), K(kSecMatchLimit),
    K(kSecMatchLimitAll), K(kSecMatchSearchList), K(kSecMatchItemList), K(kSecUseDataProtectionKeychain),
    K(kSecInputIsAttributeName), K(kSecInputIsDigest), K(kSecTransformInputAttributeName),
    K(kSecKeyAlgorithmECDSASignatureDigestX962), K(kSecKeyAlgorithmRSASignatureRaw),
    K(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA1), K(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA256),
    K(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA384), K(kSecKeyAlgorithmRSASignatureDigestPKCS1v15SHA512),
    K(kSecKeyAlgorithmRSASignatureDigestPSSSHA1), K(kSecKeyAlgorithmRSASignatureDigestPSSSHA256),
    K(kSecKeyAlgorithmRSASignatureDigestPSSSHA384), K(kSecKeyAlgorithmRSASignatureDigestPSSSHA512),
#undef K
};
const size_t security_shims_count = SHIM_COUNT(security_shims);
