# chat-stick

A handheld chat interface for large language models, built on an [M5StickS3](https://docs.m5stack.com/en/core/M5StickS3) (ESP32-S3). Hold a button, talk, release to hear the AI respond. Powered by [Google's Gemini 3.1 Live API](https://ai.google.dev/gemini-api/docs/live) via a Cloudflare Worker relay.

## Introduction

This application uses a M5StickS3 as a user interface to chat with a large language model. The project is designed for Google's Gemini 3.1 Live API, which allows for low-latency conversational experience. The user communicates with the system by holding down the device's A button to record their voice; the recordings are sent via WiFi to a CloudFlare worker, which holds the conversation history for the device, passes the audio to the Gemini Live API, and then sends back the responses.

In addition to being able to respond in natural language, the model is also given various tool calls for accessing information or performing actions. It can access the internet via web fetch and web search, from a vector database of known information, as well as accessing information about the device, such as its settings and battery level. The model can perform actions on the device such as adjusting brightness, volume, or power, or displaying text and images, or playing sounds.

## Architecture

```
M5StickS3 ──WebSocket──▶ Cloudflare Worker (Durable Object) ──WebSocket──▶ Gemini Live API
  mic/speaker               relay + tool handling                           speech-to-speech AI
```

**Firmware** (`firmware/`) — PlatformIO/Arduino project for the M5StickS3. Captures audio via push-to-talk (A button), streams it over WiFi/WebSocket to the server, and plays back AI audio responses through the speaker.

**Server** (`server/`) — Cloudflare Worker with a Durable Object (`LiveSession`) that bridges the device to Gemini's Live API. Holds conversation history in D1, routes tool calls (web fetch, web search, vector database lookups, device info and control), and manages session lifecycle.

The device persists brightness, volume, the current `chat_id`, and recently successful WiFi credentials in ESP32 NVS so those settings survive reboots. On boot, it reuses the saved chat session, prefers saved WiFi networks, and fetches the last assistant reply from the server before reconnecting.

## Prerequisites

- [PlatformIO](https://platformio.org/install) (firmware builds)
- [Node.js](https://nodejs.org/) 18+ (server)
- [Wrangler CLI](https://developers.cloudflare.com/workers/wrangler/install-and-update/) (`npm i -g wrangler`)
- A [Google AI Studio](https://aistudio.google.com/) API key with Gemini Live API access
- A Cloudflare account (for Workers, D1, Vectorize)

## Setup

### Server

```bash
cd server
npm install

# Copy the example env file and add your API keys
cp .dev.vars.example .dev.vars
# Edit .dev.vars with your GEMINI_API_KEY

# Copy the wrangler config template (wrangler.toml is gitignored —
# it holds your Cloudflare account-specific bindings)
cp wrangler.toml.example wrangler.toml

# Create the D1 database
wrangler d1 create m5-live-conversations
# Paste the returned database_id into wrangler.toml

# Apply migrations
wrangler d1 migrations apply --local

# Create the Vectorize index (768 dims for @cf/baai/bge-base-en-v1.5)
wrangler vectorize create chat-stick-docs --dimensions=768 --metric=cosine

# Run locally
npm run dev
```

### Firmware

```bash
cd firmware

# Copy the credentials template and fill in your values
cp src/credentials.h.example src/credentials.h
# Edit src/credentials.h:
#   - DEVELOPMENT_SERVER_ADDRESS: LAN IP of your `wrangler dev` machine
#   - PRODUCTION_SERVER_ADDRESS:  your deployed worker, e.g. m5-live.<you>.workers.dev
#   - WIFI_NETWORKS: your WiFi SSIDs and passwords

# Update upload_port/monitor_port in platformio.ini to your device's serial port

# Build and upload
pio run -t upload

# Monitor serial output
pio device monitor
# or: python monitor.py
```

### Deploy

```bash
# Deploy the worker
cd server
wrangler secret put GEMINI_API_KEY
wrangler secret put HISTORY_API_TOKEN
wrangler deploy

# Update firmware to point at the deployed endpoint
# Set PRODUCTION_SERVER_ADDRESS in firmware/src/credentials.h to your worker's hostname
```

## Optional: Email Notifications

The model can be given an `email_me` tool that sends a short plain-text email through Cloudflare Email Routing. The tool is hidden from the model entirely unless you configure it, so this section is fully optional.

Requires a domain on Cloudflare with [Email Routing](https://developers.cloudflare.com/email-routing/) enabled. Cloudflare only allows sending **to verified destination addresses**, so this is suited to self-notifications, not emailing arbitrary users.

```bash
cd server

# 1. In the Cloudflare dashboard, enable Email Routing on your domain and verify
#    a destination address (e.g. you@your-domain.com).

# 2. Uncomment the [[send_email]] block in wrangler.toml and set
#    destination_address to your verified address.

# 3. Set the sender + recipient secrets. Sender must be on your Cloudflare
#    domain; recipient must equal destination_address from wrangler.toml.
wrangler secret put EMAIL_SENDER       # e.g. chat-stick@your-domain.com
wrangler secret put EMAIL_RECIPIENT    # the verified destination address

wrangler deploy
```

For local `wrangler dev`, also add `EMAIL_SENDER` and `EMAIL_RECIPIENT` to `server/.dev.vars`. Outbound email is not delivered in `wrangler dev` (the binding is a no-op locally), so test by deploying.

## History and Session APIs

- `GET /history/:deviceId` — recent conversations for a device. Requires `X-History-Token` or `?token=...`.
- `GET /session/:chatId?device_id=...` — last saved assistant message for a chat. The firmware uses this during boot restore.

## Credentials

All secrets are gitignored. You need to create these files locally:

| File                         | Purpose                                    | Template                             |
| ---------------------------- | ------------------------------------------ | ------------------------------------ |
| `server/.dev.vars`           | Gemini API key, history token              | `server/.dev.vars.example`           |
| `server/wrangler.toml`       | Cloudflare bindings (your D1 ID, optional email) | `server/wrangler.toml.example`       |
| `firmware/src/credentials.h` | Server addresses + WiFi SSIDs/passwords    | `firmware/src/credentials.h.example` |

Never commit credentials. The `.gitignore` is configured to exclude these files.

## Hardware

- **Device**: M5StickS3 (ESP32-S3, 135x240 LCD, MEMS mic, 1W speaker)
- **Buttons**: A (GPIO 11) = push-to-talk, B (GPIO 12) = menu/control
- **Audio**: 16kHz input / 24kHz output PCM

## Adding Docs to the Knowledge Base

The server includes a vector search knowledge base powered by Cloudflare Vectorize. You can populate it with `.mdx` content files so the model can answer questions about your docs.

### 1. Organize your content

Point `DOCS_PATH` at a directory of `.mdx` files organized into subdirectories. Each subdirectory becomes a **section** in the index.

```
my-docs/
  getting-started/
    installation.mdx
    quick-start.mdx
  guides/
    configuration.mdx
    advanced-usage.mdx
```

Each `.mdx` file should have YAML frontmatter with at least a `title`. Optional fields: `keywords` (list) and `status` (only `published` entries are indexed).

```yaml
---
title: Installation
keywords:
  - setup
  - install
  - getting started
---
Your content here...
```

### 2. Build the docs index

This parses all `.mdx` files and writes a JSON index to `server/src/docs-index.json`.

```bash
cd server
DOCS_PATH=/path/to/my-docs npx tsx scripts/build-docs-index.ts
```

### 3. Upload to Vectorize

You can index the docs into Cloudflare Vectorize using the worker-based approach (recommended) or the standalone script.

**Worker approach** (uses Workers AI and Vectorize bindings directly):

```bash
cd server
wrangler dev scripts/index-docs-worker.ts --port 8799
curl http://localhost:8799/index
```

**Standalone script** (uses Cloudflare REST API):

```bash
cd server
CLOUDFLARE_ACCOUNT_ID=xxx CLOUDFLARE_API_TOKEN=xxx npx tsx scripts/index-docs.ts
```

### 4. Test search

Verify your docs are searchable with the keyword search test script:

```bash
cd server
npx tsx scripts/test-search.ts
```

Or test vector search through the indexing worker:

```bash
curl "http://localhost:8799/search?q=your+query"
```

## Test Data

`test-data/` contains sample `.m4a` audio files for development and testing.

## Releases and OTA

The repository is public and source-only. **Do not publish pre-built `.bin` files anywhere public.** `credentials.h` is compiled into the firmware, so any binary built locally embeds your WiFi SSIDs, passwords, and worker URL as plaintext — `strings firmware.bin` will surface them.

OTA distribution is per-deployment: each user's binary lives in their own R2 bucket and is served by their own worker.

### Convenience scripts (repo root)

| Script                       | What it does                                                |
| ---------------------------- | ----------------------------------------------------------- |
| `./flash.sh [--monitor]`     | Build firmware and upload over USB to a connected device.   |
| `./deploy.sh`                | Deploy the Cloudflare Worker.                               |
| `./publish-ota-release.sh`   | Build firmware and upload it to R2 for OTA pickup.          |
| `./publish.sh`               | Run `publish-ota-release.sh` then `deploy.sh`.              |

### Cutting a new firmware release

1. Bump `FIRMWARE_VERSION` in `firmware/src/Config.h`.
2. `./publish-ota-release.sh` — builds and uploads `firmware-v<N>.bin` to your R2 bucket under `chat-stick/firmware/`.
3. Devices on older versions will install it on next boot.

The worker auto-detects the latest available version by listing the R2 prefix and picking the highest `firmware-v<N>.bin` — no version tracking secrets required.

If you want a public source tag for changelog purposes, `gh release create v<N> --title "Firmware v<N>" --notes "..."` with **no binary asset**. This is purely informational; the OTA path doesn't read GitHub.

## License

MIT
