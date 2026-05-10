# Refactor Notes

The current architecture is broadly sound for an embedded voice device. The
main boundaries are useful and should be preserved:

- `AppController` orchestrates app state, input flow, rendering decisions, and
  service coordination.
- `AudioService` owns mic capture, speaker playback, I2S, codec setup, and
  playback buffering.
- `LiveSessionService` owns device-side websocket transport plus related HTTP
  calls for session restore, history, and OTA.
- `WiFiService` owns network connection and provisioning.
- `SettingsStore` owns persisted settings.
- `TextDisplay` owns framebuffer/UI rendering.
- The Cloudflare Worker and Durable Object handle the heavier AI, tool,
  persistence, and session logic that should not live on the ESP32.

## What Works

- The firmware is service-oriented without being overly abstract.
- Hardware concerns are mostly below application logic.
- The server-side Durable Object is a good per-device/session boundary.
- The ESP32 firmware stays focused on capture, playback, input, display,
  settings, and transport.
- The server owns AI protocol details, tool execution, persistence, and Gemini
  integration, which is the right distribution of responsibilities.

## Main Risk

`AppController` is becoming the firmware's gravitational center. It currently
owns:

- app state transitions
- callback wiring
- display state construction
- menu behavior
- recording/thinking/playing turn flow
- power behavior
- update flow
- transcript reveal timing
- history restore
- error recovery

This is acceptable at the current size, but it is the first place likely to
accumulate regressions as features grow.

The same pattern exists server-side in `server/src/live-session.ts`, which owns:

- Gemini websocket lifecycle and protocol handling
- device websocket lifecycle
- tool declarations and tool dispatch
- prompt/system instruction construction
- conversation persistence
- debug audio handling
- image handling
- file/email/docs/search tool integration

The Durable Object boundary is good, but the implementation can be split as it
grows.

## Suggested Refactor Path

Do not do a broad rewrite. Keep the current layout and extract pieces only when
new work naturally touches them.

1. Extract firmware turn flow from `AppController`.

   Candidate names:

   - `TurnController`
   - `ConversationState`
   - `VoiceTurnState`

   Candidate responsibilities:

   - recording/thinking/playing state for one turn
   - `_turnComplete`, `_turnHasAudio`, `_pendingTurnReset`
   - transcript accumulation
   - reveal timing state
   - ignored-audio cleanup
   - start/stop recording decisions that do not need direct button knowledge

2. Extract repeated HTTP endpoint logic from `LiveSessionService`.

   Session restore, history fetch, firmware check, and OTA download all repeat
   endpoint iteration and secure/insecure HTTP setup. A small helper could own:

   - endpoint URL construction
   - `WiFiClient` vs `WiFiClientSecure`
   - CA selection
   - retrying across configured endpoints
   - common logging for failed HTTP calls

3. Split server `LiveSession` internally.

   Keep the Durable Object public API, but move responsibilities into modules:

   - `GeminiClient`: Gemini websocket setup, send/receive protocol helpers
   - `ToolRouter`: tool schema and dispatch
   - `ConversationStore`: D1 reads/writes for conversations and logs
   - `PromptBuilder`: system instruction and voice/location/user preference text
   - `DebugAudioStore`: latest debug WAV/metadata handling

4. Document the device/server JSON protocol.

   The protocol currently lives implicitly in string literals and switch cases.
   Add a small protocol document or shared schema covering:

   - device to server messages
   - server to device messages
   - binary audio frame expectations
   - tool call/response envelopes
   - session restore and chat id behavior

5. Watch dynamic allocation in hot firmware paths.

   The current `String` usage is workable, but long-lived embedded firmware can
   become sensitive to heap fragmentation. Prefer tighter allocation patterns in
   paths that run frequently:

   - audio loop
   - websocket message parsing
   - transcript accumulation
   - display render/reveal paths

## Near-Term Rule Of Thumb

Keep the service layout. Avoid refactoring for aesthetics alone. When adding a
feature makes `AppController`, `LiveSessionService`, or `live-session.ts`
noticeably harder to reason about, extract the smallest coherent responsibility
at that point.
