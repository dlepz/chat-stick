---
tags:
  - reachy_mini_python_app
---

# Reachy Chat Stick

A Reachy Mini Python app that connects the robot to the existing chat-stick voice server.

Reachy provides the microphones and speaker. The app speaks the chat-stick wire protocol:

- Send `{"type":"start"}` when voice activity starts.
- Stream 16 kHz signed PCM as binary WebSocket frames.
- Send `{"type":"stop"}` after silence.
- Play assistant 24 kHz PCM binary frames through Reachy's speaker.
- Answer device-side tool calls for status, sounds, melodies, and timers.

## Configuration

Set these in the environment used by the Reachy daemon or before running directly:

```bash
export CHAT_STICK_SERVER_URL="http://localhost:8787"
export CHAT_STICK_DEVICE_ID="reachy-mini"
export REACHY_CONNECTION_MODE="localhost_only"
export REACHY_HOST="reachy-mini.local"
export REACHY_MEDIA_BACKEND="webrtc"
# Optional, only if DEVICE_AUTH_TOKEN is configured on the worker:
export CHAT_STICK_DEVICE_TOKEN="..."
export CHAT_STICK_VOICE="Aoede"
```

For a Wireless robot, `CHAT_STICK_SERVER_URL` usually needs to point to your deployed worker or to a LAN-reachable dev machine, not `localhost`.

Useful tuning knobs:

```bash
export CHAT_STICK_VAD_START_THRESHOLD="0.020"
export CHAT_STICK_VAD_SILENCE_THRESHOLD="0.010"
export CHAT_STICK_VAD_START_S="0.25"
export CHAT_STICK_VAD_END_SILENCE_S="0.70"
export CHAT_STICK_USE_DOA="false"
export CHAT_STICK_LISTEN_WHILE_SPEAKING="false"
export CHAT_STICK_REACHY_MOTION="false"
```

## Run Directly

From this app directory:

```bash
./run.sh
```

The launcher creates/uses `.venv`, installs dependencies when needed, checks that the chat-stick server is reachable, and then starts the Reachy app.

By default the launcher connects to the local Reachy daemon but forces WebRTC media:

```bash
REACHY_CONNECTION_MODE=localhost_only
REACHY_MEDIA_BACKEND=webrtc
```

That was the combination that worked here for robot microphone/speaker audio. If the SDK reports `Using LOCAL backend`, it is using computer audio instead.

Before running it, start the chat-stick server in another terminal:

```bash
cd server
npx wrangler dev --local --port 8787
```

For a deployed worker:

```bash
CHAT_STICK_SERVER_URL="https://your-worker.example.workers.dev" ./run.sh
```

The Reachy daemon must already be running. On Lite, that is normally on your laptop. On Wireless, run this in the robot's app environment or install it through the daemon.

Robot motion is off by default. To enable a small fixed listening pose only while the app is recording:

```bash
CHAT_STICK_REACHY_MOTION=true ./run.sh
```

## Install Manually on Wireless

```bash
scp -r apps/reachy-chat-stick pollen@reachy-mini.local:/tmp/reachy-chat-stick
ssh pollen@reachy-mini.local "/venvs/apps_venv/bin/pip install /tmp/reachy-chat-stick"
curl -X POST http://reachy-mini.local:8000/api/apps/start-app/reachy_chat_stick
```

Replace `reachy-mini.local` with the robot IP if mDNS is unavailable.

## Notes

- This app does not change the chat-stick server. It connects as another device with `device_id=reachy-mini`.
- Image and text display tool calls are acknowledged but only logged because Reachy Mini does not have the chat-stick screen.
- Timers are persisted to `~/.local/state/reachy-chat-stick/timers.json` by default.
