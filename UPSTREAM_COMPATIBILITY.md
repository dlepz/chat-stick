# Upstream Compatibility Notes

This repo tracks Steve Ruiz's `chat-stick` while preserving the local language
learning flow. Do not raw-merge `upstream/waveshare` into `main`: that branch is
an older divergent shape that deletes the Reachy app, removes the current
`devices/firmware/` layout, and drops the local learning bridge files.

## Already Integrated

- `upstream/main` is merged into `main`.
- `upstream/reachy` is merged into `main`.
- Local language-learning files are preserved:
  - `server/src/flashcard-api.ts`
  - `server/src/learning-types.ts`
  - learning tools in `server/src/live-session.ts`
  - flashcard/review/turn-feedback environment variables

## Ported From `upstream/waveshare`

- Branch-aware Worker deploy helpers:
  - `server/scripts/worker-name.mjs`
  - `server/scripts/deploy-worker.mjs`
  - `npm run deploy`
- Protocol/refactor/setup docs:
  - `AGENTS.md`
  - `PROTOCOL.md`
  - `REFACTOR.md`
  - README branch-aware deploy instructions
- Server behavior:
  - transcript overlap/delta handling via `server/src/transcript-delta.ts`
  - low-amplitude audio turns are forwarded instead of discarded as silence
  - latest-turn debug audio endpoint: `/debug/audio/:deviceId/latest.wav|json`
  - canonical aliases for `user-instructions.md`
  - D1 write verification for file tools
- Setup hygiene:
  - `.clangd` compile flag removals
  - generated `compile_commands.json` ignores

## Intentionally Not Merged Into `main`

- The branch-wide Waveshare firmware layout. Current `main` keeps both:
  - newer Ruiz multi-device firmware under `devices/firmware/`
  - local legacy firmware under `firmware/`
- The server extraction commits (`prompt-builder`, `tool-router`,
  `gemini-client`, etc.). They are useful refactor candidates, but they must be
  ported one at a time so the language-learning tools stay intact.
- The older `upstream/waveshare` image pipeline shape. Current `main` keeps the
  newer device-size-aware Imagen path and original/dithered archival metadata.
- The older `upstream/waveshare` router shape, which predates local auth,
  learning endpoints, flashcard endpoints, and multi-device OTA paths.

## Safe Future Path

Use a dedicated branch for deeper refactors, then verify:

```bash
cd server && npx tsc --noEmit
python3 -m compileall apps/reachy-chat-stick/reachy_chat_stick
```

PlatformIO firmware builds require `pio` to be installed locally.
