// Copyright (C) 2026 Hyprism Launcher
// SPDX-License-Identifier: GPL-3.0-only

package org.hyprism.game;

import java.io.ByteArrayInputStream;
import java.io.ByteArrayOutputStream;
import java.math.BigInteger;
import java.security.AlgorithmParameters;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.MessageDigest;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.Signature;
import java.security.cert.CertificateFactory;
import java.security.cert.X509Certificate;
import java.security.interfaces.ECPrivateKey;
import java.security.interfaces.ECPublicKey;
import java.security.interfaces.RSAPrivateCrtKey;
import java.security.interfaces.RSAPublicKey;
import java.security.spec.ECGenParameterSpec;
import java.security.spec.ECParameterSpec;
import java.security.spec.ECPrivateKeySpec;
import java.security.spec.PKCS8EncodedKeySpec;
import java.util.Arrays;

import javax.crypto.Cipher;
import javax.crypto.spec.IvParameterSpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Key generation and signing for the loader's SecKey shims (security.c), with the
 * platform crypto provider. Keys travel as Apple's external representations:
 * EC private 04||X||Y||K, EC public 04||X||Y, RSA private PKCS#1 RSAPrivateKey and
 * RSA public PKCS#1 RSAPublicKey.
 */
final class KeyBridge {
    private KeyBridge() {}

    /** Generates a key pair and returns the private key representation. */
    static byte[] generate(boolean ec, int bits) throws Exception {
        KeyPairGenerator generator = KeyPairGenerator.getInstance(ec ? "EC" : "RSA");
        if (ec)
            generator.initialize(new ECGenParameterSpec(curveName(bits)));
        else
            generator.initialize(bits);
        KeyPair pair = generator.generateKeyPair();
        if (ec) {
            ECPublicKey pub = (ECPublicKey) pair.getPublic();
            ECPrivateKey priv = (ECPrivateKey) pair.getPrivate();
            int n = fieldBytes(bits);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(4);
            out.write(fixed(pub.getW().getAffineX(), n));
            out.write(fixed(pub.getW().getAffineY(), n));
            out.write(fixed(priv.getS(), n));
            return out.toByteArray();
        }
        RSAPrivateCrtKey priv = (RSAPrivateCrtKey) pair.getPrivate();
        return rsaPrivatePkcs1(priv);
    }

    /** Returns the public representation for a private RSA representation. */
    static byte[] rsaPublicFromPrivate(byte[] pkcs1) throws Exception {
        RSAPrivateCrtKey priv = (RSAPrivateCrtKey) rsaPrivate(pkcs1);
        return rsaPublicPkcs1(priv.getModulus(), priv.getPublicExponent());
    }

    static int rsaBits(byte[] repr, boolean isPrivate) throws Exception {
        if (isPrivate)
            return ((RSAPrivateCrtKey) rsaPrivate(repr)).getModulus().bitLength();
        return new BigInteger(1, derIntegers(repr)[0]).bitLength();
    }

    /**
     * Signs data with a private key representation. algorithm is the Apple
     * SecKeyAlgorithm identifier; digest variants receive the digest as data.
     */
    static byte[] sign(boolean ec, int bits, byte[] priv, String algorithm, byte[] data) throws Exception {
        if (ec) {
            if (!algorithm.startsWith("algid:sign:ECDSA:digest-X962"))
                throw new IllegalArgumentException("unsupported algorithm " + algorithm);
            int n = fieldBytes(bits);
            byte[] k = Arrays.copyOfRange(priv, 1 + 2 * n, 1 + 3 * n);
            Signature signature = Signature.getInstance("NONEwithECDSA");
            signature.initSign(KeyFactory.getInstance("EC").generatePrivate(
                    new ECPrivateKeySpec(new BigInteger(1, k), curve(bits))));
            signature.update(data);
            return signature.sign();
        }
        PrivateKey key = rsaPrivate(priv);
        if (algorithm.equals("algid:sign:RSA:raw")) {
            Cipher cipher = Cipher.getInstance("RSA/ECB/NoPadding");
            cipher.init(Cipher.ENCRYPT_MODE, key);
            return cipher.doFinal(data);
        }
        if (algorithm.startsWith("algid:sign:RSA:digest-PKCS1v15:")) {
            byte[] prefix = digestInfoPrefix(algorithm.substring("algid:sign:RSA:digest-PKCS1v15:".length()));
            byte[] info = new byte[prefix.length + data.length];
            System.arraycopy(prefix, 0, info, 0, prefix.length);
            System.arraycopy(data, 0, info, prefix.length, data.length);
            Signature signature = Signature.getInstance("NONEwithRSA");
            signature.initSign(key);
            signature.update(info);
            return signature.sign();
        }
        throw new IllegalArgumentException("unsupported algorithm " + algorithm);
    }

