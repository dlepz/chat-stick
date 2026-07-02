# Implementation Guide: Make Chat Turn Handling Robust Again

This guide is for implementing a robust fix for the “stuck in Thinking until the next user turn / double response” behavior in the M5StickS3 chat stick.

The core theme is: **treat push-to-talk as an explicit turn protocol, not as a loose stream of audio plus hope that Gemini VAD ends the turn correctly.** Tool calls made the old looseness much more visible, because a tool call can now happen at the wrong phase, pause model generation, or produce a tool-only turn that the firmware currently does not count as a response.

---

## 0. Short diagnosis

Current path:

```text
Device A down  -> firmware sends {type:"start"}, then binary PCM chunks
Device A up    -> firmware sends {type:"stop"}
Server stop    -> sends 1 second of silence to Gemini and relies on Gemini VAD
Gemini reply   -> server forwards audio chunks + transcripts + turn_complete
Tool call      -> server/device executes it and sends toolResponse
Firmware       -> only leaves Thinking after turn_complete if it saw audio or model text
```

This has several liminal states:

1. **Gemini can decide the user stopped speaking before the button is released.**
   - The current bridge relies on automatic activity detection plus trailing silence.
   - In `wrangler-dev.log`, there are smoking-gun examples where Gemini emits a `save_flashcard` tool call while audio chunks are still arriving, before `[Device] stop`.
   - That means the model can execute tools or begin a response based on a partial utterance.

2. **Tool-only turns do not count as real turns on the firmware.**
   - `AppController::onTurnComplete` ignores `turn_complete` unless `_turnHasAudio || _turnHasModelText`.
   - If Gemini calls a tool and then completes without audio, the firmware can sit in Thinking until timeout.

3. **Firmware timeout is local only.**
   - `processThinkingTimeout()` returns the device to Ready, but it does not tell the server/Gemini to cancel the pending turn.
   - A late Gemini response can arrive during the next user turn, causing “two responses where one ought to be”.

4. **Tool responses are fire-and-forget.**
   - Server-side tools send `toolResponse` from many scattered branches.
   - Device-side tools are forwarded to Gemini only if `geminiWs && geminiReady` at that exact moment; otherwise the response is silently lost.
   - There is no watchdog for “tool responded, but model never continued”.

5. **There is no explicit per-turn state machine.**
   - The server has global `currentUserText` and `currentAssistantText`, but not a strong concept of: recording, waiting for model, executing tools, streaming answer, completed, canceled.
   - Without that, stale model output can be applied to the next physical button turn.

The fix should be layered, not a one-line timeout increase.

---

## 1. Target behavior

After the implementation, these must be true:

1. **One button hold equals one user activity.**
   - Gemini must not receive an end-of-turn signal until the user releases A.
   - No tool calls, model audio, DB commits, or turn feedback should occur while the firmware is still recording, except explicit interruption/drop events.

2. **Every user turn ends cleanly.**
   - The device leaves Thinking/Playing after:
     - model audio finishes, or
     - text-only/model-transcript answer completes, or
     - a user-visible tool-only action completes, or
     - a controlled fallback/error completes.

3. **A stuck turn is canceled on both sides.**
   - If firmware times out, it sends `cancel_turn` to the server.
   - The server cancels/reconnects the Gemini Live session or otherwise guarantees stale output will not be forwarded later.

4. **Tool calls cannot wedge the session.**
   - Every tool call gets exactly one `toolResponse` or an explicit timeout/error response.
   - Tool response sending is centralized and logged.
   - If Gemini does not continue after a tool response, the server nudges once, then completes/fails the turn instead of waiting forever.

5. **No double responses.**
   - Late output from an abandoned turn is dropped by server cancellation or by turn-phase checks.

---

## 2. Relevant files

Server:

- `server/src/live-session.ts`
  - Durable Object bridge between device WebSocket and Gemini Live WebSocket.
  - Owns Gemini setup, audio forwarding, tool handling, transcripts, DB persistence.

Firmware:

- `firmware/src/services/LiveSessionService.h`
- `firmware/src/services/LiveSessionService.cpp`
  - Device WebSocket protocol, parses server messages, sends start/stop/audio/tool responses.

- `firmware/src/app/AppController.h`
- `firmware/src/app/AppController.cpp`
  - App state machine: Ready, Recording, Thinking, Playing.
  - Push-to-talk, playback, thinking timeout.

Also useful:

- `firmware/src/services/AudioService.cpp`
  - Playback buffering.
- `server/schema.sql`
  - Conversation/message/tool logs.

---

## 3. Important background for a junior dev

### 3.1 Device state vs Gemini state

The device has local UI states:

```cpp
enum class AppState {
  Connecting,
  Ready,
  Recording,
  Thinking,
  Playing,
  ConfirmReset,
  Error,
};
```

These are not the same as Gemini’s internal state. Currently the bridge assumes they line up naturally:

```text
Recording -> user audio stream
Thinking  -> Gemini generating
Playing   -> device playing Gemini audio
Ready     -> turn complete
```

But the server does not enforce that. Gemini can emit:

- input transcription while the user is still talking,
- output transcription,
- audio,
- `turnComplete`,
- `toolCall`,
- `interrupted`,

in combinations that do not map cleanly to those four UI states unless the bridge has a proper turn protocol.

### 3.2 Tool calls are not final answers

A Gemini `toolCall` means “the model needs this function result before continuing.” It may be followed by:

- model audio/text after the `toolResponse`, or
- another tool call, or
- a `turnComplete` with no audio, especially if the tool itself was the action.

Examples:

- User: “Save this as a flashcard.”
  - Tool call: `save_flashcard(...)`
  - The actual user-visible response may be only “Saved.”
  - If Gemini does not speak after saving, the device still needs to return to Ready.

- User: “Set volume to 50.”
  - Tool call forwarded to device.
  - The physical side effect may be enough.
  - The firmware should not wait forever for audio.

### 3.3 Why trailing silence is not enough

The current server uses:

```ts
private sendTrailingSilence() {
  // 1s of silence at 16kHz 16-bit mono = 32000 bytes
  const silence = new ArrayBuffer(32000)
  this.geminiWs.send(JSON.stringify({
    realtimeInput: {
      audio: { data: base64, mimeType: 'audio/pcm;rate=16000' },
    },
  }))
}
```

This only helps Gemini’s automatic voice activity detector notice an end. It does **not** tell Gemini “ignore pauses until the hardware button is released.”

For push-to-talk, the bridge should use Gemini Live’s explicit activity boundary mode:

- disable automatic activity detection in setup, or configure it so manual activity boundaries dominate,
- send `activityStart` on button press,
- stream audio chunks,
- send `activityEnd` on button release.

Exact Gemini field names should be checked against the current Live API docs, but the intended shape is roughly:

```ts
// In setup
realtimeInputConfig: {
  automaticActivityDetection: { disabled: true },
  activityHandling: 'START_OF_ACTIVITY_INTERRUPTS',
  turnCoverage: 'TURN_INCLUDES_ONLY_ACTIVITY',
}

// On A down
{ realtimeInput: { activityStart: {} } }

// On A up
{ realtimeInput: { activityEnd: {} } }
```

If the API requires `audioStreamEnd` in addition to or instead of `activityEnd`, add it deliberately and test it. Do not blindly keep relying on trailing silence as the primary turn delimiter.

