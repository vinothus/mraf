package com.mraf.server.webrtc;

import org.junit.jupiter.api.Test;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertTrue;

class StunServerTest {
    private static final int MAGIC_COOKIE = 0x2112A442;

    @Test
    void answersBindingRequestWithXorMappedAddress() throws Exception {
        WebRtcProperties props = new WebRtcProperties();
        props.getStun().setPort(0); // ephemeral
        StunServer server = new StunServer(props);
        server.start();
        try {
            assertTrue(server.isRunning());
            int port = server.getBoundPort();
            assertTrue(port > 0);

            byte[] txId = new byte[12];
            for (int i = 0; i < txId.length; i++) {
                txId[i] = (byte) (i + 1);
            }
            ByteBuffer request = ByteBuffer.allocate(20).order(ByteOrder.BIG_ENDIAN);
            request.putShort((short) 0x0001); // Binding Request
            request.putShort((short) 0);
            request.putInt(MAGIC_COOKIE);
            request.put(txId);

            try (DatagramSocket client = new DatagramSocket()) {
                client.setSoTimeout(3000);
                client.send(new DatagramPacket(request.array(), request.array().length,
                        new InetSocketAddress(InetAddress.getLoopbackAddress(), port)));
                byte[] buf = new byte[256];
                DatagramPacket response = new DatagramPacket(buf, buf.length);
                client.receive(response);

                ByteBuffer out = ByteBuffer.wrap(response.getData(), 0, response.getLength())
                        .order(ByteOrder.BIG_ENDIAN);
                assertEquals(0x0101, out.getShort() & 0xFFFF, "Binding Success");
                int msgLen = out.getShort() & 0xFFFF;
                assertEquals(MAGIC_COOKIE, out.getInt());
                byte[] echoed = new byte[12];
                out.get(echoed);
                assertEquals(ByteBuffer.wrap(txId), ByteBuffer.wrap(echoed), "transaction id echoed");

                Integer xorPort = null;
                boolean sawFingerprint = false;
                int end = 20 + msgLen;
                while (out.position() + 4 <= end) {
                    int type = out.getShort() & 0xFFFF;
                    int len = out.getShort() & 0xFFFF;
                    int valueStart = out.position();
                    if (type == 0x0020) { // XOR-MAPPED-ADDRESS
                        out.get();
                        out.get();
                        xorPort = (out.getShort() & 0xFFFF) ^ (MAGIC_COOKIE >> 16);
                    } else if (type == 0x8028) { // FINGERPRINT
                        sawFingerprint = true;
                    }
                    out.position(valueStart + len + ((4 - len % 4) % 4));
                }
                assertEquals(client.getLocalPort(), xorPort, "server-reflexive port reported");
                assertTrue(sawFingerprint, "FINGERPRINT present");
                assertEquals(1, server.getRequests());
                assertEquals(1, server.getResponses());
            }
        } finally {
            server.stop();
        }
    }

    @Test
    void ignoresNonBindingPackets() throws Exception {
        WebRtcProperties props = new WebRtcProperties();
        props.getStun().setPort(0);
        StunServer server = new StunServer(props);
        server.start();
        try {
            try (DatagramSocket client = new DatagramSocket()) {
                client.setSoTimeout(600);
                byte[] junk = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
                client.send(new DatagramPacket(junk, junk.length,
                        new InetSocketAddress(InetAddress.getLoopbackAddress(), server.getBoundPort())));
                DatagramPacket response = new DatagramPacket(new byte[64], 64);
                boolean got = true;
                try {
                    client.receive(response);
                } catch (java.net.SocketTimeoutException e) {
                    got = false;
                }
                assertTrue(!got, "no reply for non-binding packet");
            }
            assertEquals(0, server.getRequests());
        } finally {
            server.stop();
        }
    }
}
