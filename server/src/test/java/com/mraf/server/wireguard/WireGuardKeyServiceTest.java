package com.mraf.server.wireguard;

import org.junit.jupiter.api.Test;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertNotEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class WireGuardKeyServiceTest {
    private final WireGuardKeyService keys = new WireGuardKeyService();

    @Test
    void generates32ByteBase64Keys() {
        WireGuardKeyService.KeyPairStrings kp = keys.generate();
        assertTrue(keys.isValidKey(kp.privateKey()));
        assertTrue(keys.isValidKey(kp.publicKey()));
        assertEquals(44, kp.privateKey().length());
        assertNotEquals(kp.privateKey(), kp.publicKey());
    }

    @Test
    void derivesPublicKeyFromPrivate() {
        WireGuardKeyService.KeyPairStrings kp = keys.generate();
        assertEquals(kp.publicKey(), keys.publicKeyFromPrivate(kp.privateKey()));
    }

    @Test
    void derivesKnownPublicKey() {
        // Cross-checked against an independent X25519 implementation.
        String priv = "yAnz5TF+lXXJte14tji3zlMNq+hd2rYUIgJBgB3fBmk=";
        String pub = "HIgo9xNzJMWLKASShiTqIybxZ0U3wGLiUeJ1PKf8ykw=";
        assertEquals(pub, keys.publicKeyFromPrivate(priv));
    }

    @Test
    void generatesPresharedKeys() {
        String psk = keys.generatePresharedKey();
        assertTrue(keys.isValidKey(psk));
        assertNotEquals(psk, keys.generatePresharedKey());
    }

    @Test
    void rejectsInvalidKeys() {
        assertFalse(keys.isValidKey("not-base64!!"));
        assertFalse(keys.isValidKey(""));
        assertFalse(keys.isValidKey(null));
    }
}
