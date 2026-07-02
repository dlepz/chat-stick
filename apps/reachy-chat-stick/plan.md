# Reachy Chat Stick Plan

## Goal

Build a Reachy Mini application that turns the robot into a chat-stick client: listen through Reachy's microphones, send speech to the existing chat-stick WebSocket server, and play the assistant's speech back through Reachy's speaker.

## Approach

- Reuse the existing `GET /ws` device protocol rather than changing the Cloudflare Worker.
- Convert Reachy SDK audio input (`float32`, normally 16 kHz stereo) into chat-stick input (`int16` PCM, 16 kHz mono).
- Convert chat-stick assistant output (`int16` PCM, 24 kHz mono) into Reachy speaker samples (`float32`, SDK output sample rate/channels).
- Use voice activity detection to emulate push-to-talk: send `start` when speech begins, stream PCM frames, send `stop` after silence.
- Handle device-side tool calls with Reachy-appropriate responses:
  - Supported locally: status, sounds, melodies, in-app timers, stop app.
  - Logged or unavailable: screen brightness, text/image display, speaker route, external gain.
- Add subtle optional Reachy head/antenna motion while listening and speaking.

## Assumptions

- The chat-stick server is already running locally or deployed.
- Reachy Mini's daemon/app environment has the official `reachy-mini` Python SDK.
- The robot app can receive configuration through environment variables inherited from the daemon.
- On-device speech means using Reachy's mic and speaker; transcription/TTS remain handled by the existing Gemini Live bridge.