    /**
     * Returns the public key representation of a DER certificate; info receives
     * {1 for EC or 0 for RSA, key size in bits}.
     */
    static byte[] certificatePublicKey(byte[] der, int[] info) throws Exception {
        X509Certificate cert = (X509Certificate) CertificateFactory.getInstance("X.509")
                .generateCertificate(new ByteArrayInputStream(der));
        PublicKey key = cert.getPublicKey();
        if (key instanceof ECPublicKey) {
            ECPublicKey ec = (ECPublicKey) key;
            int bits = ec.getParams().getCurve().getField().getFieldSize();
            int n = fieldBytes(bits);
            ByteArrayOutputStream out = new ByteArrayOutputStream();
            out.write(4);
            out.write(fixed(ec.getW().getAffineX(), n));
            out.write(fixed(ec.getW().getAffineY(), n));
            info[0] = 1;
            info[1] = bits;
            return out.toByteArray();
        }
        if (key instanceof RSAPublicKey) {
            RSAPublicKey rsa = (RSAPublicKey) key;
            info[0] = 0;
            info[1] = rsa.getModulus().bitLength();
            return rsaPublicPkcs1(rsa.getModulus(), rsa.getPublicExponent());
        }
        return null;
    }

    /**
     * Exports a private key as an EncryptedPrivateKeyInfo (PBES2 with PBKDF2-HMAC-SHA256
     * and AES-256-CBC), the format SecItemExport returns for kSecFormatWrappedPKCS8.
     */
    static byte[] exportEncryptedPkcs8(boolean ec, int bits, byte[] repr, String password) throws Exception {
        byte[] info = privateKeyInfo(ec, bits, repr);
        SecureRandom random = new SecureRandom();
        byte[] salt = new byte[16], iv = new byte[16];
        random.nextBytes(salt);
        random.nextBytes(iv);
        int iterations = 2048;
        Cipher cipher = Cipher.getInstance("AES/CBC/PKCS5Padding");
        cipher.init(Cipher.ENCRYPT_MODE, pbkdf2(password, salt, iterations), new IvParameterSpec(iv));
        byte[] encrypted = cipher.doFinal(info);
        byte[] kdf = derSequence(hex("06092a864886f70d01050c"), // PBKDF2
                derSequence(derTag(0x04, salt), derInteger(BigInteger.valueOf(iterations)),
                        derSequence(hex("06082a864886f70d0209"), hex("0500")))); // hmacWithSHA256
        byte[] scheme = derSequence(hex("060960864801650304012a"), derTag(0x04, iv)); // aes256-CBC
        byte[] algorithm = derSequence(hex("06092a864886f70d01050d"), derSequence(kdf, scheme)); // PBES2
        return derSequence(algorithm, derTag(0x04, encrypted));
    }

