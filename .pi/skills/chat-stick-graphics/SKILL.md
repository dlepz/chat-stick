---
name: chat-stick-graphics
description: Use when working on chat-stick firmware graphics, UI rendering, M5StickS3 display code, TextDisplay.cpp, reactive face/scene/menu visuals, LovyanGFX/M5Unified drawing primitives, sprites, fonts, colors, clipping, performance, or display latency. Clones/updates LovyanGFX into /tmp for API reference before graphics work.
---

# Chat Stick Graphics

Use this skill whenever changing the chat-stick display/UI/graphics stack, especially files under `firmware/src/ui/`, display-related state in `firmware/src/state/StateTypes.h`, or rendering decisions in `firmware/src/app/AppController.cpp`.

## Required first step: refresh LovyanGFX reference

Before making graphics/display changes, ensure LovyanGFX is available locally for reference:

```bash
if [ -d /tmp/LovyanGFX/.git ]; then
  git -C /tmp/LovyanGFX fetch --depth=1 origin || true
  git -C /tmp/LovyanGFX reset --hard origin/master || true
else
  git clone --depth=1 https://github.com/lovyan03/LovyanGFX /tmp/LovyanGFX
fi
```

Why: M5Unified display APIs are backed by LovyanGFX-style primitives. The project usually calls through `M5.Display`/M5Unified, but LovyanGFX examples and headers are the best local reference for sprites, clipping, text datum, colors, pushImage, draw primitives, and performance patterns.

Useful places to inspect after cloning:

```bash
find /tmp/LovyanGFX -maxdepth 3 -type f \( -name '*.hpp' -o -name '*.h' -o -name '*.cpp' -o -name '*.ino' -o -name '*.md' \) | head
rg -n "class LGFX|LGFX_Sprite|setClipRect|pushSprite|drawString|setTextDatum|startWrite|endWrite|fillRect" /tmp/LovyanGFX
```

## Project graphics map

Primary project files:

- `firmware/src/ui/TextDisplay.cpp` — custom immediate-mode renderer for chat/status/menu/scene/face UI.
- `firmware/src/ui/TextDisplay.h` — display renderer interface.
- `firmware/src/state/StateTypes.h` — `DisplayState`, render inputs, UI flags.
- `firmware/src/app/AppController.cpp` — builds `DisplayState`, decides what is visible and when `_screenDirty` is set.
- `firmware/src/Config.h` — screen dimensions and display constants.

## Best practices

Read the detailed guide before larger visual changes:

- [references/best-practices.md](references/best-practices.md)

Short version:

1. Keep rendering deterministic from `DisplayState`.
2. Avoid blocking in render paths.
3. Minimize full-screen redraws unless necessary.
4. Prefer small primitives and cached layout over complex per-frame allocation.
5. Preserve push-to-talk responsiveness; graphics must not interfere with audio capture/playback.
6. Test by building firmware with `cd firmware && pio run`.
7. For anything involving interrupts, recording animation, playback, or reconnect states, test on-device with serial logs.

## Verification checklist

After graphics work:

```bash
cd firmware
pio run
```

On device, verify:

- boot screen still appears quickly,
- Ready / Recording / Thinking / Playing states are visually distinct,
- menus are readable on 135×240 display,
- reactive face animation does not slow recording,
- long text paginates or wraps acceptably,
- brightness changes still apply,
- no crash/reboot during 2–3 minutes of interaction.
