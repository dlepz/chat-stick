# chat-stick

A handheld voice interface for large language models, built on an
[M5StickS3](https://docs.m5stack.com/en/core/M5StickS3) ESP32-S3 device.
Hold the A button, talk, release, and hear the model respond. A Cloudflare
Worker relays device audio to Google's Gemini Live API and routes tool calls
back to the device or to server-side services.

The project is currently configured for `models/gemini-3.1-flash-live-preview`.
Image requests use Imagen, then the server converts the generated image into a
1-bit dithered bitmap that fits the StickS3 display.

## What It Does

- Speech-to-speech chat over WiFi/WebSocket.
- Device controls for brightness, volume, voice, speaker output, sounds,
  melodies, text display, image display, status, and power.
- Countdown timers and alarms that persist across chat sessions and reboots.
- Server-side tools for Google Search, URL fetches, docs search, persistent
  device files, generated image history, and optional email notifications.
- Conversation history and resume support per device.
- Optional OTA firmware delivery from Cloudflare R2.

## Controls

- **Button A**: hold to record, release to send. In menus, press to select.
- **Button B**: hold to open the menu or go back. Click to cycle menu items or
  page through on-screen text/images.
- **A + B hold**: factory reset prompt. This clears saved settings and WiFi
  credentials, then restarts.

## Architecture

```text
M5StickS3 --WebSocket--> Cloudflare Worker / Durable Object --WebSocket--> Gemini Live API
  mic/speaker/display       relay, history, tools, OTA                  speech-to-speech AI
```

**Firmware** (`firmware/`) is a PlatformIO/Arduino project for the M5StickS3.
It captures 16 kHz PCM audio, streams it to the worker, plays 24 kHz PCM audio
responses, manages the screen/menu/buttons, stores local settings in ESP32 NVS,
and executes device-side tool calls.

**Server** (`server/`) is a Cloudflare Worker with a `LiveSession` Durable
Object per device. It bridges the device and Gemini Live WebSockets, persists
conversation/tool/file/image state in D1, uses Workers AI + Vectorize for docs
search, and optionally uses R2 for OTA firmware and image PNG archival.

## Prerequisites

- M5StickS3 and USB-C cable.
- [PlatformIO](https://platformio.org/install) for firmware builds.
- Node.js 18+ and npm for the server.
- [Wrangler CLI](https://developers.cloudflare.com/workers/wrangler/install-and-update/).
- Google AI Studio API key with Gemini Live API access. Image generation also
  requires access to `imagen-4.0-fast-generate-001`.
- Cloudflare account with Workers, Durable Objects, D1, Workers AI, Vectorize,
  and optionally R2 / Email Routing.

## Setup

### Server

```bash
cd server
npm install

cp .dev.vars.example .dev.vars
# Edit .dev.vars:
#   GEMINI_API_KEY=...
#   HISTORY_API_TOKEN=...
#   Optional: ADMIN_API_TOKEN, DEVICE_AUTH_TOKEN, EMAIL_SENDER, EMAIL_RECIPIENT

cp wrangler.toml.example wrangler.toml
# Edit wrangler.toml with your Cloudflare D1 database id and optional bindings.

wrangler d1 create m5-live-conversations
# Paste the returned database_id into wrangler.toml.

wrangler d1 migrations apply DB --local
wrangler vectorize create chat-stick-docs --dimensions=768 --metric=cosine

npm run dev
```

For production, also apply migrations remotely:

```bash
cd server
wrangler d1 migrations apply DB --remote
```

### Firmware

```bash
cd firmware

cp src/credentials.h.example src/credentials.h
# Edit src/credentials.h:
#   DEVELOPMENT_SERVER_ADDRESS: LAN IP of the machine running `wrangler dev`
#   PRODUCTION_SERVER_ADDRESS: deployed worker hostname
#   DEVICE_AUTH_TOKEN: only if the worker has DEVICE_AUTH_TOKEN configured
#   WIFI_NETWORKS: known SSIDs/passwords for first boot

# Update upload_port/monitor_port in platformio.ini for your serial port.

pio run -t upload
pio device monitor
```

The device also has a captive WiFi setup flow. From the menu, use
`Device -> Set up WiFi`, join the `chat-stick-setup` access point, and submit
credentials in the browser form.

### Deploy

```bash
cd server
wrangler secret put GEMINI_API_KEY
wrangler secret put HISTORY_API_TOKEN
wrangler secret put ADMIN_API_TOKEN      # optional
wrangler secret put DEVICE_AUTH_TOKEN    # optional; must match firmware
wrangler d1 migrations apply DB --remote
wrangler deploy
```

After deploying, set `PRODUCTION_SERVER_ADDRESS` in
`firmware/src/credentials.h`, rebuild, and flash the device.

## Optional Cloudflare Bindings

### R2 Storage

R2 is optional for normal chat. It is required for OTA firmware delivery and is
used to archive generated image PNGs when configured. Generated images are still
recorded in D1 for recall even without R2.

```bash
cd server
wrangler r2 bucket create your-bucket-name
```

Then uncomment the `[[r2_buckets]]` block in `wrangler.toml` with binding
`STORAGE`. If you use `publish-ota-release.sh`, make sure its `BUCKET` value
matches the bucket in `wrangler.toml`.

### Email Notifications

The model only receives the `email_me` tool when the Email Routing binding and
email secrets are configured. Cloudflare Email Routing can send only to verified
destination addresses, so this is for self-notifications.

```bash
cd server

# Enable Email Routing for your domain in Cloudflare and verify a destination.
# Uncomment [[send_email]] in wrangler.toml and set destination_address.

wrangler secret put EMAIL_SENDER       # e.g. chat-stick@your-domain.com
wrangler secret put EMAIL_RECIPIENT    # verified destination address
wrangler deploy
```

For local development, add `EMAIL_SENDER` and `EMAIL_RECIPIENT` to
`server/.dev.vars`. Mail is not delivered from `wrangler dev`; deploy to test.

## Server APIs

- `GET /health` - unauthenticated health check.
- `GET /ping` - device connectivity check. Requires `DEVICE_AUTH_TOKEN` when configured.
- `GET /history/:deviceId` - recent conversations for a device.
- `GET /session/:chatId` - last saved assistant message for a chat.
- `GET /firmware/check?version=<n>` - returns OTA availability for a device.
- `GET /firmware/download` - downloads the latest firmware binary from R2.
- `GET /admin/index` - indexes `src/docs-index.json` into Vectorize.
- `GET /admin/search?q=...` - tests Vectorize search.
- `GET /ws?device_id=...&chat_id=...` - device WebSocket endpoint.

History endpoints require `X-History-Token`, `Authorization: Bearer ...`, or
`?token=...`. Admin endpoints use `X-Admin-Token` or fall back to the history
token. Device endpoints use `X-Device-Token` or `?device_token=...` when
`DEVICE_AUTH_TOKEN` is configured; if no device token is configured, device
endpoints are open.

## Knowledge Base

The server can search a docs index through keyword search and Cloudflare
Vectorize. Source content is expected to be `.mdx` files grouped by directory,
where each directory becomes a section.

```text
my-docs/
  getting-started/
    installation.mdx
  guides/
    configuration.mdx
```

Each `.mdx` file should include YAML frontmatter:

```yaml
---
title: Installation
keywords:
  - setup
  - install
status: published
---
```

Build the committed JSON index:

```bash
cd server
DOCS_PATH=/path/to/my-docs npx tsx scripts/build-docs-index.ts
```

Index into Vectorize using the running worker:

```bash
curl -H "X-Admin-Token: $ADMIN_API_TOKEN" http://localhost:8787/admin/index
curl -H "X-Admin-Token: $ADMIN_API_TOKEN" "http://localhost:8787/admin/search?q=setup"
```

Or use the standalone indexing worker:

```bash
cd server
wrangler dev scripts/index-docs-worker.ts --port 8799
curl http://localhost:8799/index
curl "http://localhost:8799/search?q=setup"
```

Local keyword search can be checked with:

```bash
cd server
npx tsx scripts/test-search.ts
```

## Device Tools

Gemini can call tools that the worker handles directly or forwards to the
device.

Server-side tools include `search_docs`, `web_fetch`, `list_files`,
`read_file`, `write_file`, `append_to_file`, `search_files`,
`new_conversation`, image history tools, and optional `email_me`.

Device-side tools include `set_brightness`, `set_volume`, `set_speaker`,
`set_external_speaker_gain`, `set_voice`, `show_text`, `show_image`,
`set_timer`, `list_timers`, `cancel_timer`, `extend_timer`, `play_sound`,
`play_melody`, `power_off`, and `get_device_status`.

## Test Data

`test-data/` contains sample `.m4a` audio files for development and testing.

## Releases and OTA

This repository is source-only. Do not publish pre-built `.bin` firmware files
publicly. `credentials.h` is compiled into the firmware, so local binaries
contain WiFi SSIDs, passwords, device tokens, and server URLs in plaintext.

OTA distribution is per deployment. Each user should serve their own firmware
binary from their own R2 bucket through their own worker.

Convenience scripts:

| Script | What it does |
| --- | --- |
| `./flash.sh [--monitor]` | Build firmware and upload over USB. |
| `./deploy.sh` | Deploy the Cloudflare Worker. |
| `./publish-ota-release.sh` | Build firmware and upload `firmware-v<N>.bin` to R2. |
| `./publish.sh` | Publish the OTA binary, then deploy the worker. |

To cut a firmware release:

1. Bump `FIRMWARE_VERSION` in `firmware/src/Config.h`.
2. Confirm `BUCKET` in `publish-ota-release.sh` matches your R2 bucket.
3. Run `./publish-ota-release.sh`.
4. Devices on older versions install the update on next boot, or from
   `Device -> Check for updates`.

The worker finds the latest available firmware by listing
`chat-stick/firmware/firmware-v<N>.bin` in R2 and choosing the highest version.

## Credentials

All deployment-specific files are gitignored:

| File | Purpose | Template |
| --- | --- | --- |
| `server/.dev.vars` | Local secrets for Wrangler dev. | `server/.dev.vars.example` |
| `server/wrangler.toml` | Cloudflare bindings and account-specific ids. | `server/wrangler.toml.example` |
| `firmware/src/credentials.h` | Server endpoints, device token, WiFi networks. | `firmware/src/credentials.h.example` |

Never commit credentials or firmware binaries built with credentials embedded.

## Hardware

- **Device**: M5StickS3 (ESP32-S3, 135x240 LCD, MEMS mic, 1W speaker)
- **Buttons**: A = GPIO 11, B = GPIO 12
- **Audio**: 16-bit PCM, 16 kHz input, 24 kHz output
- **Optional speaker**: M5Stack HAT SPK2 external speaker

## License

MIT
