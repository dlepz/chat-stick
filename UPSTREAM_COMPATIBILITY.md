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
  - debug audio, web fetch, docs search, email, voice, and image tool handling are
    split into focused helper modules
  - Imagen vocabulary requests explicitly call `show_image` with concrete
    visual prompts while preserving D1/R2 image history and recall
  - canonical aliases for `user-instructions.md`
  - D1 write verification for file tools
- Setup hygiene:
  - `.clangd` compile flag removals
  - generated `compile_commands.json` ignores

## Refreshed Audit: 2026-07-02

After fetching `origin` and `upstream`, local `main` is synced with
`origin/main`. `upstream/main` and `upstream/reachy` have no unmerged commits
relative to this repo. The remaining upstream-only work is on
`upstream/waveshare`, which is still a divergent branch rather than a direct
successor to this repo's current shape.

The `upstream/waveshare` branch was rechecked for portable behavior. Its useful
runtime changes have either already been merged from `upstream/main` /
`upstream/reachy` or ported manually into this repo. The remaining differences
are primarily broad extractions and older layout changes that would remove or
replace local behavior if merged directly.

## Intentionally Not Merged Into `main`

- The branch-wide Waveshare firmware layout. Current `main` keeps both:
  - newer Ruiz multi-device firmware under `devices/firmware/`
  - local legacy firmware under `firmware/`
- The server extraction commits (`prompt-builder`, `tool-router`,
  `gemini-client`, etc.). They are useful refactor candidates, but they must be
  ported one at a time so the language-learning tools stay intact.
- The older `upstream/waveshare` image pipeline shape. Current `main` keeps the
  newer device-size-aware Imagen path, original/dithered archival metadata, and
  saved-image recall through D1/R2.
- The older `upstream/waveshare` router shape, which predates local auth,
  learning endpoints, flashcard endpoints, and multi-device OTA paths.
- The older `upstream/waveshare` deploy and firmware scripts, which assume the
  top-level Waveshare-only firmware layout rather than this repo's
  `devices/firmware/` layout plus local compatibility tree.

## Safe Future Path

Use a dedicated branch for deeper refactors, then verify:

```bash
cd server && npx tsc --noEmit
python3 -m compileall apps/reachy-chat-stick/reachy_chat_stick
```

PlatformIO firmware builds require `pio` to be installed locally.