---

## 4. Milestone 1 — Add observability and reproduce the bug

### Goal

Before changing behavior, make the current turn lifecycle visible enough that you can prove the bug is fixed later.

### Files

- `server/src/live-session.ts`
- optionally `firmware/src/app/AppController.cpp`

### Server logging to add

Add a monotonically increasing server turn id:

```ts
private nextServerTurnId = 1
private activeServerTurnId: number | null = null
```

On device `start`:

```ts
this.activeServerTurnId = this.nextServerTurnId++
console.log(`[Turn ${this.activeServerTurnId}] device start`)
```

On device audio chunk logs, include the turn id:

```ts
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] audio #${this.audioChunkCount} ${data.byteLength} bytes`)
```

On device `stop`:

```ts
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] device stop bytes=${this.currentTurnAudioBytes}`)
```

On Gemini messages, log phase-relevant events:

```ts
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] gemini setupComplete`)
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] gemini inputTranscript len=${text.length}`)
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] gemini outputTranscript len=${text.length}`)
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] gemini audio bytes=${raw.byteLength}`)
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] gemini toolCall ${call.name}`)
console.log(`[Turn ${this.activeServerTurnId ?? '-'}] gemini turnComplete`)
```

Also track whether the server believes the device is currently recording:

```ts
private deviceRecording = false
```

Set it true on `start`, false on `stop`. Then log a warning if Gemini emits model output or tool calls while recording:

```ts
if (this.deviceRecording && msg.toolCall) {
  console.warn(`[Turn ${this.activeServerTurnId ?? '-'}] BUG: toolCall while device still recording`)
}
```

Do the same for model audio/output transcription while recording.

### Reproduction script/manual test

1. Start local worker:

   ```bash
   cd server
   npm run dev
   ```

2. Start firmware serial monitor:

   ```bash
   cd firmware
   pio device monitor
   # or python monitor.py
   ```

3. Use the stick and intentionally pause mid-sentence while holding A:

   > “Can you save a flashcard for the German prefix be- ... [pause 2 seconds, still holding] ... and explain three examples?”

4. Watch `wrangler-dev.log` or terminal logs.

### Expected current bad behavior

You may see something like:

```text
[Turn 12] device start
[Turn 12] audio #80 ...
[Turn 12] gemini toolCall save_flashcard
[Turn 12] BUG: toolCall while device still recording
[Turn 12] audio #90 ...
[Turn 12] audio #100 ...
[Turn 12] device stop
```

That is the bug. Gemini should not be deciding the user turn is complete while the physical button is still down.

### Verification for this milestone

- You can reproduce or at least search logs for early tool/model activity.
- Every line needed to trace one physical turn contains the same `[Turn N]` prefix.
- You have a baseline failure log saved before changing behavior.

---

## 5. Milestone 2 — Make push-to-talk explicit with activity boundaries

### Goal

Gemini should not run VAD independently of the hardware button. The server should send explicit activity start/end messages corresponding to A press/release.

### Files

- `server/src/live-session.ts`

### Step 2.1: Add explicit activity config to Gemini setup

In `connectGemini()`, inside the `setup` object, add Live API realtime input config.

Current setup includes:

```ts
setup: {
  model: 'models/gemini-3.1-flash-live-preview',
  generationConfig: {
    responseModalities: ['AUDIO'],
  },
  outputAudioTranscription: {},
  inputAudioTranscription: {},
  systemInstruction: ...,
  tools: ...,
}
```

Add the manual activity configuration near `generationConfig`:

```ts
realtimeInputConfig: {
  automaticActivityDetection: { disabled: true },
  activityHandling: 'START_OF_ACTIVITY_INTERRUPTS',
  turnCoverage: 'TURN_INCLUDES_ONLY_ACTIVITY',
},
```

Important notes:

- Confirm the exact field names/enums against the current Gemini Live API docs.
- If the websocket closes immediately after setup, the field names may be wrong. Temporarily log the close `code` and `reason` and adjust.
- Keep this change small and test it before touching tool logic.

### Step 2.2: Send `activityStart` on device `start`

In `onDeviceMessage`, for `msg.type === 'start'`, after ensuring Gemini is connected/ready, send:

```ts
private sendActivityStart() {
  if (!this.geminiWs || !this.geminiReady) return false
  this.geminiWs.send(JSON.stringify({
    realtimeInput: { activityStart: {} },
  }))
  console.log(`[Turn ${this.activeServerTurnId ?? '-'}] sent activityStart`)
  return true
}
```

Call it when handling `start`.

### Step 2.3: Send `activityEnd` on device `stop`

Replace the primary use of `sendTrailingSilence()` with a manual activity end:

```ts
private sendActivityEnd() {
  if (!this.geminiWs || !this.geminiReady) return false
  this.geminiWs.send(JSON.stringify({
    realtimeInput: { activityEnd: {} },
  }))
  console.log(`[Turn ${this.activeServerTurnId ?? '-'}] sent activityEnd`)
  this.resetCurrentTurnMetrics()
  return true
}
```

Keep `sendTrailingSilence()` temporarily behind a feature flag only if needed:

```ts
private static readonly USE_TRAILING_SILENCE_FALLBACK = false
```

If manual activity works, trailing silence should not be the main mechanism anymore.

### Step 2.4: Preserve ordering while Gemini reconnects

The current server has a pending audio chunk queue:

```ts
private pendingAudioChunks: ArrayBuffer[] = []
private pendingStopAfterReady = false
```

With activity boundaries, ordering matters:

```text
activityStart
PCM chunk 1
PCM chunk 2
...
activityEnd
```

If Gemini is reconnecting, do **not** flush audio before sending the delayed `activityStart`.

Best implementation: replace the audio-only pending queue with a typed input event queue.

Suggested type:

```ts
type PendingGeminiInput =
  | { type: 'activityStart'; turnId: number }
  | { type: 'audio'; turnId: number; data: ArrayBuffer }
  | { type: 'activityEnd'; turnId: number }
  | { type: 'text'; turnId: number; content: string }
