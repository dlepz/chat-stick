# Device/Server Protocol

This document describes the current protocol between the ESP32 firmware and the
Cloudflare Worker. The source of truth is still the implementation:

- `firmware/src/services/LiveSessionService.cpp`
- `server/src/live-session.ts`
- `server/src/index.ts`
- `server/src/gemini-tools.ts`

Update this document when adding, renaming, or changing protocol messages.

## WebSocket Session

The device opens a WebSocket to:

```text
/ws?device_id=<device-id>[&chat_id=<chat-id>][&voice=<voice-name>]
```

`device_id` selects the Durable Object instance. `chat_id` is optional; when it
is omitted, the server creates a new chat id and returns it in a `session`
message. `voice` is optional; the server resolves it to a supported Gemini voice
or falls back to the default voice.

WebSocket text frames are UTF-8 JSON messages. WebSocket binary frames are raw
PCM audio. There is no explicit protocol version today, so breaking changes must
be coordinated between firmware and server.

## Binary Audio

Device to server binary frames are microphone audio:

- signed 16-bit PCM
- mono
- 16 kHz
- little-endian sample order from the ESP32
- normally sent as 100 ms chunks

The server forwards these chunks to Gemini as `audio/pcm;rate=16000`. A
`start` JSON message resets turn metrics. A `stop` JSON message ends the audio
stream after any pending audio is flushed. Very short turns are ignored by the
server and reported back with `ignore_audio`.

Server to device binary frames are model playback audio:

- signed 16-bit PCM
- mono
- 24 kHz
- little-endian sample order

The firmware ignores binary frames shorter than 16 bytes.

## Device To Server JSON

### `start`

Sent when the user starts push-to-talk recording.

```json
{ "type": "start" }
```

Server behavior:

- resets current turn metrics
- clears pending audio
- reconnects Gemini if needed

### `stop`

Sent when the user releases push-to-talk.

```json
{ "type": "stop" }
```

Server behavior:

- sends Gemini `audioStreamEnd` when ready
- buffers the stop if Gemini is still connecting
- may send `ignore_audio` instead for an accidental short turn

### `text`

Optional text input path. The current firmware does not use this for normal
push-to-talk, but the server accepts it.

```json
{ "type": "text", "content": "hello" }
```

Server behavior:

- forwards `content` to Gemini as realtime text input when Gemini is ready

### `tool_response`

Sent after the firmware handles a device-side tool call.

```json
{
  "type": "tool_response",
  "name": "set_volume",
  "id": "function-call-id",
  "result": "Volume set to 180"
}
```

`result` is usually a string. `get_device_status` returns a JSON string. The
server wraps the value in Gemini's `toolResponse` envelope.

## Server To Device JSON

### `session`

Sent immediately after the WebSocket connects and again when the server starts a
fresh conversation.

```json
{ "type": "session", "chatId": "uuid" }
```

When `reset` is true, the firmware clears the current conversation UI and stores
the new chat id.

```json
{ "type": "session", "chatId": "uuid", "reset": true }
```

### `ready`

Sent after Gemini setup completes.

```json
{ "type": "ready" }
```

The firmware uses this to enter the ready state.

### `settings`

Sent after `ready` with server-controlled runtime settings.

```json
{
  "type": "settings",
  "power": {
    "dim_ms": 60000,
    "screen_off_ms": 120000,
    "light_sleep_ms": 300000,
    "power_off_ms": 600000
  }
}
```

The firmware falls back to its compiled defaults if any field is missing.

### `transcript`

Sent for live captions. `text` is an append-only delta, not the full transcript.

```json
{ "type": "transcript", "source": "user", "text": "hello" }
```

```json
{ "type": "transcript", "source": "model", "text": "Hi there." }
```

Valid `source` values are `user` and `model`.

### `turn_complete`

Sent when Gemini reports turn completion.

```json
{ "type": "turn_complete" }
```

The server also persists the completed exchange to D1.

### `drop_audio`

Sent when Gemini reports that the model output was interrupted.

```json
{ "type": "drop_audio" }
```

