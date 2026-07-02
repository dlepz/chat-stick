# Agent Notes

This repository follows Steve Ruiz's chat-stick project, with local language
learning additions layered on top. When pulling upstream work, prefer additive
ports and small compatible refactors over branch-wide rewrites.

## Preserve The Learning Flow

Keep the language learning bridge intact:

- `server/src/flashcard-api.ts`
- `server/src/learning-types.ts`
- learning tools in `server/src/live-session.ts`
- `FLASHCARD_APP_BASE_URL`
- `FLASHCARD_APP_BRIDGE_TOKEN`
- `CHAT_STICK_LINK_TOKEN`
- `CONVERSATION_END_REVIEW_ENABLED`
- `TURN_FEEDBACK_ENABLED`

Do not remove these while merging upstream server changes. If an upstream branch
extracts or rewrites `LiveSession`, port behavior carefully and re-run TypeScript
before committing.

## Current Layout

- `server/` is the Cloudflare Worker and Durable Object server.
- `devices/firmware/` is the newer Ruiz multi-device firmware layout.
- `firmware/` is the local legacy firmware tree kept for compatibility.
- `apps/reachy-chat-stick/` is an additive Reachy Mini app from upstream.

## Checks

Run the focused checks for the area you touch:

```bash
cd server && npx tsc --noEmit
python3 -m compileall apps/reachy-chat-stick/reachy_chat_stick
```

PlatformIO firmware builds require `pio` to be installed locally.