```

Then use one helper:

```ts
private sendOrQueueGeminiInput(input: PendingGeminiInput) {
  if (!this.geminiWs || !this.geminiReady) {
    this.queueGeminiInput(input)
    this.connectGemini()
    return false
  }
  this.sendGeminiInputNow(input)
  return true
}
```

`flushPendingAudio()` should become something like `flushPendingGeminiInputs()` and replay events in original order.

### Step 2.5: Text turns also need a boundary

Firmware menu actions use `LiveSessionService::sendText()`, which becomes server `msg.type === 'text'` and is forwarded as:

```ts
realtimeInput: { text: msg.content }
```

Make text prompts behave like synthetic turns:

```text
begin turn
send realtimeInput.text
end turn
wait for model
```

Depending on Gemini’s requirements, this may be:

```ts
this.sendOrQueueGeminiInput({ type: 'activityStart', turnId })
this.sendOrQueueGeminiInput({ type: 'text', turnId, content: msg.content })
this.sendOrQueueGeminiInput({ type: 'activityEnd', turnId })
```

If the Live API docs say text realtime input should not be wrapped in activity messages, still create a server turn state around it and send an explicit end/flush signal if available.

### Verification

Run the same manual paused-sentence test.

Healthy logs should look like:

```text
[Turn 21] device start
[Turn 21] sent activityStart
[Turn 21] audio #1 ...
[Turn 21] audio #80 ...
[Turn 21] device stop bytes=...
[Turn 21] sent activityEnd
[Turn 21] gemini toolCall save_flashcard
[Turn 21] toolResponse sent save_flashcard
[Turn 21] gemini audio bytes=...
[Turn 21] gemini turnComplete
```

Bad logs that must disappear:

```text
BUG: toolCall while device still recording
BUG: model audio while device still recording
BUG: output transcript while device still recording
```

Also verify that normal short turns still work:

- Hold A, say “Hello”, release.
- Response should start normally.
- No new regressions from disabling automatic VAD.

---

## 6. Milestone 3 — Introduce a real server turn state machine

### Goal

Make server behavior phase-aware. The server should know whether it is recording, waiting for Gemini, executing tools, streaming output, completed, or canceled.

### Files

- `server/src/live-session.ts`

### Suggested state model

Add types near the top of `live-session.ts`:

```ts
type TurnPhase =
  | 'idle'
  | 'recording_audio'
  | 'sent_text'
  | 'waiting_for_model'
  | 'executing_tools'
  | 'waiting_after_tools'
  | 'streaming_model'
  | 'completed'
  | 'canceled'

interface ActiveTurn {
  id: number
  phase: TurnPhase
  startedAtMs: number
  stoppedAtMs?: number
  firstModelActivityAtMs?: number
  completedAtMs?: number
  canceledAtMs?: number
  cancelReason?: string

  userAudioBytes: number
  userSamples: number
  userAbsSum: number

  inputText: string
  assistantText: string
  modelAudioBytes: number
  modelTextBytes: number

  toolCallCount: number
  pendingToolIds: Set<string>
  hadToolActivity: boolean
  toolResponseSentAtMs?: number

  committed: boolean
  turnCompleteSentToDevice: boolean
  nudgeSent: boolean
}
```

Add fields:

```ts
private activeTurn: ActiveTurn | null = null
private nextTurnId = 1
```

### Step 3.1: Create helpers

Implement helpers rather than scattering state changes.

```ts
private beginTurn(kind: 'audio' | 'text'): ActiveTurn {
  if (this.activeTurn && !this.isTurnTerminal(this.activeTurn)) {
    this.cancelActiveTurn(`new_${kind}_turn`)
  }

  const turn: ActiveTurn = {
    id: this.nextTurnId++,
    phase: kind === 'audio' ? 'recording_audio' : 'sent_text',
    startedAtMs: Date.now(),
    userAudioBytes: 0,
    userSamples: 0,
    userAbsSum: 0,
    inputText: '',
    assistantText: '',
    modelAudioBytes: 0,
    modelTextBytes: 0,
    toolCallCount: 0,
    pendingToolIds: new Set(),
    hadToolActivity: false,
    committed: false,
    turnCompleteSentToDevice: false,
    nudgeSent: false,
  }

  this.activeTurn = turn
  console.log(`[Turn ${turn.id}] begin ${kind}`)
  return turn
}
```

```ts
private isTurnTerminal(turn: ActiveTurn) {
  return turn.phase === 'completed' || turn.phase === 'canceled'
}
```

```ts
private noteModelActivity(kind: 'audio' | 'text' | 'output_transcript') {
  const turn = this.activeTurn
  if (!turn || this.isTurnTerminal(turn)) return

  if (!turn.firstModelActivityAtMs) {
    turn.firstModelActivityAtMs = Date.now()
  }
  if (turn.phase === 'waiting_for_model' || turn.phase === 'waiting_after_tools') {
    turn.phase = 'streaming_model'
  }
}
```

### Step 3.2: Use the turn object instead of global counters

Currently the server has:

```ts
private currentUserText = ''
private currentAssistantText = ''
private currentTurnAudioBytes = 0
private currentTurnAbsSum = 0
private currentTurnSamples = 0
```

You do not have to delete them all at once, but the end state should be:

- user audio metrics live on `activeTurn`,
- input transcript accumulates on `activeTurn.inputText`,
- assistant transcript accumulates on `activeTurn.assistantText`,
- model audio byte count lives on `activeTurn.modelAudioBytes`,
- DB commit commits one `ActiveTurn` once.

This avoids a preliminary tool `turnComplete` clearing the transcript before the final answer arrives.

### Step 3.3: Phase transitions

Expected transitions:

```text
idle
  -> recording_audio        on device start
  -> sent_text              on device text

recording_audio
  -> waiting_for_model      on device stop + activityEnd
  -> canceled               on cancel/reconnect

sent_text
  -> waiting_for_model      after text sent + activityEnd/flush

waiting_for_model
  -> executing_tools        on toolCall
  -> streaming_model        on audio/output transcript
  -> completed              on turnComplete with visible output or tool-only completion
  -> canceled               on watchdog/cancel/new user turn

executing_tools
  -> waiting_after_tools    after toolResponse sent
  -> canceled               on tool timeout/fatal close

waiting_after_tools
  -> streaming_model        on audio/output transcript
  -> completed              on tool-only completion/fallback
  -> canceled               on watchdog/cancel/new user turn

streaming_model
  -> completed              on turnComplete and playback-safe metadata sent to device
