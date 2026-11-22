// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import android.net.http.X509TrustManagerExtensions;
import android.os.Build;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.nio.ByteBuffer;
import java.security.KeyStore;
import java.security.cert.CertPathValidatorException;
import java.security.cert.Certificate;
import java.security.cert.CertificateExpiredException;
import java.security.cert.CertificateFactory;
import java.security.cert.CertificateNotYetValidException;
import java.security.cert.X509Certificate;
import java.util.Collections;
import java.util.List;

import javax.net.ssl.SNIHostName;
import javax.net.ssl.SSLContext;
import javax.net.ssl.SSLEngine;
import javax.net.ssl.SSLEngineResult;
import javax.net.ssl.SSLException;
import javax.net.ssl.SSLParameters;
import javax.net.ssl.TrustManager;
import javax.net.ssl.TrustManagerFactory;
import javax.net.ssl.X509TrustManager;

/**
 * TLS sessions and certificate trust for the loader's SecureTransport and SecTrust
 * shims (security.c). The engine moves bytes only: the native side does the I/O
 * through the client's callbacks.
 *
 * The engine accepts any server certificate because the .NET client validates the
 * peer chain itself after every handshake, through SecTrustEvaluateWithError, which
 * {@link #verify} implements with the platform trust store. This keeps the client's
 * own validation callbacks working as they do on macOS.
 */
final class TlsBridge {
    // Handshake results, mirrored in security.c.
    static final int DONE = 0;
    static final int NEED_INPUT = 1;
    static final int FAILED = -1;

    // verify() status codes, mirrored in security.c.
    static final int TRUSTED = 0;
    static final int UNTRUSTED = 1;
    static final int EXPIRED = 2;

    private static final ByteBuffer EMPTY = ByteBuffer.allocate(0);
    private static SSLContext sContext;

    private final SSLEngine engine;
    private ByteBuffer netIn = ByteBuffer.allocate(1 << 15);
    private ByteBuffer appIn = ByteBuffer.allocate(1 << 15);
    private final ByteArrayOutputStream out = new ByteArrayOutputStream();
    private boolean closed;
    private String error;

    TlsBridge(String host, String[] alpn, String[] protocols) throws Exception {
        engine = host != null ? context().createSSLEngine(host, 443) : context().createSSLEngine();
        engine.setUseClientMode(true);
        SSLParameters params = engine.getSSLParameters();
        if (host != null)
            params.setServerNames(Collections.singletonList(new SNIHostName(host)));
        if (alpn != null && alpn.length > 0 && Build.VERSION.SDK_INT >= 29)
            params.setApplicationProtocols(alpn);
        if (protocols != null && protocols.length > 0)
            params.setProtocols(protocols);
        engine.setSSLParameters(params);
        engine.beginHandshake();
    }

    private static synchronized SSLContext context() throws Exception {
        if (sContext == null) {
            X509TrustManager acceptAll = new X509TrustManager() {
                @Override public void checkClientTrusted(X509Certificate[] chain, String authType) {}
                @Override public void checkServerTrusted(X509Certificate[] chain, String authType) {}
                @Override public X509Certificate[] getAcceptedIssuers() { return new X509Certificate[0]; }
            };
            SSLContext context = SSLContext.getInstance("TLS");
            context.init(null, new TrustManager[] { acceptAll }, null);
            sContext = context;
        }
        return sContext;
    }

    /** Feeds received bytes and advances the handshake as far as they allow. */
    int handshake(byte[] input) {
        feed(input);
        try {
            for (;;) {
                switch (engine.getHandshakeStatus()) {
                case NEED_TASK:
                    runTasks();
                    break;
                case NEED_WRAP:
                    if (wrap(EMPTY).getStatus() == SSLEngineResult.Status.CLOSED)
                        return fail("closed during handshake");
                    break;
                case NEED_UNWRAP:
                case NEED_UNWRAP_AGAIN: {
                    SSLEngineResult result = unwrap();
                    if (result.getStatus() == SSLEngineResult.Status.BUFFER_UNDERFLOW)
                        return NEED_INPUT;
                    if (result.getStatus() == SSLEngineResult.Status.CLOSED)
                        return fail("closed during handshake");
                    break;
                }
                default:
                    return DONE;
                }
            }
        } catch (Exception e) {
            return fail(e.toString());
        }
    }