The firmware drops queued playback audio for the interrupted turn.

### `ignore_audio`

Sent when the server intentionally ignores a captured turn.

```json
{
  "type": "ignore_audio",
  "reason": "too_short",
  "bytes": 3200,
  "avg_abs": 17,
  "chunks": 1
}
```

Current `reason` values:

- `too_short`

### `tool_call`

Sent when Gemini calls a device-side tool.

```json
{
  "type": "tool_call",
  "name": "set_brightness",
  "id": "function-call-id",
  "args": { "level": 120 }
}
```

The firmware executes the tool and replies with `tool_response`.

### `voice_changed`

Sent after the server accepts a `set_voice` tool call.

```json
{ "type": "voice_changed", "voice": "Kore" }
```

The firmware persists the selected voice. The server reconnects Gemini so the
next spoken response uses the new voice.

### `show_image_pending`

Sent when image generation has started.

```json
{ "type": "show_image_pending" }
```

The firmware shows a pending image state while generation continues in the
background.

### `show_image_failed`

Sent when image generation or image processing fails.

```json
{ "type": "show_image_failed" }
```

### `show_image`

Sent when a generated image is ready.

```json
{
  "type": "show_image",
  "data": "<base64 packed 1-bit bitmap>",
  "width": 232,
  "height": 112,
  "key": "chat-stick/assets/device/images/chat-stamp.png"
}
```

`data` is a base64-encoded 1-bit bitmap, packed MSB first. `width` and `height`
must match the firmware display image slot, currently 232x112. `key` is optional
and is present only when the server successfully stores the dithered PNG in R2.

### `error`

Sent for server-side session errors.

```json
{
  "type": "error",
  "category": "gemini_unavailable",
  "message": "Failed to connect to AI"
}
```

The firmware displays `message` and uses `category` for logging/state handling.

## Tool Routing

The server handles these tools without forwarding them to the firmware:

- `search_docs`
- `web_fetch`
- `fetch_url`
- `list_files`
- `read_file`
- `write_file`
- `append_to_file`
- `search_files`
- `set_voice`
- `email_me` when email is configured
- `show_image`
- `new_conversation`
- `new_chat`

The server forwards these tools to the firmware:

- `set_brightness`
- `set_volume`
- `show_text`
- `play_sound`
- `play_melody`
- `power_off`
- `get_device_status`

Any Gemini tool call not handled server-side also falls through to the device as
a `tool_call`.

## HTTP Companion Endpoints

The firmware also uses HTTP endpoints alongside the WebSocket session.

### `GET /session/:chatId?device_id=<device-id>`

Returns the saved conversation row for session restore:

```json
{
  "chat_id": "uuid",
  "device_id": "waveshare-amoled18-live",
  "last_message": "Previous assistant reply",
  "updated_at": "2026-05-10T12:00:00.000Z"
}
```

Authorized when the query `device_id` matches the saved row's device id, or when
the request includes the configured history token.

### `GET /history/:deviceId?device_id=<device-id>`

Returns up to 10 recent conversations for the device:

```json
[
  {
    "chat_id": "uuid",
    "last_message": "Previous assistant reply",
    "updated_at": "2026-05-10T12:00:00.000Z"
  }
]
```

Authorized when the query `device_id` matches the path device id, or when the
request includes the configured history token.

### `GET /firmware/check?version=<number>`

Returns update availability:

```json
{
  "available": true,
  "latest_version": 9,
  "notes": "",
  "download_url": "https://example.com/firmware/download"
}
```

### `GET /firmware/download`

Returns the latest firmware binary from R2 when storage is configured and a
firmware object is available.

### `GET /health`

Returns `ok`.

## Session Persistence

The firmware persists `chat_id` and `voice` in NVS. On reconnect, it sends the
saved `chat_id` and `voice` in the WebSocket query string. The server sends a
`session` message on every connection so the firmware can persist a newly
generated chat id.

`new_conversation` and `new_chat` create a new server-side chat id, send
`session` with `reset: true`, clear the server's in-memory transcript state, and
reconnect Gemini.