```

### Step 3.4: Guard impossible events

Add warnings for impossible/undesirable events:

- audio chunk received while no active recording turn,
- stop received with no active recording turn,
- Gemini tool call while `phase === 'recording_audio'`,
- Gemini model audio while `phase === 'recording_audio'`,
- Gemini output transcript while `phase === 'recording_audio'`,
- device tool response with no pending tool call,
- turnComplete with no active turn.

Do not crash; log and recover.

### Verification

Create a simple checklist from logs:

- Every `[Device] start` starts a new `[Turn N] begin audio`.
- Every `[Device] stop` moves the same turn to `waiting_for_model`.
- No Gemini model/tool activity appears while phase is `recording_audio`.
- Each turn reaches exactly one terminal phase: `completed` or `canceled`.

You should be able to grep:

```bash
rg "\[Turn .*\] (begin|phase|completed|canceled|BUG)" wrangler-dev.log
```

There should be no `BUG` lines during normal use.

---

## 7. Milestone 4 — Centralize tool execution and tool responses

### Goal

A tool call should never wedge the Gemini session. Every function call gets one response, and sending that response is done in one place.

### Files

- `server/src/live-session.ts`
- `firmware/src/services/LiveSessionService.cpp` only if device tool response protocol changes

### Current problem

`handleGeminiMessage()` currently has many branches like:

```ts
this.geminiWs?.send(JSON.stringify({
  toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: ... }] },
}))
```

Device tool responses are handled separately in `onDeviceMessage`:

```ts
if (msg.type === 'tool_response' && this.geminiWs && this.geminiReady) {
  this.geminiWs.send(JSON.stringify({ toolResponse: ... }))
}
```

Issues:

- Some branches may fail to send if an exception happens before the response.
- Device responses can be dropped if Gemini is not ready.
- Multiple tool calls from one Gemini message are answered as separate messages.
- There is no timeout.
- The server does not start a “waiting after tools” watchdog.

### Step 4.1: Define a common tool result shape

```ts
interface ToolExecutionResult {
  name: string
  id: string
  response: Record<string, unknown>
  handledBy: 'server' | 'device'
  status: 'ok' | 'error'
  error?: string
  durationMs: number
  logResult?: unknown
}
```

### Step 4.2: Make `executeToolCall()` return a result instead of sending to Gemini

Create:

```ts
private async executeToolCall(call: GeminiToolCall): Promise<ToolExecutionResult> {
  const startMs = Date.now()
  try {
    switch (call.name) {
      case 'search_docs':
        return await this.executeSearchDocs(call, startMs)
      case 'save_flashcard':
        return await this.executeSaveFlashcard(call, startMs)
      // etc.
      default:
        return await this.executeDeviceTool(call, startMs)
    }
  } catch (err) {
    const message = err instanceof Error ? err.message : String(err)
    return {
      name: call.name,
      id: call.id,
      response: { error: message },
      handledBy: isDeviceTool(call.name) ? 'device' : 'server',
      status: 'error',
      error: message,
      durationMs: Date.now() - startMs,
    }
  }
}
```

Each old tool branch should move its existing logic into a helper that returns `ToolExecutionResult`.

Do this incrementally:

1. Move `save_flashcard` first, because it is frequently involved in the reported bug.
2. Move one simple server tool, e.g. `search_docs`.
3. Move device tools.
4. Move the rest.

### Step 4.3: Send one `toolResponse` per Gemini `toolCall` message

In `handleGeminiMessage()`:

```ts
if (msg.toolCall) {
  const turn = this.activeTurn
  if (turn) {
    turn.phase = 'executing_tools'
    turn.toolCallCount += msg.toolCall.functionCalls.length
    turn.hadToolActivity = true
    for (const call of msg.toolCall.functionCalls) {
      turn.pendingToolIds.add(call.id)
    }
  }

  const results = await Promise.all(
    msg.toolCall.functionCalls.map((call) => this.executeToolCall(call))
  )

  await this.sendToolResponseBatch(results)

  if (turn && !this.isTurnTerminal(turn)) {
    for (const result of results) turn.pendingToolIds.delete(result.id)
    turn.phase = 'waiting_after_tools'
    turn.toolResponseSentAtMs = Date.now()
    this.startPostToolWatchdog(turn.id)
  }
}
```

Batch sender:

```ts
private async sendToolResponseBatch(results: ToolExecutionResult[]) {
  if (!this.geminiWs || !this.geminiReady) {
    // Do not silently drop. This tool call belongs to the current Gemini session.
    // If the session is gone, fail/cancel the active turn and reconnect.
    this.cancelActiveTurn('gemini_not_ready_for_tool_response')
    return
  }

  const payload = {
    toolResponse: {
      functionResponses: results.map((r) => ({
        name: r.name,
        id: r.id,
        response: r.response,
      })),
    },
  }

  this.geminiWs.send(JSON.stringify(payload))
  console.log(`[Turn ${this.activeTurn?.id ?? '-'}] sent toolResponse batch count=${results.length}`)

  for (const result of results) {
    await this.logToolCall({
      name: result.name,
      args: /* original args: store in result or look up */,
      result: result.logResult ?? result.response,
      handledBy: result.handledBy,
      status: result.status,
      error: result.error,
      durationMs: result.durationMs,
    })
  }
}
```

Store original args either inside `ToolExecutionResult` or log before returning.

### Step 4.4: Device tools should be promises with timeouts

Current device call flow:

```text
Gemini toolCall set_volume
server -> device {type:"tool_call"}
device executes
server receives {type:"tool_response"}
server forwards to Gemini immediately
```

Change to:

```text
Gemini toolCall set_volume
executeDeviceTool() returns a Promise
server -> device {type:"tool_call"}
device executes
server receives {type:"tool_response"}
server resolves Promise
sendToolResponseBatch() sends the result to Gemini
```

Pending map:

```ts
interface PendingDeviceTool {
  name: string
  id: string
  args: unknown
  startMs: number
  resolve: (result: string) => void
  reject: (err: Error) => void
  timeout: ReturnType<typeof setTimeout>
}

