# MRAF Matter bridge

Exposes the relays, pump and float sensors managed by the MRAF Spring Boot
server as Matter devices. The ESP32 firmware and CoAP protocol are unchanged;
the bridge is a small Node.js process that talks to the server over REST.

Full documentation, endpoint mapping and troubleshooting:
[docs/12-matter-bridge.md](../docs/12-matter-bridge.md).

## Requirements

- Node.js 20.19 or newer
- A running MRAF server (`http://127.0.0.1:8080` by default)
- Server admin credentials (default `admin` / `mraf-demo`)
- IPv6 and mDNS on the bridge host for commissioning

## Run

```bash
cd matter
npm install
MRAF_SERVER_URL=http://127.0.0.1:8080 \
MRAF_SERVER_USER=admin \
MRAF_SERVER_PASSWORD=mraf-demo \
npm start
```

The process prints a Matter QR code and manual pairing code. Add it from Apple
Home, Google Home, Amazon Alexa or Home Assistant as a Matter bridge. Each
confirmed MRAF device appears as an aggregator with relay, pump and sensor
endpoints.

## Configuration

| Variable | Default | Purpose |
|---|---|---|
| `MRAF_SERVER_URL` | `http://127.0.0.1:8080` | Spring Boot server base URL |
| `MRAF_SERVER_USER` | `admin` | Server login |
| `MRAF_SERVER_PASSWORD` | `mraf-demo` | Server password |
| `MRAF_MATTER_DEVICE` | all confirmed | Expose one MRAF device ID only |
| `MRAF_MATTER_PORT` | `5540` | Matter UDP port |
| `MRAF_MATTER_STORAGE` | `data/matter` | Matter storage directory |
| `MRAF_MATTER_POLL_MS` | `5000` | Server state poll interval |
| `MRAF_MATTER_CONFIG_MS` | `60000` | Device config refresh interval |
| `MRAF_MATTER_COMMAND_GRACE_MS` | `8000` | Ignore stale state after a command |
| `MRAF_MATTER_PASSCODE` | `20202021` | Commissioning passcode |
| `MRAF_MATTER_DISCRIMINATOR` | `3840` | Commissioning discriminator |
| `MRAF_MATTER_NODE_ID` | `mraf-bridge` | Matter node ID |
| `MRAF_MATTER_LOG_LEVEL` | Matter.js default | `debug`, `info`, `warn` |

## Tests

```bash
npm test
npm run check
```
