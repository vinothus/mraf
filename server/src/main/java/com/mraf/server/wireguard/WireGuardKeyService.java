package com.mraf.server.wireguard;

import org.springframework.stereotype.Service;

import java.math.BigInteger;
import java.security.KeyFactory;
import java.security.KeyPair;
import java.security.KeyPairGenerator;
import java.security.PrivateKey;
import java.security.PublicKey;
import java.security.SecureRandom;
import java.security.interfaces.XECPrivateKey;
import java.security.interfaces.XECPublicKey;
import java.security.spec.NamedParameterSpec;
import java.security.spec.XECPrivateKeySpec;
import java.security.spec.XECPublicKeySpec;
import java.util.Base64;

import javax.crypto.KeyAgreement;

/**
 * Generates and derives Curve25519 keys in the base64 format used by
 * {@code wg(8)} and the WireGuard ESP32 client. Uses the JDK's native X25519
 * implementation, so no third party crypto dependency is required.
 */
@Service
public class WireGuardKeyService {
    private static final String X25519 = "X25519";
    private static final int KEY_BYTES = 32;
    private final SecureRandom random = new SecureRandom();

    public record KeyPairStrings(String privateKey, String publicKey) {
    }

    /** Generates a fresh WireGuard keypair. */
    public KeyPairStrings generate() {
        try {
            KeyPairGenerator generator = KeyPairGenerator.getInstance(X25519);
            KeyPair pair = generator.generateKeyPair();
            byte[] priv = ((XECPrivateKey) pair.getPrivate()).getScalar()
                    .orElseThrow(() -> new IllegalStateException("no X25519 scalar"));
            byte[] pub = encodeU(((XECPublicKey) pair.getPublic()).getU());
            return new KeyPairStrings(b64(priv), b64(pub));
        } catch (Exception e) {
            throw new IllegalStateException("could not generate X25519 keypair", e);
        }
    }

    /** Generates a 32 byte preshared key. */
    public String generatePresharedKey() {
        byte[] psk = new byte[KEY_BYTES];
        random.nextBytes(psk);
        return b64(psk);
    }

    /** Derives the public key from a base64 encoded private key. */
    public String publicKeyFromPrivate(String privateKeyB64) {
        try {
            byte[] priv = unb64(privateKeyB64);
            if (priv.length != KEY_BYTES) {
                throw new IllegalArgumentException("private key must be 32 bytes");
            }
            KeyFactory factory = KeyFactory.getInstance(X25519);
            PrivateKey privateKey = factory.generatePrivate(
                    new XECPrivateKeySpec(NamedParameterSpec.X25519, priv));

            byte[] base = new byte[KEY_BYTES];
            base[0] = 9;
            PublicKey basePoint = factory.generatePublic(
                    new XECPublicKeySpec(NamedParameterSpec.X25519, new BigInteger(1, reverse(base))));

            KeyAgreement agreement = KeyAgreement.getInstance(X25519);
            agreement.init(privateKey);
            agreement.doPhase(basePoint, true);
            return b64(agreement.generateSecret());
        } catch (IllegalArgumentException e) {
            throw e;
        } catch (Exception e) {
            throw new IllegalStateException("could not derive public key", e);
        }
    }

    /** True when the string is a valid 32 byte base64 WireGuard key. */
    public boolean isValidKey(String key) {
        if (key == null || key.isBlank()) {
            return false;
        }
        try {
            return unb64(key).length == KEY_BYTES;
        } catch (IllegalArgumentException e) {
            return false;
        }
    }

    private static byte[] encodeU(BigInteger u) {
        byte[] bigEndian = u.toByteArray();
        byte[] littleEndian = new byte[KEY_BYTES];
        for (int i = 0; i < KEY_BYTES; i++) {
            int index = bigEndian.length - 1 - i;
            littleEndian[i] = index >= 0 ? bigEndian[index] : 0;
        }
        return littleEndian;
    }

    private static byte[] reverse(byte[] in) {
        byte[] out = new byte[in.length];
        for (int i = 0; i < in.length; i++) {
            out[i] = in[in.length - 1 - i];
        }
        return out;
    }

    private static String b64(byte[] bytes) {
        return Base64.getEncoder().encodeToString(bytes);
    }

    private static byte[] unb64(String value) {
        try {
            return Base64.getDecoder().decode(value.trim());
        } catch (IllegalArgumentException e) {
            throw new IllegalArgumentException("not a valid base64 key", e);
        }
    }
}