    /**
     * Reads an EncryptedPrivateKeyInfo written by exportEncryptedPkcs8 and returns
     * the private key representation; info receives {1 for EC or 0 for RSA, bits}.
     */
    static byte[] importEncryptedPkcs8(byte[] der, String password, int[] info) throws Exception {
        Der epki = new Der(der).sequence();
        Der pbes2 = epki.sequence();
        pbes2.skip(); // PBES2 OID
        Der params = pbes2.sequence();
        Der kdf = params.sequence();
        kdf.skip(); // PBKDF2 OID
        Der kdfParams = kdf.sequence();
        byte[] salt = kdfParams.next(0x04);
        int iterations = new BigInteger(1, kdfParams.next(0x02)).intValue();
        Der scheme = params.sequence();
        scheme.skip(); // AES-256-CBC OID
        byte[] iv = scheme.next(0x04);
        byte[] encrypted = epki.next(0x04);
        Cipher cipher = Cipher.getInstance("AES/CBC/PKCS5Padding");
        cipher.init(Cipher.DECRYPT_MODE, pbkdf2(password, salt, iterations), new IvParameterSpec(iv));
        byte[] plain = cipher.doFinal(encrypted);

        Der pki = new Der(plain).sequence();
        pki.next(0x02); // version
        Der algorithm = pki.sequence();
        byte[] oid = algorithm.next(0x06);
        byte[] key = pki.next(0x04);
        if (Arrays.equals(oid, hex("2a864886f70d010101"))) { // rsaEncryption
            info[0] = 0;
            info[1] = ((RSAPrivateCrtKey) rsaPrivate(key)).getModulus().bitLength();
            return key;
        }
        byte[] curveOid = algorithm.next(0x06);
        int bits = Arrays.equals(curveOid, hex("2a8648ce3d030107")) ? 256
                : Arrays.equals(curveOid, hex("2b81040022")) ? 384 : 521;
        Der ecKey = new Der(key).sequence();
        ecKey.next(0x02); // version
        byte[] k = ecKey.next(0x04);
        ecKey.next(0xa0); // parameters
        Der publicKey = new Der(ecKey.next(0xa1));
        byte[] point = publicKey.next(0x03);
        int n = fieldBytes(bits);
        byte[] repr = new byte[1 + 3 * n];
        System.arraycopy(point, 1, repr, 0, 1 + 2 * n); // skip the unused-bits byte
        byte[] fixedK = fixed(new BigInteger(1, k), n);
        System.arraycopy(fixedK, 0, repr, 1 + 2 * n, n);
        info[0] = 1;
        info[1] = bits;
        return repr;
    }

    /** Exports a public key as a DER SubjectPublicKeyInfo. */
    static byte[] exportSpki(boolean ec, int bits, byte[] publicRepr) {
        byte[] algorithm = ec ? derSequence(hex("06072a8648ce3d0201"), curveOid(bits))
                : derSequence(hex("06092a864886f70d0101010500"));
        byte[] bits0 = new byte[publicRepr.length + 1];
        System.arraycopy(publicRepr, 0, bits0, 1, publicRepr.length);
        return derSequence(algorithm, derTag(0x03, bits0));
    }

    // -------------------------------------------------------------------------

    // PBKDF2-HMAC-SHA256 for a 32-byte key, written out because the platform
    // SecretKeyFactory and Mac reject the empty passwords Apple's export accepts.
    private static SecretKeySpec pbkdf2(String password, byte[] salt, int iterations) throws Exception {
        byte[] key = password.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        MessageDigest sha = MessageDigest.getInstance("SHA-256");
        if (key.length > 64)
            key = sha.digest(key);
        byte[] ipad = new byte[64], opad = new byte[64];
        for (int i = 0; i < 64; i++) {
            byte k = i < key.length ? key[i] : 0;
            ipad[i] = (byte) (k ^ 0x36);
            opad[i] = (byte) (k ^ 0x5c);
        }
        byte[] block = new byte[salt.length + 4];
        System.arraycopy(salt, 0, block, 0, salt.length);
        block[salt.length + 3] = 1; // block index 1: one SHA-256 output is the whole key
        byte[] u = hmac(sha, ipad, opad, block);
        byte[] t = u.clone();
        for (int i = 1; i < iterations; i++) {
            u = hmac(sha, ipad, opad, u);
            for (int j = 0; j < t.length; j++)
                t[j] ^= u[j];
        }
        return new SecretKeySpec(t, "AES");
    }

