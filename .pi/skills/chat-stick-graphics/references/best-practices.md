# Chat Stick Graphics Best Practices

This project uses a small custom renderer rather than a full graphics engine. The display stack is:

```text
AppController builds DisplayState
  -> TextDisplay renders it
  -> M5Unified / LovyanGFX-style drawing primitives
  -> M5StickS3 ST7789 LCD
```

The device is tiny and audio-focused. Display changes should feel nice, but must never compromise push-to-talk latency, audio capture, playback, or WebSocket polling.

## Hardware/UI constraints

- Screen: 135×240 physical LCD, used as landscape `240×135` in project constants.
- User attention: glanceable. Prefer 1–3 short lines over dense paragraphs.
- Controls: A = push-to-talk/action, B = menu/page/control.
- CPU/battery: avoid expensive redraws and avoid high-frequency full-screen animation.
- Audio priority: recording and playback should remain smooth even while the face/UI animates.

## Rendering architecture rules

### 1. Keep `TextDisplay` mostly pure

`TextDisplay` should render from `DisplayState`. Avoid making it own app logic.

Good:

```cpp
DisplayState state;
state.appState = _appState;
state.bodyText = buildBodyText();
_display.render(state);
```

Avoid:

```cpp
// TextDisplay decides whether to connect WiFi, send tools, mutate app state, etc.
```

If a new visual needs new data, add a field to `DisplayState` and populate it in `AppController::buildDisplayState()`.

### 2. Use `_screenDirty` intentionally

Most UI updates should set `_screenDirty = true` and let `renderIfNeeded()` redraw once.

For animations, use existing timing gates:

- scene frame tick around 7 Hz (`_lastSceneFrameMs`),
- reactive face tick around 30 Hz (`_lastFaceRenderMs`) only when visible.

Do not render every loop unconditionally.

### 3. Do not allocate in hot render paths

Avoid heap allocation, large temporary `String` churn, or dynamic containers inside per-frame drawing code. The ESP32-S3 can handle some allocation, but fragmentation and latency matter.

Prefer:

- precomputed layout,
- stack/local small values,
- simple loops,
- existing `String` content prepared in `AppController`.

### 4. Keep drawing operations bounded

Full-screen fill is okay for state transitions or low-rate redraws. For animation, prefer updating only necessary regions if practical.

If using sprites (`LGFX_Sprite`), consider:

- memory cost: width × height × bytes per pixel,
- PSRAM availability,
- whether a sprite avoids flicker enough to justify memory.

For small UI elements, direct primitives are usually fine.

## LovyanGFX/M5Unified practices

### Clone LovyanGFX reference before display work

```bash
if [ -d /tmp/LovyanGFX/.git ]; then
  git -C /tmp/LovyanGFX fetch --depth=1 origin || true
  git -C /tmp/LovyanGFX reset --hard origin/master || true
else
  git clone --depth=1 https://github.com/lovyan03/LovyanGFX /tmp/LovyanGFX
fi
```

Useful searches:

```bash
rg -n "LGFX_Sprite|pushSprite|setClipRect|clearClipRect|setTextDatum|drawString|drawRoundRect|startWrite|endWrite" /tmp/LovyanGFX
```

### Batch writes when doing many primitives

For many small primitives, LovyanGFX supports `startWrite()`/`endWrite()` patterns. Check compatibility through M5Unified display object before adding.

Conceptually:

```cpp
M5.Display.startWrite();
// many draw calls
M5.Display.endWrite();
```

Do not hold a write transaction across slow logic, delays, network calls, or audio operations.

### Use clipping deliberately

Clip regions can prevent accidental overwrites and reduce redraw work, but stale clips cause confusing bugs. Always clear/reset clips after use.

```cpp
M5.Display.setClipRect(x, y, w, h);
// draw within area
M5.Display.clearClipRect();
```

### Be careful with text datum and font state

Display text state is global-ish on the display object. When changing datum, font, color, or text size, set the required state explicitly before drawing a component. Do not rely on a previous component's state.

## UI design conventions for this repo

### Header/body/footer layout

Keep the current mental model:

- Header: mode/status/battery/time.
- Body: main message, menu, face, scene, or review card.
- Footer: button hints.

New visuals should not hide essential controls unless intentionally in a scene/face mode.

### State colors and affordances

Preserve fast recognition of states:

- Connecting: calm/progress.
- Ready: stable and inviting.
- Recording: obvious listening indicator and/or animated face.
- Thinking: waiting state, no implication that the user should speak yet.
- Playing: speaking/assistant output.
- Error: readable recovery instructions.

### Text length

The display is too small for dense prose. Prefer:

- short labels,
- pagination for long assistant text,
- clear footer hints,
- no more than ~7 short body lines.

If adding a visual mode, define what happens with long text before implementing it.

## Performance and audio safety

The loop does all of these:

- reads buttons,
- captures mic chunks,
- polls WebSocket,
- advances speaker playback,
- updates power state,
- renders display.

Graphics work must not introduce long blocking sections. Avoid:

- `delay()` in drawing code,
- network calls from rendering,
- filesystem/flash writes from rendering,
- excessive serial logging per frame,
- expensive pixel loops at high FPS.

If a visual effect needs animation, drive it from timestamps/frame counters and render one incremental frame per loop.

## Testing workflow

Build first:

```bash
cd firmware
pio run
```

Upload if a device is connected:

```bash
pio run -t upload --upload-port /dev/cu.usbmodemXXXX
```

Monitor:

```bash
pio device monitor --port /dev/cu.usbmodemXXXX
```

Manual test checklist:

1. Boot and WiFi connect screen.
2. Ready screen.
3. Hold A: Recording screen/face animation remains smooth.
4. Release A: Thinking appears immediately.
5. Playback: text/audio state transitions to Playing.
6. Long assistant message: pagination works with B.
7. Menu: all items readable and selected item clear.
8. Interrupt playback by pressing A: visual state changes to Recording quickly.
9. Brightness tool: display remains visible and setting persists.
10. Let device idle: power dim/screen-off behavior still works.

## When to inspect LovyanGFX examples

Use `/tmp/LovyanGFX` especially when implementing:

- sprites/double buffering,
- custom fonts,
- transparent drawing,
- image push/draw routines,
- clipping/scissor regions,
- anti-aliased primitives,
- text datum/alignment changes,
- performance optimizations.

Do not copy large examples blindly. Adapt only the minimal pattern needed for `TextDisplay`.