    /** Feeds received bytes and returns up to max decrypted bytes, or null on error. */
    byte[] read(byte[] input, int max) {
        feed(input);
        try {
            while (netIn.position() > 0 && !closed) {
                SSLEngineResult result = unwrap();
                if (result.getStatus() == SSLEngineResult.Status.CLOSED)
                    closed = true;
                // Post-handshake messages such as TLS 1.3 key updates may need an answer.
                while (engine.getHandshakeStatus() == SSLEngineResult.HandshakeStatus.NEED_TASK
                        || engine.getHandshakeStatus() == SSLEngineResult.HandshakeStatus.NEED_WRAP) {
                    if (engine.getHandshakeStatus() == SSLEngineResult.HandshakeStatus.NEED_TASK)
                        runTasks();
                    else
                        wrap(EMPTY);
                }
                if (result.getStatus() == SSLEngineResult.Status.BUFFER_UNDERFLOW
                        || (result.bytesConsumed() == 0 && result.bytesProduced() == 0))
                    break;
            }
        } catch (Exception e) {
            fail(e.toString());
            return null;
        }
        appIn.flip();
        byte[] data = new byte[Math.min(max, appIn.remaining())];
        appIn.get(data);
        appIn.compact();
        return data;
    }

    boolean write(byte[] data) {
        try {
            ByteBuffer src = ByteBuffer.wrap(data);
            while (src.hasRemaining())
                if (wrap(src).getStatus() == SSLEngineResult.Status.CLOSED) {
                    fail("closed");
                    return false;
                }
            return true;
        } catch (Exception e) {
            fail(e.toString());
            return false;
        }
    }

    byte[] takeOutput() {
        byte[] data = out.toByteArray();
        out.reset();
        return data;
    }

    boolean isClosed() { return closed; }

    String error() { return error; }

    String protocol() { return engine.getSession().getProtocol(); }

    String alpn() {
        return Build.VERSION.SDK_INT >= 29 ? engine.getApplicationProtocol() : null;
    }

    int cipherSuite() {
        switch (engine.getSession().getCipherSuite()) {
        case "TLS_AES_128_GCM_SHA256": return 0x1301;
        case "TLS_AES_256_GCM_SHA384": return 0x1302;
        case "TLS_CHACHA20_POLY1305_SHA256": return 0x1303;
        case "TLS_ECDHE_ECDSA_WITH_AES_128_GCM_SHA256": return 0xC02B;
        case "TLS_ECDHE_ECDSA_WITH_AES_256_GCM_SHA384": return 0xC02C;
        case "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256": return 0xC02F;
        case "TLS_ECDHE_RSA_WITH_AES_256_GCM_SHA384": return 0xC030;
        case "TLS_ECDHE_RSA_WITH_CHACHA20_POLY1305_SHA256": return 0xCCA8;
        case "TLS_ECDHE_ECDSA_WITH_CHACHA20_POLY1305_SHA256": return 0xCCA9;
        default: return 0;
        }
    }

    byte[][] peerCertificates() {
        try {
            Certificate[] certs = engine.getSession().getPeerCertificates();
            byte[][] der = new byte[certs.length][];
            for (int i = 0; i < certs.length; i++)
                der[i] = certs[i].getEncoded();
            return der;
        } catch (Exception e) {
            return new byte[0][];
        }
    }