private pendingDeviceCalls = new Map<string, PendingDeviceTool>()
```

`executeDeviceTool()`:

```ts
private executeDeviceTool(call: GeminiToolCall, startMs: number): Promise<ToolExecutionResult> {
  return new Promise((resolve) => {
    const timeout = setTimeout(() => {
      this.pendingDeviceCalls.delete(call.id)
      resolve({
        name: call.name,
        id: call.id,
        response: { error: 'device tool timed out' },
        handledBy: 'device',
        status: 'error',
        error: 'device tool timed out',
        durationMs: Date.now() - startMs,
      })
    }, 5000)

    this.pendingDeviceCalls.set(call.id, {
      name: call.name,
      id: call.id,
      args: call.args,
      startMs,
      timeout,
      resolve: (deviceResult) => {
        clearTimeout(timeout)
        resolve({
          name: call.name,
          id: call.id,
          response: { result: deviceResult },
          handledBy: 'device',
          status: 'ok',
          durationMs: Date.now() - startMs,
          logResult: deviceResult,
        })
      },
      reject: () => {},
    })

    this.sendToDevice({
      type: 'tool_call',
      name: call.name,
      id: call.id,
      args: call.args,
    })
  })
}
```

Then in `onDeviceMessage`, replace direct Gemini forwarding with resolver logic:

```ts
if (msg.type === 'tool_response') {
  const pending = this.pendingDeviceCalls.get(msg.id)
  if (!pending) {
    console.warn(`[Tool] stale device tool_response id=${msg.id} name=${msg.name}`)
    return
  }
  this.pendingDeviceCalls.delete(msg.id)
  pending.resolve(String(msg.result ?? ''))
  return
}
```

### Step 4.5: Send visible tool status for user-facing tools

For server-side tools with user-visible side effects, send a lightweight status to the device.

Example after `save_flashcard` succeeds:

```ts
this.sendToDevice({
  type: 'tool_status',
  name: 'save_flashcard',
  status: 'ok',
  text: `Saved flashcard`,
})
```

Do not overuse this for invisible search tools. Good candidates:

- `save_flashcard`
- `load_learning_resource`
- `clear_learning_resource`
- maybe `end_practice_review` fallback

This gives the firmware something user-visible even if Gemini decides not to speak after a tool.

### Verification

1. Ask for a flashcard save.
2. Logs should show:

```text
[Turn 31] gemini toolCall save_flashcard
[Turn 31] executing tool save_flashcard
[Turn 31] sent toolResponse batch count=1
[Turn 31] phase waiting_after_tools
```

3. There should be no direct scattered `toolResponse` sends left outside the central helper, except maybe during the migration before all tools are moved.
4. Device tool timeout test:
   - Temporarily make firmware ignore one tool call, or disconnect the device after the server forwards a tool call.
   - Server should send an error `toolResponse` to Gemini after ~5s and complete/fail the turn, not wait forever.

---

## 8. Milestone 5 — Teach firmware about tool-only completions

### Goal

The firmware should treat a completed tool action as a valid response, even if no model audio was generated.

### Files

- `firmware/src/services/LiveSessionService.h`
- `firmware/src/services/LiveSessionService.cpp`
- `firmware/src/app/AppController.h`
- `firmware/src/app/AppController.cpp`

### Step 5.1: Extend turn_complete payload from the server

Change server `turn_complete` messages from:

```ts
this.sendToDevice({ type: 'turn_complete' })
```

to include metadata:

```ts
this.sendToDevice({
  type: 'turn_complete',
  turn_id: turn.id,
  had_audio: turn.modelAudioBytes > 0,
  had_model_text: turn.assistantText.trim().length > 0,
  had_tool_activity: turn.hadToolActivity,
  synthetic: false,
  reason: 'model_turn_complete',
})
```

For fallback/error completion:

```ts
this.sendToDevice({
  type: 'turn_complete',
  turn_id: turn.id,
  had_audio: false,
  had_model_text: false,
  had_tool_activity: turn.hadToolActivity,
  synthetic: true,
  reason: 'tool_response_timeout' // or similar
})
```

### Step 5.2: Add a firmware struct for completion info

In `LiveSessionService.h`:

```cpp
struct TurnCompleteInfo {
  uint32_t turnId = 0;
  bool hadAudio = false;
  bool hadModelText = false;
  bool hadToolActivity = false;
  bool synthetic = false;
  String reason;
};
```

Change callback type:

```cpp
std::function<void(const TurnCompleteInfo &)> onTurnComplete;
```

This requires updating callback wiring in `AppController.cpp`.

### Step 5.3: Parse metadata in `LiveSessionService::handleMessage()`

Current:

```cpp
if (strcmp(type, "turn_complete") == 0) {
  Serial.println("[Server] Turn complete");
  if (_callbacks.onTurnComplete) {
    _callbacks.onTurnComplete();
  }
  return;
}
```

Change to:

```cpp
if (strcmp(type, "turn_complete") == 0) {
  TurnCompleteInfo info;
  info.turnId = doc["turn_id"] | 0;
  info.hadAudio = doc["had_audio"] | false;
  info.hadModelText = doc["had_model_text"] | false;
  info.hadToolActivity = doc["had_tool_activity"] | false;
  info.synthetic = doc["synthetic"] | false;
  info.reason = String(doc["reason"] | "");

  Serial.printf("[Server] Turn complete id=%lu audio=%d text=%d tool=%d synthetic=%d reason=%s\n",
                static_cast<unsigned long>(info.turnId), info.hadAudio,
                info.hadModelText, info.hadToolActivity, info.synthetic,
                info.reason.c_str());

  if (_callbacks.onTurnComplete) {
    _callbacks.onTurnComplete(info);
  }
  return;
}
```

### Step 5.4: Track tool activity in AppController

In `AppController.h`, add:

```cpp
bool _turnHasToolActivity = false;
```

Reset it anywhere turn flags are reset:

- `startRecording()`
- `requestConversationSuggestion()`
- `requestClarification()`
- menu/scene starts that send text prompts
- `onDropAudio`
- `onIgnoredAudio`
- completion cleanup
- timeout cleanup

Example:

```cpp
_turnComplete = false;
_turnHasAudio = false;
_turnHasModelText = false;
_turnHasToolActivity = false;
```

Set it true when a tool status arrives or device-side tool has a visible effect:

- `onShowText`
- `onBrightness`
- `onVolume`
- `onPlaySound`
- `onPlayMelody`
- new `onToolStatus`

Add callback in `LiveSessionCallbacks`:

```cpp
std::function<void(const String &, const String &, const String &)> onToolStatus;
```

Handle server JSON:

```cpp
if (strcmp(type, "tool_status") == 0) {
  const char *name = doc["name"];
  const char *status = doc["status"];
  const char *text = doc["text"];
  if (_callbacks.onToolStatus) {
    _callbacks.onToolStatus(String(name ? name : ""),
                            String(status ? status : ""),
                            String(text ? text : ""));
  }
  return;
}
```

In `AppController.cpp`:

```cpp
callbacks.onToolStatus = [this](const String &name, const String &status, const String &text) {
  _turnHasToolActivity = true;
  if (!text.isEmpty()) {
    _toolText = text;
    resetBodyPage();
  }
  _screenDirty = true;
};
```

### Step 5.5: Update `onTurnComplete`

Current:

```cpp
callbacks.onTurnComplete = [this]() {
  if (_turnHasAudio || _turnHasModelText) {
    _turnComplete = true;
    _screenDirty = true;
  }
};
```

Change to:

```cpp
callbacks.onTurnComplete = [this](const TurnCompleteInfo &info) {
  if (info.hadAudio) {
    _turnHasAudio = true;
  }
  if (info.hadModelText) {
    _turnHasModelText = true;
  }
  if (info.hadToolActivity) {
    _turnHasToolActivity = true;
  }

  if (_turnHasAudio || _turnHasModelText || _turnHasToolActivity || info.synthetic) {
    _turnComplete = true;
    _screenDirty = true;
  }
};
```

### Step 5.6: Let `processPlayback()` finish non-audio turns

Current text-only path:

```cpp
if (_appState == AppState::Thinking && _turnComplete && !_turnHasAudio &&
    _turnHasModelText) {
  // return Ready
}
```

Expand it:

```cpp
if (_appState == AppState::Thinking && _turnComplete && !_turnHasAudio &&
    (_turnHasModelText || _turnHasToolActivity)) {
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _audio.stopPlayback();
  _shakeSuggestionArmed = true;
  setAppState(AppState::Ready, "Ready");
  return;
}
```

### Verification

Test these user-visible tool-only cases:

1. “Set volume to 50.”
   - Device should perform action and return Ready.
   - It should not sit in Thinking until timeout.

2. “Save a flashcard that der Baum means the tree.”
   - Device should at least display “Saved flashcard” quickly.
   - If Gemini speaks a confirmation, it should play once.
   - If Gemini does not speak, the device should still return Ready.

3. “Show the text hello.”
   - Screen should update.
   - Turn should complete without requiring model audio.

---

## 9. Milestone 6 — Add cancellation from firmware to server

### Goal

When the device gives up on a turn or the user interrupts, the server/Gemini must also stop that turn. This prevents stale responses from surfacing during the next turn.

### Files

- `firmware/src/services/LiveSessionService.h`
- `firmware/src/services/LiveSessionService.cpp`
- `firmware/src/app/AppController.cpp`
- `server/src/live-session.ts`

### Step 6.1: Add firmware `sendCancelTurn()`

In `LiveSessionService.h`:

```cpp
bool sendCancelTurn(const String &reason);
```

In `LiveSessionService.cpp`:

```cpp
bool LiveSessionService::sendCancelTurn(const String &reason) {
  JsonDocument doc;
  doc["type"] = "cancel_turn";
  doc["reason"] = reason;

  String payload;
  serializeJson(doc, payload);
  return _ws.send(payload);
}
```

### Step 6.2: Send cancel on thinking timeout

Current `processThinkingTimeout()`:

```cpp
if (millis() - _thinkingStartMs > kThinkingTimeoutMs) {
  Serial.println("[Loop] Thinking timeout");
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _audio.stopPlayback();
  setAppState(AppState::Ready, "Ready");
}
```

Change to:

```cpp
if (millis() - _thinkingStartMs > kThinkingTimeoutMs) {
  Serial.println("[Loop] Thinking timeout");
  _live.sendCancelTurn("firmware_thinking_timeout");
  _turnComplete = false;
  _turnHasAudio = false;
  _turnHasModelText = false;
  _turnHasToolActivity = false;
  _audio.stopPlayback();
  setAppState(AppState::Ready, "Ready");
}
```

### Step 6.3: Send cancel when starting a new recording from Thinking/Playing

The firmware intentionally allows barge-in:

```cpp
if (_buttonA.consumePressed() &&
    (_appState == AppState::Ready || _appState == AppState::Playing ||
     _appState == AppState::Thinking)) {
  startRecording();
  return;
}
```

Before starting a new recording from `Thinking` or `Playing`, cancel the old turn.

Option A: do it in `handleChatButtons()` before `startRecording()`.

Option B: pass the previous state into `startRecording()`.

Example:

```cpp
const AppState previousState = _appState;
if (previousState == AppState::Thinking || previousState == AppState::Playing) {
  _live.sendCancelTurn(previousState == AppState::Thinking
                           ? "user_interrupted_thinking"
                           : "user_interrupted_playback");
}
startRecording();
```

Do the same in scene mode if it can start recording from Thinking/Playing.

### Step 6.4: Server handles `cancel_turn`

In `onDeviceMessage`:

```ts
if (msg.type === 'cancel_turn') {
  const reason = typeof msg.reason === 'string' ? msg.reason : 'device_cancel'
  this.cancelActiveTurn(reason)
  return
}
```

Implement `cancelActiveTurn()`:

```ts
private cancelActiveTurn(reason: string) {
  const turn = this.activeTurn
  if (turn && !this.isTurnTerminal(turn)) {
    turn.phase = 'canceled'
    turn.canceledAtMs = Date.now()
    turn.cancelReason = reason
    console.warn(`[Turn ${turn.id}] canceled: ${reason}`)
  }

  this.clearTurnWatchdogs()

  // Resolve/fail pending device tools so no promises hang.
  for (const [id, pending] of this.pendingDeviceCalls) {
    clearTimeout(pending.timeout)
    pending.resolve(`Canceled: ${reason}`)
    this.pendingDeviceCalls.delete(id)
  }

  // Tell firmware to drop any queued audio tail.
  this.sendToDevice({ type: 'drop_audio', reason })

  // Important: clear Gemini's pending generation/tool state.
  // The simplest reliable approach is to reconnect the Live session.
  this.reconnectGeminiSession().catch((err) => {
    console.error('[Gemini] Failed to reconnect after cancel:', err)
  })
}
```

Why reconnect? Because Gemini server messages do not currently carry your local turn id. If you merely mark a turn canceled but keep the same Gemini session, late model audio from the old turn can still arrive and you cannot always distinguish it from the new turn. Reconnecting is heavier, but it gives a clean cut.

### Step 6.5: Preserve audio while reconnecting after cancel

If the user starts recording immediately after canceling, the server may be reconnecting Gemini. Your pending Gemini input queue from Milestone 2 should handle this:

```text
cancel old turn
close Gemini
begin new turn
queue activityStart/audio chunks while new Gemini connects
setupComplete
flush activityStart/audio/activityEnd in order
```

If you kept the old `pendingAudioChunks` design, this is the point where you should upgrade to the typed event queue.

### Verification

1. Force a stuck Thinking condition.
   - Temporarily make the server delay `turn_complete`, or add a debug flag that drops the next Gemini response.
2. Wait for firmware timeout.
3. Logs should show:

```text
[Loop] Thinking timeout
[Device] cancel_turn firmware_thinking_timeout
[Turn 44] canceled: firmware_thinking_timeout
[Gemini] reconnecting after cancel
```

4. Speak a new turn.
5. You should get exactly one response, not the old delayed response plus the new one.

Also test barge-in:

- Start a long assistant response.
- Press A while it is speaking.
- Old audio should stop immediately.
- New recording should produce one new answer.

---

## 10. Milestone 7 — Add server watchdogs for missing model continuation

### Goal

If Gemini stops after a tool response or never produces output, recover predictably.

### Files

- `server/src/live-session.ts`

### Watchdogs to add

Use regular Workers timers while the Durable Object is active:

```ts
private firstModelWatchdog: ReturnType<typeof setTimeout> | null = null
private postToolWatchdog: ReturnType<typeof setTimeout> | null = null
private emptyTurnGraceTimer: ReturnType<typeof setTimeout> | null = null
```

Clear them whenever a turn completes or is canceled.

### Step 7.1: First model activity watchdog

After device `stop` / text turn end:

```ts
private startFirstModelWatchdog(turnId: number) {
  this.clearFirstModelWatchdog()
  this.firstModelWatchdog = setTimeout(() => {
    const turn = this.activeTurn
    if (!turn || turn.id !== turnId || this.isTurnTerminal(turn)) return
    if (turn.firstModelActivityAtMs || turn.pendingToolIds.size > 0) return

    console.warn(`[Turn ${turn.id}] no model activity after user stop`)
    this.sendToDevice({
      type: 'tool_status',
      name: 'turn_watchdog',
      status: 'error',
      text: 'No response; try again',
    })
    this.completeTurnSynthetic(turn, 'no_model_activity')
    this.reconnectGeminiSession().catch(() => {})
  }, 12000)
}
```

Choose a value longer than normal model latency but shorter than firmware timeout, or coordinate with firmware timeout. Current firmware timeout is 15s, so 10–12s is reasonable.

### Step 7.2: Post-tool continuation watchdog

After sending a `toolResponse`, the model should either continue with audio/text or complete the turn. If nothing happens, nudge once.

```ts
private startPostToolWatchdog(turnId: number) {
  this.clearPostToolWatchdog()
  this.postToolWatchdog = setTimeout(() => {
    const turn = this.activeTurn
    if (!turn || turn.id !== turnId || this.isTurnTerminal(turn)) return
    if (turn.firstModelActivityAtMs && turn.phase === 'streaming_model') return

    if (!turn.nudgeSent) {
      turn.nudgeSent = true
      console.warn(`[Turn ${turn.id}] no continuation after toolResponse; nudging Gemini`)
      this.geminiWs?.send(JSON.stringify({
        realtimeInput: {
          text: 'SYSTEM: Continue your response now using the tool result. If the tool completed the user request, briefly confirm it. Keep it concise.',
        },
      }))
      this.startPostToolWatchdog(turnId)
      return
    }

    console.warn(`[Turn ${turn.id}] no continuation after toolResponse+nudge; completing synthetic`)
    this.sendToDevice({
      type: 'tool_status',
      name: 'turn_watchdog',
      status: 'ok',
      text: turn.hadToolActivity ? 'Done' : 'No response; try again',
    })
    this.completeTurnSynthetic(turn, 'post_tool_no_continuation')
    this.reconnectGeminiSession().catch(() => {})
  }, turn.nudgeSent ? 8000 : 5000)
}
```

The nudge text is a pragmatic fallback. The primary fix should be manual activity boundaries and proper tool responses. The nudge only covers Live API edge cases.

### Step 7.3: Empty turnComplete grace period

Gemini may emit `turnComplete` with no audio/text around a tool call. Do not immediately commit/clear the exchange if there may be a tool call right after it.

When `sc.turnComplete` arrives and the turn has no audio/text/tool activity:

```ts
private handleEmptyTurnCompleteWithGrace(turn: ActiveTurn) {
  this.clearEmptyTurnGraceTimer()
  this.emptyTurnGraceTimer = setTimeout(() => {
    const current = this.activeTurn
    if (!current || current.id !== turn.id || this.isTurnTerminal(current)) return
    if (current.toolCallCount > 0 || current.hadToolActivity) return

    console.warn(`[Turn ${current.id}] empty turnComplete after grace`)
    this.completeTurnSynthetic(current, 'empty_turn_complete')
  }, 250)
}
```

If a `toolCall` arrives during the grace window, clear the grace timer and execute tools.

### Step 7.4: Synthetic completion helper

```ts
private completeTurnSynthetic(turn: ActiveTurn, reason: string) {
  if (this.isTurnTerminal(turn)) return
  turn.phase = 'completed'
  turn.completedAtMs = Date.now()
  turn.turnCompleteSentToDevice = true

  this.sendToDevice({
    type: 'turn_complete',
    turn_id: turn.id,
    had_audio: turn.modelAudioBytes > 0,
    had_model_text: turn.assistantText.trim().length > 0,
    had_tool_activity: turn.hadToolActivity,
    synthetic: true,
    reason,
  })

  this.commitTurnExchange(turn).catch((err) => {
    console.error(`[Turn ${turn.id}] failed to commit synthetic completion`, err)
  })

  this.clearTurnWatchdogs()
}
```

### Verification

Test with artificial delays:

1. Add a temporary debug branch in `save_flashcard` execution:

   ```ts
   if ((call.args as any).front === 'debug-no-continuation') {
     // send tool response but skip/observe Gemini continuation
   }
   ```

   Or temporarily drop model output after a tool response.

2. Ask the device to save that card.
3. Expected behavior:

```text
[Turn 52] sent toolResponse batch count=1
[Turn 52] no continuation after toolResponse; nudging Gemini
[Turn 52] no continuation after toolResponse+nudge; completing synthetic
```

4. Firmware should display “Done” or “Saved flashcard” and return Ready.
5. The next user turn should not receive stale audio from the failed turn.

---

## 11. Milestone 8 — Make DB commits turn-aware and deduplicated

### Goal

Conversation history should have one coherent record per completed user turn, not fragmented rows caused by preliminary tool `turnComplete` events.

### Files

- `server/src/live-session.ts`
- optionally `server/schema.sql` if you want better telemetry later

### Current problem

`commitExchange()` reads and clears global fields:

```ts
const user = this.currentUserText.trim()
const assistant = this.currentAssistantText.trim()
this.currentUserText = ''
this.currentAssistantText = ''
```

If Gemini emits a tool-related `turnComplete` before the final answer, the user text can be committed and cleared early. Later assistant text may be committed separately or associated with the next turn.

### Step 8.1: Add `commitTurnExchange(turn)`

Do not use globals for final commit.

```ts
private async commitTurnExchange(turn: ActiveTurn) {
  if (turn.committed) return
  turn.committed = true

  const user = turn.inputText.trim()
  const assistant = turn.assistantText.trim()

  if (!user && !assistant) return

  // Existing D1 logic, but using user/assistant from this turn.
}
```

### Step 8.2: Keep existing `commitExchange()` only for disconnect migration

During transition, `saveConversation()` can commit the active turn if it has uncommitted text:

```ts
private async saveConversation() {
  if (this.activeTurn && !this.activeTurn.committed) {
    await this.commitTurnExchange(this.activeTurn)
  }
}
```

Eventually remove or rename old `commitExchange()` to avoid accidental use.

### Step 8.3: Feedback should use the completed user text once

Current feedback runs after every `turnComplete`:

```ts
const userForFeedback = this.currentUserText.trim()
this.sendToDevice({ type: 'turn_complete' })
await this.commitExchange()
this.maybeSendTurnFeedback(userForFeedback, true)
```

Move this into final completion:

```ts
const userForFeedback = turn.inputText.trim()
await this.commitTurnExchange(turn)
this.maybeSendTurnFeedback(userForFeedback, true)
```

Do not run grammar feedback for:

- empty tool-only synthetic completions,
- canceled turns,
- preliminary empty turnComplete events.

### Verification

Use D1 queries after a short test session:

```bash
cd server
wrangler d1 execute m5-live-conversations --local --command \
  "SELECT id, created_at, user_text, assistant_text FROM message_log ORDER BY id DESC LIMIT 10;"
