package com.mraf.server.webrtc;

import jakarta.annotation.PostConstruct;
import jakarta.annotation.PreDestroy;
import org.slf4j.Logger;
import org.slf4j.LoggerFactory;
import org.springframework.stereotype.Component;

import java.net.DatagramPacket;
import java.net.DatagramSocket;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.nio.ByteBuffer;
import java.nio.ByteOrder;
import java.util.concurrent.atomic.AtomicLong;
import java.util.zip.CRC32;

/**
 * Minimal RFC 5389 STUN server.
 *
 * <p>WebRTC peers (the browser and the controller) send Binding Requests here
 * while gathering ICE candidates. We answer with the address we observed the
 * packet arrive from ({@code XOR-MAPPED-ADDRESS}), which is how a peer behind
 * NAT learns its server-reflexive address. Only Binding Requests are handled;
 * TURN allocations and other methods are ignored.
 */
@Component
public class StunServer {
    private static final Logger log = LoggerFactory.getLogger(StunServer.class);

    private static final int MAGIC_COOKIE = 0x2112A442;
    private static final int BINDING_REQUEST = 0x0001;
    private static final int BINDING_SUCCESS = 0x0101;
    private static final int BINDING_ERROR = 0x0111;

    private static final int ATTR_MAPPED_ADDRESS = 0x0001;
    private static final int ATTR_XOR_MAPPED_ADDRESS = 0x0020;
    private static final int ATTR_SOFTWARE = 0x8022;
    private static final int ATTR_FINGERPRINT = 0x8028;
    private static final int FINGERPRINT_XOR = 0x5354554E;

    private static final String SOFTWARE = "mraf-stun 1.0";

    private final WebRtcProperties props;
    private final AtomicLong requests = new AtomicLong();
    private final AtomicLong responses = new AtomicLong();
    private final AtomicLong errors = new AtomicLong();

    private volatile DatagramSocket socket;
    private volatile Thread thread;
    private volatile boolean running;

    public StunServer(WebRtcProperties props) {
        this.props = props;
    }

    @PostConstruct
    public void start() {
        if (!props.isEnabled() || !props.getStun().isEnabled()) {
            log.info("STUN server disabled");
            return;
        }
        try {
            socket = new DatagramSocket(props.getStun().getPort());
            socket.setReuseAddress(true);
        } catch (Exception e) {
            log.warn("STUN server could not bind UDP {}: {}", props.getStun().getPort(), e.getMessage());
            socket = null;
            return;
        }
        running = true;
        thread = new Thread(this::loop, "mraf-stun");
        thread.setDaemon(true);
        thread.start();
        log.info("STUN server listening on UDP {}", props.getStun().getPort());
    }

    @PreDestroy
    public void stop() {
        running = false;
        DatagramSocket s = socket;
        if (s != null) {
            s.close();
        }
    }

    private void loop() {
        byte[] buf = new byte[1024];
        DatagramSocket s = socket;
        while (running && s != null && !s.isClosed()) {
            try {
                DatagramPacket packet = new DatagramPacket(buf, buf.length);
                s.receive(packet);
                handle(s, packet);
            } catch (Exception e) {
                if (running) {
                    log.debug("STUN receive failed: {}", e.getMessage());
                }
            }
        }
    }

    private void handle(DatagramSocket s, DatagramPacket packet) {
        byte[] data = packet.getData();
        int len = packet.getLength();
        if (len < 20) {
            return;
        }
        ByteBuffer in = ByteBuffer.wrap(data, 0, len).order(ByteOrder.BIG_ENDIAN);
        int type = in.getShort() & 0xFFFF;
        int msgLen = in.getShort() & 0xFFFF;
        int cookie = in.getInt();
        byte[] txId = new byte[12];
        in.get(txId);
        if (cookie != MAGIC_COOKIE || type != BINDING_REQUEST || msgLen > len - 20) {
            return;
        }
        requests.incrementAndGet();

        InetSocketAddress from = (InetSocketAddress) packet.getSocketAddress();
        InetAddress addr = from.getAddress();
        int port = from.getPort();

        boolean ipv6 = addr.getAddress().length == 16;
        int family = ipv6 ? 0x02 : 0x01;
        byte[] ip = addr.getAddress();

        ByteBuffer out = ByteBuffer.allocate(128).order(ByteOrder.BIG_ENDIAN);
        out.putShort((short) BINDING_SUCCESS);
        out.putShort((short) 0); // length patched later
        out.putInt(MAGIC_COOKIE);
        out.put(txId);

        putAddress(out, ATTR_MAPPED_ADDRESS, family, port, ip, null);
        putAddress(out, ATTR_XOR_MAPPED_ADDRESS, family, port, ip, txId);
        putString(out, ATTR_SOFTWARE, SOFTWARE);

        // FINGERPRINT must be the last attribute: CRC32 over everything so far.
        int fpStart = out.position();
        out.putShort((short) ATTR_FINGERPRINT);
        out.putShort((short) 4);
        CRC32 crc = new CRC32();
        crc.update(out.array(), 0, fpStart);
        out.putInt((int) (crc.getValue() ^ FINGERPRINT_XOR));

        int total = out.position();
        out.putShort(2, (short) (total - 20));

        try {
            s.send(new DatagramPacket(out.array(), total, from));
            responses.incrementAndGet();
        } catch (Exception e) {
            errors.incrementAndGet();
        }
    }

    private static void putAddress(ByteBuffer out, int attrType, int family, int port,
                                   byte[] ip, byte[] txId) {
        out.putShort((short) attrType);
        out.putShort((short) (4 + ip.length));
        out.put((byte) 0);
        out.put((byte) family);
        if (txId == null) {
            out.putShort((short) port);
            out.put(ip);
        } else {
            out.putShort((short) (port ^ (MAGIC_COOKIE >> 16)));
            byte[] mask = new byte[ip.length];
            mask[0] = (byte) (MAGIC_COOKIE >> 24);
            mask[1] = (byte) (MAGIC_COOKIE >> 16);
            mask[2] = (byte) (MAGIC_COOKIE >> 8);
            mask[3] = (byte) MAGIC_COOKIE;
            if (ip.length == 16) {
                System.arraycopy(txId, 0, mask, 4, 12);
            }
            for (int i = 0; i < ip.length; i++) {
                out.put((byte) (ip[i] ^ mask[i]));
            }
        }
    }

    private static void putString(ByteBuffer out, int attrType, String value) {
        byte[] bytes = value.getBytes(java.nio.charset.StandardCharsets.UTF_8);
        out.putShort((short) attrType);
        out.putShort((short) bytes.length);
        out.put(bytes);
        while ((out.position() & 3) != 0) {
            out.put((byte) 0);
        }
    }

    public boolean isRunning() {
        return running && socket != null && !socket.isClosed();
    }

    public int getPort() {
        return props.getStun().getPort();
    }

    /** The port actually bound (differs from the configured port when 0 = ephemeral). */
    public int getBoundPort() {
        DatagramSocket s = socket;
        return s != null ? s.getLocalPort() : props.getStun().getPort();
    }

    public long getRequests() {
        return requests.get();
    }

    public long getResponses() {
        return responses.get();
    }

    public long getErrors() {
        return errors.get();
    }
}