    /**
     * Builds and validates a chain for certs[0] with the platform trust store, or
     * with anchors only when they are given. Returns the built chain from leaf to
     * root, or certs when validation fails; status[0] receives the verdict.
     */
    static byte[][] verify(byte[][] certs, byte[][] anchors, int[] status) {
        try {
            CertificateFactory factory = CertificateFactory.getInstance("X.509");
            X509Certificate[] chain = new X509Certificate[certs.length];
            for (int i = 0; i < certs.length; i++)
                chain[i] = (X509Certificate) factory.generateCertificate(new ByteArrayInputStream(certs[i]));

            KeyStore store = null;
            if (anchors != null) {
                store = KeyStore.getInstance(KeyStore.getDefaultType());
                store.load(null);
                for (int i = 0; i < anchors.length; i++)
                    store.setCertificateEntry("anchor" + i,
                            factory.generateCertificate(new ByteArrayInputStream(anchors[i])));
            }
            TrustManagerFactory tmf = TrustManagerFactory.getInstance(TrustManagerFactory.getDefaultAlgorithm());
            tmf.init(store);
            X509TrustManager trust = null;
            for (TrustManager tm : tmf.getTrustManagers())
                if (tm instanceof X509TrustManager)
                    trust = (X509TrustManager) tm;

            List<X509Certificate> built = new X509TrustManagerExtensions(trust).checkServerTrusted(chain, "GENERIC", null);
            byte[][] der = new byte[built.size()][];
            for (int i = 0; i < der.length; i++)
                der[i] = built.get(i).getEncoded();
            status[0] = TRUSTED;
            return der;
        } catch (Exception e) {
            status[0] = isExpiry(e) ? EXPIRED : UNTRUSTED;
            android.util.Log.w("HytaleLoader", "[tls] chain rejected: " + e);
            return certs;
        }
    }

    static String subjectSummary(byte[] der) {
        try {
            X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509")
                    .generateCertificate(new ByteArrayInputStream(der));
            String dn = cert.getSubjectX500Principal().getName();
            for (String part : dn.split(","))
                if (part.startsWith("CN="))
                    return part.substring(3);
            return dn;
        } catch (Exception e) {
            return null;
        }
    }

    private static boolean isExpiry(Throwable e) {
        for (; e != null; e = e.getCause()) {
            if (e instanceof CertificateExpiredException || e instanceof CertificateNotYetValidException)
                return true;
            if (e instanceof CertPathValidatorException) {
                CertPathValidatorException.Reason reason = ((CertPathValidatorException) e).getReason();
                if (reason == CertPathValidatorException.BasicReason.EXPIRED
                        || reason == CertPathValidatorException.BasicReason.NOT_YET_VALID)
                    return true;
            }
        }
        return false;
    }

    private int fail(String message) {
        error = message;
        return FAILED;
    }

    private void feed(byte[] input) {
        if (input == null || input.length == 0)
            return;
        if (netIn.remaining() < input.length) {
            ByteBuffer bigger = ByteBuffer.allocate(Math.max(netIn.capacity() * 2, netIn.position() + input.length));
            netIn.flip();
            bigger.put(netIn);
            netIn = bigger;
        }
        netIn.put(input);
    }

    private void runTasks() {
        Runnable task;
        while ((task = engine.getDelegatedTask()) != null)
            task.run();
    }

    private SSLEngineResult unwrap() throws SSLException {
        for (;;) {
            netIn.flip();
            SSLEngineResult result;
            try {
                result = engine.unwrap(netIn, appIn);
            } finally {
                netIn.compact();
            }
            if (result.getStatus() != SSLEngineResult.Status.BUFFER_OVERFLOW)
                return result;
            ByteBuffer bigger = ByteBuffer.allocate(appIn.capacity() + engine.getSession().getApplicationBufferSize());
            appIn.flip();
            bigger.put(appIn);
            appIn = bigger;
        }
    }

    private SSLEngineResult wrap(ByteBuffer src) throws SSLException {
        ByteBuffer dst = ByteBuffer.allocate(engine.getSession().getPacketBufferSize());
        for (;;) {
            SSLEngineResult result = engine.wrap(src, dst);
            if (result.getStatus() == SSLEngineResult.Status.BUFFER_OVERFLOW) {
                dst = ByteBuffer.allocate(dst.capacity() * 2);
                continue;
            }
            out.write(dst.array(), 0, dst.position());
            return result;
        }
    }
}