    private static byte[] hmac(MessageDigest sha, byte[] ipad, byte[] opad, byte[] message) {
        sha.reset();
        sha.update(ipad);
        byte[] inner = sha.digest(message);
        sha.update(opad);
        return sha.digest(inner);
    }

    private static byte[] curveOid(int bits) {
        switch (bits) {
        case 256: return hex("06082a8648ce3d030107");
        case 384: return hex("06052b81040022");
        default: return hex("06052b81040023");
        }
    }

    /** PKCS#8 PrivateKeyInfo for a private key representation. */
    private static byte[] privateKeyInfo(boolean ec, int bits, byte[] repr) {
        if (!ec)
            return derSequence(derInteger(BigInteger.ZERO), derSequence(hex("06092a864886f70d0101010500")),
                    derTag(0x04, repr));
        int n = fieldBytes(bits);
        byte[] point = new byte[2 + 2 * n];
        System.arraycopy(repr, 0, point, 1, 1 + 2 * n);
        byte[] ecPrivateKey = derSequence(derInteger(BigInteger.ONE), derTag(0x04, Arrays.copyOfRange(repr, 1 + 2 * n, 1 + 3 * n)),
                derTag(0xa0, curveOid(bits)), derTag(0xa1, derTag(0x03, point)));
        return derSequence(derInteger(BigInteger.ZERO), derSequence(hex("06072a8648ce3d0201"), curveOid(bits)),
                derTag(0x04, ecPrivateKey));
    }

    /** Sequential reader over DER TLVs. */
    private static final class Der {
        private final byte[] data;
        private int pos;
        private final int end;

        Der(byte[] data) { this(data, 0, data.length); }

        private Der(byte[] data, int start, int end) {
            this.data = data;
            this.pos = start;
            this.end = end;
        }

        private int[] header(int expected) {
            if (pos >= end)
                throw new IllegalArgumentException("DER data ended early");
            int tag = data[pos] & 0xff;
            if (expected >= 0 && tag != expected)
                throw new IllegalArgumentException("unexpected DER tag " + tag);
            int[] p = { pos + 1 };
            int len = readLength(data, p);
            return new int[] { p[0], len };
        }

        Der sequence() {
            int[] h = header(0x30);
            pos = h[0] + h[1];
            return new Der(data, h[0], h[0] + h[1]);
        }

        byte[] next(int tag) {
            int[] h = header(tag);
            pos = h[0] + h[1];
            return Arrays.copyOfRange(data, h[0], h[0] + h[1]);
        }

        void skip() { next(-1); }
    }

    private static String curveName(int bits) {
        switch (bits) {
        case 256: return "secp256r1";
        case 384: return "secp384r1";
        case 521: return "secp521r1";
        default: throw new IllegalArgumentException("unsupported EC key size " + bits);
        }
    }

    private static int fieldBytes(int bits) { return (bits + 7) / 8; }

    private static ECParameterSpec curve(int bits) throws Exception {
        AlgorithmParameters params = AlgorithmParameters.getInstance("EC");
        params.init(new ECGenParameterSpec(curveName(bits)));
        return params.getParameterSpec(ECParameterSpec.class);
    }

    private static byte[] fixed(BigInteger value, int n) {
        byte[] raw = value.toByteArray();
        byte[] out = new byte[n];
        int copy = Math.min(raw.length, n);
        System.arraycopy(raw, raw.length - copy, out, n - copy, copy);
        return out;
    }

