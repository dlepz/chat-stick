# Refactor Notes

The current architecture is broadly sound for an embedded voice device plus a
language-learning companion flow. Preserve the major boundaries and extract only
when a change naturally touches the same responsibility.

## Main Boundaries

- Firmware `AppController` coordinates state, input, display, service wiring,
  turn flow, power behavior, and update flow.
- Firmware `AudioService` owns microphone capture, speaker playback, I2S, codec
  setup, and playback buffering.
- Firmware `LiveSessionService` owns device-side websocket transport plus HTTP
  calls for session restore, history, and OTA.
- Firmware `WiFiService` owns network connection and provisioning.
- Firmware `SettingsStore` owns persisted settings.
- Firmware `TextDisplay` owns framebuffer/UI rendering.
- The Cloudflare Worker and Durable Object own AI protocol details, tool
  execution, persistence, image generation, language-learning state, and Gemini
  integration.

## Repo-Specific Risk

`server/src/live-session.ts` is intentionally carrying both upstream chat-stick
behavior and local language-learning behavior. It includes Gemini websocket
lifecycle, device websocket lifecycle, tool declarations, tool dispatch,
prompt/system instruction construction, conversation persistence, debug audio,
image handling, files/email/docs/search tools, flashcards, learning-resource
loading, turn feedback, and practice review.

The Durable Object boundary is good, but broad upstream extraction should not be
merged wholesale until the learning tools are explicitly preserved.

## Suggested Path

1. Keep the current layout. Avoid refactoring for aesthetics alone.
2. Extract small helpers when they reduce risk, such as transcript delta
   handling or deployment naming.
3. When porting an upstream server refactor, move one responsibility at a time
   and keep the public Durable Object behavior unchanged.
4. Update `PROTOCOL.md` whenever server/device JSON messages change.
5. Keep device firmware ports additive where possible; avoid deleting
   `devices/firmware/`, `firmware/`, or `apps/reachy-chat-stick/` in a merge.