```

Expected:

- One row per physical user release or synthetic text turn.
- No duplicate assistant-only rows caused by the same model answer.
- Flashcard/tool turns either have a concise assistant confirmation or user text with empty assistant only if the turn truly had no spoken/text response.

Also inspect tool log:

```bash
wrangler d1 execute m5-live-conversations --local --command \
  "SELECT id, created_at, tool_name, status, duration_ms FROM tool_log ORDER BY id DESC LIMIT 10;"
```

Expected:

- Every tool call has `status='ok'` or `status='error'`.
- Device tool timeouts are logged as errors, not invisible wedges.

---

## 12. Milestone 9 — Add automated regression tests around the turn coordinator

### Goal

Have tests for the logic that caused this regression, even if full Gemini/ESP32 integration remains manual.

### Files

Recommended new files:

- `server/src/turn-coordinator.ts`
- `server/src/turn-coordinator.test.ts`

Update:

- `server/package.json`

### Step 9.1: Extract pure turn logic

Do not try to unit-test the whole Durable Object first. Extract the pure parts:

- begin turn,
- record audio bytes,
- stop turn,
- note model activity,
- note tool call,
- note tool response,
- complete,
- cancel,
- synthetic completion decision.

Example API:

```ts
export class TurnCoordinator {
  beginAudioTurn(): TurnSnapshot
  receiveAudio(bytes: number, absSum: number, samples: number): TurnSnapshot
  endUserTurn(): TurnSnapshot
  noteToolCall(ids: string[]): TurnSnapshot
  noteToolResponse(ids: string[]): TurnSnapshot
  noteModelAudio(bytes: number): TurnSnapshot
  noteModelText(text: string): TurnSnapshot
  complete(reason: string): TurnSnapshot
  cancel(reason: string): TurnSnapshot
  snapshot(): TurnSnapshot
}
```

`LiveSession` can still own WebSockets and tool execution; it delegates state decisions to this class.

### Step 9.2: Add Node test script

In `server/package.json`:

```json
{
  "scripts": {
    "dev": "wrangler dev",
    "deploy": "wrangler deploy",
    "typecheck": "tsc --noEmit",
    "test": "node --test --import tsx src/*.test.ts"
  },
  "devDependencies": {
    "tsx": "^4.0.0"
  }
}
```

If you do not want to add `tsx`, compile tests with TypeScript first or write tests in plain JS against compiled output.

### Step 9.3: Test cases

Minimum tests:

1. **No tool while recording**

   ```text
   beginAudioTurn
   receiveAudio
   noteToolCall before endUserTurn
   expect warning/error flag or forced cancel
   ```

   If your design only logs this as impossible, test that the coordinator records a protocol violation.

2. **Tool-only completion counts**

   ```text
   beginAudioTurn
   endUserTurn
   noteToolCall save_flashcard
   noteToolResponse
   complete
   expect hadToolActivity=true and terminal completed
   ```

3. **Post-tool no continuation becomes synthetic completion**

   ```text
   beginAudioTurn
   endUserTurn
   noteToolCall
   noteToolResponse
   watchdog fires twice
   expect synthetic completion and no pending tool ids
   ```

4. **Firmware timeout cancels stale turn**

   ```text
   beginAudioTurn
   endUserTurn
   cancel firmware_thinking_timeout
   later noteModelAudio
   expect audio rejected/dropped because turn is canceled
   ```

5. **One commit per turn**

   ```text
   append input transcript
   preliminary empty turnComplete
   toolCall
   toolResponse
   append output transcript
   final turnComplete
   expect committed once
   ```

### Verification

Run:

```bash
cd server
npm run typecheck
npm test
```

A junior developer should be able to break the coordinator intentionally and see a meaningful failing test.

---

## 13. Milestone 10 — End-to-end manual validation matrix

Run these after all code changes.

### 10.1 Basic no-tool voice chat

Prompt:

> “What is two plus two?”

Expected:

- Device: Recording -> Thinking -> Playing -> Ready.
- Logs: one turn, no tools, one completion.
- No timeout.

### 10.2 Long utterance with pause while holding A

Prompt while holding A the whole time:

> “I want to save a flashcard ... [pause 2s] ... but also explain why the prefix works.”

Expected:

- No `toolCall while recording` warning.
- Tool call happens only after stop/activityEnd.
- One response, not two.

### 10.3 Server-side tool: save_flashcard

Prompt:

> “Save a flashcard: der Baum means the tree.”

Expected:

- `tool_status` appears on device quickly, or Gemini speaks confirmation.
- Device returns Ready without needing a second turn.
- `tool_log` has one `save_flashcard` row.

### 10.4 Device-side tool: volume/brightness

Prompt:

> “Set the volume to 80.”

Expected:

- Volume changes.
- Tool response sent to Gemini.
- Device returns Ready even if no audio confirmation.

### 10.5 Search tool followed by spoken answer

Prompt:

> “Find a German lesson about cafés and start it.”

Expected:

- `search_learning_resources` may call, then `load_learning_resource` may call.
- Tool responses are batched per Gemini toolCall message.
- Gemini speaks the next instruction once.
- No need to prompt again.

### 10.6 Interrupt during playback

Prompt:

> Ask for a long explanation, then press A while it is speaking.

Expected:

- Firmware stops old playback immediately.
- Server receives cancel/new start.
- No tail of old audio plays after the new turn.
- New answer corresponds to new user input.

### 10.7 Thinking timeout

Force a stuck turn using a temporary debug flag.

Expected:

- Firmware sends `cancel_turn`.
- Server cancels/reconnects Gemini.
- Next turn does not receive old delayed response.

### 10.8 Soak test

Have a 10-minute conversation with mixed actions:

- normal questions,
- flashcard saves,
- display text,
- lesson search/load,
- interrupt once or twice.

Expected:

- No “liminal Thinking” requiring a second turn.
- No paired/double responses.
- No tool call before stop.
- D1 message rows look coherent.

---

## 14. Implementation order recommendation

Do not implement everything at once. Use this order:

1. **Milestone 1 logging** — prove the current failure.
2. **Milestone 2 manual activity boundaries** — biggest root-cause fix.
3. **Milestone 5 firmware tool-only completion** — prevents tool-only Thinking limbo.
4. **Milestone 6 cancellation** — prevents late stale responses after timeout/interruption.
5. **Milestone 4 centralized tool handling** — prevents future wedges and dropped responses.
6. **Milestone 7 watchdogs** — robust recovery from Gemini/tool edge cases.
7. **Milestone 8 DB dedupe** — clean up persistence once live behavior is stable.
8. **Milestone 9 tests** — lock in the behavior.

You can stop and manually validate after each milestone.

---

## 15. Common pitfalls

### Pitfall: Just increasing `kThinkingTimeoutMs`

This hides the symptom but makes stale-response overlap worse. If Gemini eventually responds after 25 seconds and the user already started another turn, you still get double responses.

### Pitfall: Treating every `turnComplete` as final

With tools, a `turnComplete` can be empty or preliminary. Only complete the firmware-visible turn when the turn has visible output, tool activity, or a controlled fallback.

### Pitfall: Sending tool responses from scattered branches

Scattered sends make it hard to guarantee “exactly one response per call”. Centralize.

### Pitfall: Dropping device tool responses when Gemini is not ready

If the Gemini session is gone, the old tool call id is invalid. Do not silently drop; cancel/fail the turn and reconnect.

### Pitfall: Reconnecting Gemini without preserving input order

If the user is already recording during reconnect, queued input must flush as:

```text
activityStart -> audio chunks -> activityEnd
```

not just audio chunks.

### Pitfall: Forgetting scene/menu text turns

`sendText()` prompts from menus, shake hints, clarification, scene prompts, and quiz intro also need turn lifecycle handling. They can get stuck too.

---

## 16. Final healthy log example

For a flashcard save, a good log should look like:

```text
[Turn 87] begin audio
[Turn 87] sent activityStart
[Turn 87] audio #1 3200 bytes
[Turn 87] audio #2 3200 bytes
[Turn 87] device stop bytes=38400 avgAbs=...
[Turn 87] sent activityEnd
[Turn 87] phase waiting_for_model
[Turn 87] inputTranscript "Save a flashcard: der Baum means the tree."
[Turn 87] gemini toolCall save_flashcard
[Turn 87] phase executing_tools pending=1
[Turn 87] tool save_flashcard ok 34ms
[Turn 87] sent toolResponse batch count=1
[Turn 87] tool_status save_flashcard ok
[Turn 87] phase waiting_after_tools
[Turn 87] outputTranscript "Saved: der Baum → the tree."
[Turn 87] model audio bytes=4096
[Turn 87] phase streaming_model
[Turn 87] gemini turnComplete audio=true text=true tool=true
[Turn 87] sent device turn_complete
[Turn 87] committed exchange
[Turn 87] completed
```

For a tool-only completion:

```text
[Turn 88] begin audio
[Turn 88] sent activityStart
[Turn 88] device stop bytes=22400
[Turn 88] sent activityEnd
[Turn 88] gemini toolCall set_volume
[Turn 88] sent tool_call to device set_volume
[Turn 88] device tool_response set_volume ok
[Turn 88] sent toolResponse batch count=1
[Turn 88] gemini turnComplete audio=false text=false tool=true
[Turn 88] sent device turn_complete had_tool_activity=true
[Turn 88] completed
```

Firmware should then return to Ready because `_turnHasToolActivity` is true.

---

## 17. Definition of done

The change is done when all of these are true:

- [ ] No Gemini tool/model activity occurs before device stop during push-to-talk.
- [ ] Asking for a flashcard save never requires a second user turn to get unstuck.
- [ ] Tool-only turns return the device to Ready.
- [ ] Firmware timeout sends `cancel_turn` to server.
- [ ] Starting a new recording while Thinking/Playing cancels the old turn server-side.
- [ ] Late output from a canceled turn is not played during the next turn.
- [ ] Every tool call has exactly one success/error `toolResponse` and one `tool_log` row.
- [ ] D1 `message_log` has coherent one-turn rows, not fragmented duplicates.
- [ ] Manual validation matrix passes.
- [ ] Server typecheck and turn-coordinator tests pass.