    private static byte[] digestInfoPrefix(String hash) {
        switch (hash) {
        case "SHA1": return hex("3021300906052b0e03021a05000414");
        case "SHA224": return hex("302d300d06096086480165030402040500041c");
        case "SHA256": return hex("3031300d060960864801650304020105000420");
        case "SHA384": return hex("3041300d060960864801650304020205000430");
        case "SHA512": return hex("3051300d060960864801650304020305000440");
        default: throw new IllegalArgumentException("unsupported digest " + hash);
        }
    }

    private static byte[] hex(String s) {
        byte[] out = new byte[s.length() / 2];
        for (int i = 0; i < out.length; i++)
            out[i] = (byte) Integer.parseInt(s.substring(2 * i, 2 * i + 2), 16);
        return out;
    }

    // Minimal DER for PKCS#1 and PKCS#8.

    private static void derLength(ByteArrayOutputStream out, int len) {
        if (len < 0x80) {
            out.write(len);
        } else if (len < 0x100) {
            out.write(0x81);
            out.write(len);
        } else if (len < 0x10000) {
            out.write(0x82);
            out.write(len >> 8);
            out.write(len);
        } else {
            out.write(0x83);
            out.write(len >> 16);
            out.write(len >> 8);
            out.write(len);
        }
    }

    private static byte[] derTag(int tag, byte[] content) {
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.write(tag);
        derLength(out, content.length);
        out.write(content, 0, content.length);
        return out.toByteArray();
    }

    private static byte[] derSequence(byte[]... parts) {
        ByteArrayOutputStream body = new ByteArrayOutputStream();
        for (byte[] p : parts)
            body.write(p, 0, p.length);
        return derTag(0x30, body.toByteArray());
    }

    private static byte[] derInteger(BigInteger v) { return derTag(0x02, v.toByteArray()); }

    private static byte[] rsaPublicPkcs1(BigInteger n, BigInteger e) {
        return derSequence(derInteger(n), derInteger(e));
    }

    private static byte[] rsaPrivatePkcs1(RSAPrivateCrtKey k) {
        return derSequence(derInteger(BigInteger.ZERO), derInteger(k.getModulus()), derInteger(k.getPublicExponent()),
                derInteger(k.getPrivateExponent()), derInteger(k.getPrimeP()), derInteger(k.getPrimeQ()),
                derInteger(k.getPrimeExponentP()), derInteger(k.getPrimeExponentQ()), derInteger(k.getCrtCoefficient()));
    }

    private static PrivateKey rsaPrivate(byte[] pkcs1) throws Exception {
        byte[] algorithm = derSequence(hex("06092a864886f70d0101010500")); // rsaEncryption, NULL
        byte[] pkcs8 = derSequence(derInteger(BigInteger.ZERO), algorithm, derTag(0x04, pkcs1));
        return KeyFactory.getInstance("RSA").generatePrivate(new PKCS8EncodedKeySpec(pkcs8));
    }

    /** Reads the INTEGER values of a DER SEQUENCE. */
    private static byte[][] derIntegers(byte[] der) {
        java.util.ArrayList<byte[]> values = new java.util.ArrayList<>();
        int[] pos = { 0 };
        if (der[pos[0]++] != 0x30)
            throw new IllegalArgumentException("not a SEQUENCE");
        int end = readLength(der, pos) + pos[0];
        while (pos[0] < end) {
            int tag = der[pos[0]++];
            int len = readLength(der, pos);
            if (tag == 0x02)
                values.add(Arrays.copyOfRange(der, pos[0], pos[0] + len));
            pos[0] += len;
        }
        return values.toArray(new byte[0][]);
    }

    private static int readLength(byte[] der, int[] pos) {
        int first = der[pos[0]++] & 0xff;
        if (first < 0x80)
            return first;
        int len = 0;
        for (int i = 0; i < (first & 0x7f); i++)
            len = (len << 8) | (der[pos[0]++] & 0xff);
        return len;
    }
}
