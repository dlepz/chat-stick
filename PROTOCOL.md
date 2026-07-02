# Device/Server Protocol

This document describes the current protocol between chat-stick firmware and the
Cloudflare Worker. The implementation remains the source of truth:

- `server/src/live-session.ts`
- `server/src/index.ts`
- `devices/firmware/common/src/services/LiveSessionService.cpp`
- `devices/firmware/m5-stick/src/`
- `devices/firmware/waveshare/src/`
- `firmware/src/`

Update this document when adding, renaming, or changing protocol messages.

## WebSocket Session

The device opens a websocket to:

```text
/ws?device_id=<device-id>[&chat_id=<chat-id>][&voice=<voice-name>][&image_w=<px>&image_h=<px>]
```

`device_id` selects the Durable Object instance. `chat_id` restores a previous
conversation when present. `voice` selects a Gemini voice when supported.
`image_w` and `image_h` let a device declare the bitmap size it wants for
generated images.

Text frames are UTF-8 JSON. Binary frames are raw PCM audio.

## Binary Audio

Device-to-server binary frames are microphone audio:

- signed 16-bit PCM
- mono
- 16 kHz
- little-endian

Server-to-device binary frames are model playback audio:

- signed 16-bit PCM
- mono
- 24 kHz
- little-endian

## Device To Server JSON

### `start`

```json
{ "type": "start" }
```

Starts a push-to-talk turn. The server resets turn metrics, clears pending
audio, and reconnects Gemini if needed.

### `stop`

```json
{ "type": "stop" }
```

Ends the push-to-talk turn. The server sends `audioStreamEnd` when Gemini is
ready or may respond with `ignore_audio` for accidental short turns.

### `text`

```json
{ "type": "text", "content": "hello" }
```

Optional text input path accepted by the server.

### `tool_response`

```json
{
  "type": "tool_response",
  "name": "set_volume",
  "id": "function-call-id",
  "result": "Volume set to 180"
}
```

Returned by firmware after handling a device-side tool call.

## Server To Device JSON

### `session`

```json
{ "type": "session", "chatId": "uuid", "mode": "voice" }
```

Sent when a device connects. When `reset` is true, firmware clears current
conversation UI and stores the new chat id.

### `server_ready`

```json
{ "type": "server_ready" }
```

Sent after the Durable Object accepts the device websocket.

### `ready`

```json
{ "type": "ready" }
```

Sent after Gemini setup completes.

### `transcript`

```json
{ "type": "transcript", "source": "user", "text": "hello" }
```

```json
{ "type": "transcript", "source": "model", "text": "Hallo." }
```

`text` is an append-only delta. The server strips repeated or overlapping
transcription hypotheses before sending.

### `turn_complete`

```json
{ "type": "turn_complete" }
```

Sent when Gemini reports turn completion. The server also persists the exchange
and may trigger language-learning feedback.

### `drop_audio`

```json
{ "type": "drop_audio" }
```

Tells firmware to discard queued model playback after interruption.

### `ignore_audio`

```json
{ "type": "ignore_audio", "reason": "too_short", "bytes": 3200, "avg_abs": 17, "chunks": 1 }
```

Sent when the server intentionally ignores a captured turn.

### `tool_call`

```json
{
  "type": "tool_call",
  "name": "set_brightness",
  "id": "function-call-id",
  "args": { "level": 120 }
}
```

Forwarded when Gemini calls a device-side tool.

### `voice_changed`

```json
{ "type": "voice_changed", "voice": "Kore" }
```

Sent after the server accepts `set_voice`.

### Image Messages

```json
{ "type": "show_image_pending" }
```

```json
{ "type": "show_image_failed" }
```

```json
{
  "type": "show_image",
  "data": "<base64 packed 1-bit bitmap>",
  "width": 232,
  "height": 112,
  "image_id": 123
}
```

`data` is a base64-encoded 1-bit bitmap packed MSB first. `key` may also be
present when the dithered PNG was stored in R2.

### Learning Feedback Messages

```json
{ "type": "turn_feedback", "color": "green", "reason": "..." }
```

```json
{ "type": "face_emotion", "emotion": "thinking" }
```

```json
{
  "type": "face_control",
  "emotion": "focused",
  "look_x": 0,
  "look_y": 0,
  "eye_spacing": 52,
  "anim_speed": 1
}
```

These are local learning/display additions and should be preserved during
upstream merges.

### `error`

```json
{
  "type": "error",
  "category": "gemini_unavailable",
  "message": "Failed to connect to AI"
}
```

## Tool Routing

The server handles these tools without forwarding them to firmware:

- `search_docs`
- `web_fetch`
- `fetch_url`
- `list_files`
- `read_file`
- `write_file`
- `append_to_file`
- `search_files`
- `set_voice`
- `set_thinking_level`
- `email_me`
- `show_image`
- `list_recent_images`
- `search_images`
- `show_saved_image`
- `set_timer`
- `list_timers`
- `cancel_timer`
- `extend_timer`
- `new_conversation`
- `new_chat`
- `search_learning_resources`
- `load_learning_resource`
- `search_reader_passages`
- `load_reader_passage`
- `get_current_learning_resource`
- `clear_learning_resource`
- `save_flashcard`
- `end_practice_review`

The server forwards these tools to firmware:

- `set_brightness`
- `set_volume`
- `set_speaker`
- `set_external_speaker_gain`
- `show_text`
- `play_sound`
- `play_melody`
- `power_off`
- `get_device_status`

Unknown server-side tool calls fall through as `tool_call` messages.

## HTTP Endpoints

- `GET /health`
- `GET /ws`
- `GET /session/:chatId?device_id=<device-id>`
- `GET /history/:deviceId?device_id=<device-id>`
- `GET /firmware/check?version=<number>`
- `GET /firmware/download`
- `POST /device/face-control`

Session and history access require the matching device id or a configured
history/admin token. OTA endpoints depend on the R2 `STORAGE` binding.
