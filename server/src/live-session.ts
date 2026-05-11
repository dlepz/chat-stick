import {
	type ToolLogEntry,
	insertToolLog,
	saveConversationExchange,
} from './conversation-store'
import {
	MAX_DEBUG_AUDIO_BYTES,
	handleDebugAudioRequest,
	saveDebugAudio,
} from './debug-audio-store'
import { emailEnabled } from './email'
import { USER_INSTRUCTIONS_PATH, ensureUserInstructionsFile } from './files'
import { GeminiClient } from './gemini-client'
import {
	type GeminiMessage,
	buildGeminiAudioStreamEndPayload,
	buildGeminiRealtimeAudioPayload,
	buildGeminiRealtimeTextPayload,
	buildGeminiSetupPayload,
} from './gemini-live'
import { buildToolResponsePayload } from './gemini-tools'
import {
	DEFAULT_VOICE,
	buildSystemInstructionText,
	findVoice,
	resolveVoice,
} from './prompt-builder'
import {
	type GeminiFunctionCall,
	type ToolRouterEnv,
	routeGeminiToolCall,
} from './tool-router'
import { appendTranscriptDelta } from './transcript-delta'

interface Env extends ToolRouterEnv {
	HISTORY_API_TOKEN?: string
}

const DEFAULT_POWER_TIMEOUTS = {
	dim_ms: 60_000,
	screen_off_ms: 120_000,
	light_sleep_ms: 300_000,
	power_off_ms: 600_000,
} as const

export class LiveSession {
	private static readonly MIN_RECONNECT_MS = 1500
	private static readonly IDLE_CLOSE_MS = 120_000
	private static readonly MAX_PENDING_AUDIO_BYTES = 1_100_000
	private state: DurableObjectState
	private env: Env
	private deviceWs: WebSocket | null = null
	private gemini = new GeminiClient()
	private deviceId = 'unknown'
	private chatId = ''
	private currentVoice = DEFAULT_VOICE
	private currentUserText = ''
	private currentAssistantText = ''
	private sessionGeneration = 0
	private lastConnectionAt = 0
	private lastActivityMs = 0
	private locationContext = ''
	private currentTurnAudioBytes = 0
	private currentTurnAbsSum = 0
	private currentTurnSamples = 0
	private currentTurnStartedAt = 0
	private currentTurnDebugAudioChunks: ArrayBuffer[] = []
	private currentTurnDebugAudioBytes = 0
	private currentTurnDebugDroppedBytes = 0
	private pendingAudioChunks: ArrayBuffer[] = []
	private pendingAudioBytes = 0
	private pendingStop = false
	// Pending device-side tool calls keyed by call id → { name, args, startMs }
	private pendingDeviceCalls = new Map<string, { name: string; args: unknown; startMs: number }>()

	constructor(state: DurableObjectState, env: Env) {
		this.state = state
		this.env = env
	}

	async fetch(request: Request): Promise<Response> {
		const url = new URL(request.url)
		if (url.pathname.match(/^\/debug\/audio\/[^/]+\/latest\.(wav|json)$/)) {
			return handleDebugAudioRequest(request, this.state.storage, this.env)
		}

		const now = Date.now()
		if (now - this.lastConnectionAt < LiveSession.MIN_RECONNECT_MS) {
			return new Response('Too many reconnects', { status: 429 })
		}
		this.lastConnectionAt = now

		const sessionGeneration = ++this.sessionGeneration
		await this.saveConversation()
		this.cleanup()

		// Extract device_id, chat_id, and voice from URL
		this.deviceId = url.searchParams.get('device_id') || 'unknown'
		this.chatId = url.searchParams.get('chat_id') || crypto.randomUUID()
		this.currentVoice = resolveVoice(url.searchParams.get('voice'))
		this.locationContext = buildLocationContext(request)

		console.log(`[Device] Connected: device=${this.deviceId} chat=${this.chatId}`)

		const pair = new WebSocketPair()
		const [client, server] = Object.values(pair)

		server.accept()
		this.deviceWs = server
		this.lastActivityMs = Date.now()
		await this.state.storage.setAlarm(Date.now() + LiveSession.IDLE_CLOSE_MS)

		// Send chat_id to device (in case it was server-generated)
		this.sendToDevice({ type: 'session', chatId: this.chatId })

		server.addEventListener('message', (event) => {
			if (sessionGeneration !== this.sessionGeneration) return
			this.onDeviceMessage(event.data)
		})

		server.addEventListener('close', async () => {
			if (sessionGeneration !== this.sessionGeneration) return
			console.log('[Device] Disconnected')
			await this.saveConversation()
			this.cleanup()
		})

		server.addEventListener('error', (event) => {
			if (sessionGeneration !== this.sessionGeneration) return
			console.error('[Device] WebSocket error:', event)
		})

		// Connect to Gemini Live API
		await this.connectGemini(sessionGeneration)

		return new Response(null, { status: 101, webSocket: client })
	}

	private async connectGemini(sessionGeneration = this.sessionGeneration) {
		if (this.gemini.isConnecting || this.gemini.isConnected) {
			return
		}

		try {
			const userInstructions = await this.getUserInstructionsForPrompt()
			const voice = findVoice(this.currentVoice) ?? findVoice(DEFAULT_VOICE)!
			const canEmail = emailEnabled(this.env)
			const systemInstructionText = buildSystemInstructionText({
				voice,
				locationContext: this.locationContext,
				userInstructions,
				canEmail,
			})
			const result = await this.gemini.connect({
				apiKey: this.env.GEMINI_API_KEY,
				setupPayload: buildGeminiSetupPayload({
					voiceName: voice.name,
					systemInstructionText,
					canEmail,
				}),
				isCurrent: () => sessionGeneration === this.sessionGeneration,
				onMessage: (text) => {
					if (sessionGeneration !== this.sessionGeneration) return
					this.onGeminiMessage(text).catch((err) => {
						console.error('[Gemini] Message handler error:', err)
					})
				},
				onClose: (event, wasReady) => {
					if (sessionGeneration !== this.sessionGeneration) return
					console.log(
						`[Gemini] Disconnected: code=${event.code} reason="${event.reason}" wasReady=${wasReady}`
					)
					// Don't send error if we haven't set up yet — connectGemini will retry
				},
				onError: (event) => {
					if (sessionGeneration !== this.sessionGeneration) return
					console.error('[Gemini] WebSocket error:', event)
				},
			})

			if (result === 'unavailable') {
				console.error('[Gemini] WebSocket upgrade failed')
				this.sendToDevice({
					type: 'error',
					category: 'gemini_unavailable',
					message: 'Failed to connect to AI',
				})
				return
			}

			if (result === 'connected') {
				console.log('[Gemini] Setup message sent')
			}
		} catch (err) {
			console.error('[Gemini] Connection error:', err)
			this.sendToDevice({
				type: 'error',
				category: 'gemini_unavailable',
				message: `AI connection failed: ${err}`,
			})
		}
	}

	private audioChunkCount = 0
	private static readonly MIN_TURN_BYTES = 6400

	private onDeviceMessage(data: string | ArrayBuffer) {
		this.lastActivityMs = Date.now()
		if (data instanceof ArrayBuffer) {
			// Binary frame = raw PCM audio from device mic
			const chunk = data.slice(0)
			this.noteAudioChunk(chunk)
			if (this.audioChunkCount <= 3 || this.audioChunkCount % 10 === 0) {
				const firstSamples = Array.from(new Int16Array(chunk).slice(0, 4))
				const destination = this.gemini.isReady ? 'Gemini' : 'buffer'
				console.log(
					`[Bridge] Audio chunk #${this.audioChunkCount}: ${chunk.byteLength} bytes → ${destination} (samples: ${firstSamples})`
				)
			}

			if (!this.gemini.isConnected || !this.gemini.isReady) {
				this.bufferAudioChunk(chunk)
				if (!this.gemini.isConnected) {
					console.warn('[Bridge] Audio buffered — Gemini WS is null, reconnecting...')
					this.connectGemini()
				}
				return
			}

			this.flushPendingAudioIfReady()
			this.forwardAudioChunk(chunk)
		} else {
			// Text frame = control message
			try {
				const msg = JSON.parse(data)
				console.log('[Device]', msg.type)

				if (msg.type === 'start') {
					this.resetCurrentTurnMetrics()
					this.currentTurnStartedAt = Date.now()
					this.clearPendingAudio()
					if (!this.gemini.isConnected) {
						console.log('[Bridge] Start received while Gemini closed — reconnecting')
						this.connectGemini()
					}
				}

				// Forward tool response to Gemini
				if (msg.type === 'tool_response' && this.gemini.isReady) {
					console.log(`[Bridge] Tool response: ${msg.name} → Gemini`)
					this.gemini.send(
						buildToolResponsePayload(msg.name, msg.id, { result: msg.result })
					)
					const pending = this.pendingDeviceCalls.get(msg.id)
					if (pending) {
						this.pendingDeviceCalls.delete(msg.id)
						this.logToolCall({
							name: pending.name,
							args: pending.args,
							result: msg.result,
							handledBy: 'device',
							durationMs: Date.now() - pending.startMs,
						}).catch(() => {})
					}
				}

				// Forward text input to Gemini
				if (msg.type === 'text' && msg.content && this.gemini.isReady) {
					this.gemini.send(buildGeminiRealtimeTextPayload(msg.content))
				}

				// Send trailing silence so Gemini's VAD detects end-of-speech
				if (msg.type === 'stop') {
					this.handleStopMessage()
				}
			} catch {
				console.warn('[Device] Unparseable message:', data)
			}
		}
	}

	private async onGeminiMessage(data: string) {
		this.lastActivityMs = Date.now()
		let msg: GeminiMessage
		try {
			msg = JSON.parse(data)
		} catch {
			console.warn('[Gemini] Unparseable message:', typeof data, data.substring(0, 200))
			return
		}

		try {
			await this.handleGeminiMessage(msg)
		} catch (err) {
			console.error('[Gemini] Error handling message:', err)
		}
	}

	private async handleGeminiMessage(msg: GeminiMessage) {

		// Session ready
		if (msg.setupComplete) {
			console.log('[Gemini] Setup complete')
			this.gemini.markReady()
			this.sendToDevice({ type: 'ready' })
			this.sendToDevice({
				type: 'settings',
				power: DEFAULT_POWER_TIMEOUTS,
			})
			this.flushPendingAudioIfReady()
			return
		}

		if (msg.serverContent) {
			const sc = msg.serverContent

			// Transcriptions drive the device captions. Send them before binary
			// audio so the ESP32 doesn't queue text behind a burst of PCM frames.
			if (sc.inputTranscription?.text) {
				const next = appendTranscriptDelta(
					this.currentUserText,
					sc.inputTranscription.text
				)
				this.currentUserText = next.text
				if (next.delta) {
					this.sendToDevice({
						type: 'transcript',
						source: 'user',
						text: next.delta,
					})
				}
			}
			if (sc.outputTranscription?.text) {
				const next = appendTranscriptDelta(
					this.currentAssistantText,
					sc.outputTranscription.text
				)
				this.currentAssistantText = next.text
				if (next.delta) {
					this.sendToDevice({
						type: 'transcript',
						source: 'model',
						text: next.delta,
					})
				}
			}

			// Model audio — decode base64 and forward as raw binary
			if (sc.modelTurn?.parts) {
				for (const part of sc.modelTurn.parts) {
					if (part.inlineData?.data) {
						const raw = base64ToArrayBuffer(part.inlineData.data)
						this.deviceWs?.send(raw)
					}
				}
			}

			// User interrupted — tell the device to drop any buffered model audio
			// from the turn Gemini was generating.
			if (sc.interrupted) {
				this.sendToDevice({ type: 'drop_audio' })
			}

			// Turn complete — save exchange to D1
			if (sc.turnComplete) {
				this.sendToDevice({ type: 'turn_complete' })
				await this.commitExchange()
			}
		}

		if (msg.toolCall) {
			for (const call of msg.toolCall.functionCalls) {
				await routeGeminiToolCall({
					call,
					env: this.env,
					deviceId: this.deviceId,
					chatId: this.chatId,
					sendToGemini: (payload) => this.sendToGemini(payload),
					sendToDevice: (deviceMsg) => this.sendToDevice(deviceMsg),
					logToolCall: (entry) => this.logToolCall(entry),
					applyVoiceChange: (voice) => this.applyVoiceChange(voice),
					startNewConversation: () => this.startNewConversation(),
					forwardDeviceToolCall: (deviceCall, startMs) =>
						this.forwardDeviceToolCall(deviceCall, startMs),
				})
			}
		}
	}

	private sendToGemini(payload: string): boolean {
		return this.gemini.send(payload)
	}

	private sendToDevice(msg: Record<string, unknown>) {
		this.deviceWs?.send(JSON.stringify(msg))
	}

	private async applyVoiceChange(voice: string) {
		console.log(`[Gemini] Switching voice: ${voice}`)
		this.currentVoice = voice
		this.sendToDevice({ type: 'voice_changed', voice })
		await this.replaceGeminiConnection()
	}

	private async startNewConversation() {
		await this.commitExchange()
		this.chatId = crypto.randomUUID()
		this.currentUserText = ''
		this.currentAssistantText = ''
		this.sendToDevice({
			type: 'session',
			chatId: this.chatId,
			reset: true,
		})
		await this.replaceGeminiConnection()
	}

	private forwardDeviceToolCall(call: GeminiFunctionCall, startMs: number) {
		this.pendingDeviceCalls.set(call.id, {
			name: call.name,
			args: call.args,
			startMs,
		})
		this.sendToDevice({
			type: 'tool_call',
			name: call.name,
			id: call.id,
			args: call.args,
		})
	}

	private async getUserInstructionsForPrompt(): Promise<string> {
		try {
			const file = await ensureUserInstructionsFile(this.env.DB, this.deviceId)
			console.log(
				`[Files] Loaded ${USER_INSTRUCTIONS_PATH}: ${file.content.length} bytes`
			)
			return file.content
		} catch (err) {
			console.error(`[Files] Failed to load ${USER_INSTRUCTIONS_PATH}:`, err)
			return ''
		}
	}

	private async logToolCall(entry: Omit<ToolLogEntry, 'deviceId' | 'chatId'>) {
		await insertToolLog(this.env.DB, {
			deviceId: this.deviceId,
			chatId: this.chatId,
			...entry,
		})
	}

	private async commitExchange() {
		const user = this.currentUserText.trim()
		const assistant = this.currentAssistantText.trim()
		this.currentUserText = ''
		this.currentAssistantText = ''

		await saveConversationExchange(this.env.DB, this.deviceId, this.chatId, user, assistant)
	}

	private async saveConversation() {
		// Final save on disconnect — commit any pending text
		await this.commitExchange()
	}

	private getIgnoredTurnReason(): 'too_short' | null {
		if (this.currentTurnAudioBytes < LiveSession.MIN_TURN_BYTES) {
			return 'too_short'
		}

		if (this.currentTurnSamples === 0) {
			return 'too_short'
		}

		return null
	}

	private currentTurnAverageAbs() {
		if (this.currentTurnSamples === 0) return 0
		return Math.round(this.currentTurnAbsSum / this.currentTurnSamples)
	}

	private noteAudioChunk(data: ArrayBuffer) {
		this.audioChunkCount++
		this.currentTurnAudioBytes += data.byteLength
		if (
			this.currentTurnDebugAudioBytes + data.byteLength <=
			MAX_DEBUG_AUDIO_BYTES
		) {
			this.currentTurnDebugAudioChunks.push(data.slice(0))
			this.currentTurnDebugAudioBytes += data.byteLength
		} else {
			this.currentTurnDebugDroppedBytes += data.byteLength
		}
		const view = new Int16Array(data)
		for (const sample of view) {
			this.currentTurnAbsSum += Math.abs(sample)
		}
		this.currentTurnSamples += view.length
	}

	private saveCurrentTurnDebugAudio(reason: string) {
		saveDebugAudio({
			storage: this.state.storage,
			deviceId: this.deviceId,
			chatId: this.chatId,
			reason,
			chunks: this.currentTurnDebugAudioChunks,
			pcmBytes: this.currentTurnDebugAudioBytes,
			samples: this.currentTurnSamples,
			avgAbs: this.currentTurnAverageAbs(),
			chunkCount: this.audioChunkCount,
			durationMs:
				this.currentTurnStartedAt > 0 ? Date.now() - this.currentTurnStartedAt : null,
			droppedDebugBytes: this.currentTurnDebugDroppedBytes,
		})
	}

	private bufferAudioChunk(data: ArrayBuffer) {
		this.pendingAudioChunks.push(data)
		this.pendingAudioBytes += data.byteLength

		while (
			this.pendingAudioBytes > LiveSession.MAX_PENDING_AUDIO_BYTES &&
			this.pendingAudioChunks.length > 0
		) {
			const dropped = this.pendingAudioChunks.shift()
			this.pendingAudioBytes -= dropped?.byteLength ?? 0
			console.warn('[Bridge] Pending audio buffer full; dropped oldest chunk')
		}
	}

	private clearPendingAudio() {
		this.pendingAudioChunks = []
		this.pendingAudioBytes = 0
		this.pendingStop = false
	}

	private forwardAudioChunk(data: ArrayBuffer) {
		if (!this.gemini.isReady) return
		const base64 = arrayBufferToBase64(data)
		this.gemini.send(buildGeminiRealtimeAudioPayload(base64))
	}

	private flushPendingAudioIfReady() {
		if (!this.gemini.isReady || this.pendingAudioChunks.length === 0) {
			return
		}

		const count = this.pendingAudioChunks.length
		const bytes = this.pendingAudioBytes
		for (const chunk of this.pendingAudioChunks) {
			this.forwardAudioChunk(chunk)
		}
		this.pendingAudioChunks = []
		this.pendingAudioBytes = 0
		console.log(`[Bridge] Flushed ${count} buffered audio chunk(s), ${bytes} bytes`)

		if (this.pendingStop) {
			this.pendingStop = false
			this.handleStopMessage()
		}
	}

	private handleStopMessage() {
		if (!this.gemini.isConnected || !this.gemini.isReady) {
			this.pendingStop = true
			if (!this.gemini.isConnected) {
				this.connectGemini()
			}
			console.log('[Bridge] Stop buffered until Gemini is ready')
			return
		}

		if (this.pendingAudioChunks.length > 0) {
			this.pendingStop = true
			this.flushPendingAudioIfReady()
			return
		}

		const ignoreReason = this.getIgnoredTurnReason()
		this.saveCurrentTurnDebugAudio(ignoreReason ? `ignored:${ignoreReason}` : 'stop')
		if (ignoreReason) {
			console.log(
				`[Bridge] Ignoring accidental clip (${ignoreReason}, bytes=${this.currentTurnAudioBytes}, avg_abs=${this.currentTurnAverageAbs()})`
			)
			this.currentUserText = ''
			this.currentAssistantText = ''
			this.clearPendingAudio()
			this.sendToDevice({
				type: 'ignore_audio',
				reason: ignoreReason,
				bytes: this.currentTurnAudioBytes,
				avg_abs: this.currentTurnAverageAbs(),
				chunks: this.audioChunkCount,
			})
			this.reconnectGeminiSession().catch((err) => {
				console.error('[Gemini] Failed to reset ignored turn:', err)
			})
			return
		}

		this.audioChunkCount = 0
		console.log(
			`[Bridge] Ending audio stream bytes=${this.currentTurnAudioBytes} avg_abs=${this.currentTurnAverageAbs()}`
		)
		this.sendAudioStreamEnd()
	}

	private resetCurrentTurnMetrics() {
		this.audioChunkCount = 0
		this.currentTurnAudioBytes = 0
		this.currentTurnAbsSum = 0
		this.currentTurnSamples = 0
		this.currentTurnStartedAt = 0
		this.currentTurnDebugAudioChunks = []
		this.currentTurnDebugAudioBytes = 0
		this.currentTurnDebugDroppedBytes = 0
	}

	private async reconnectGeminiSession() {
		this.resetCurrentTurnMetrics()
		this.clearPendingAudio()
		await this.replaceGeminiConnection()
	}

	private async replaceGeminiConnection() {
		this.gemini.close()
		await this.connectGemini()
	}

	private sendAudioStreamEnd() {
		if (!this.gemini.send(buildGeminiAudioStreamEndPayload())) return
		console.log('[Bridge] Sent audio stream end')
		this.resetCurrentTurnMetrics()
		this.clearPendingAudio()
	}

	async alarm() {
		if (!this.gemini.isConnected && !this.deviceWs) return
		const idle = Date.now() - this.lastActivityMs
		if (idle >= LiveSession.IDLE_CLOSE_MS) {
			if (this.gemini.isConnected) {
				console.log(`[Session] Idle ${Math.floor(idle / 1000)}s — closing Gemini`)
				await this.commitExchange()
				this.gemini.close()
			}
			return
		}
		const next = LiveSession.IDLE_CLOSE_MS - idle
		await this.state.storage.setAlarm(Date.now() + next)
	}

	private cleanup() {
		if (this.currentTurnDebugAudioBytes > 0) {
			this.saveCurrentTurnDebugAudio('cleanup')
		}

		// Orphaned device tool calls (device disconnected before responding)
		for (const [id, pending] of this.pendingDeviceCalls) {
			this.logToolCall({
				name: pending.name,
				args: pending.args,
				handledBy: 'device',
				status: 'error',
				error: 'device disconnected before responding',
				durationMs: Date.now() - pending.startMs,
			}).catch(() => {})
		}
		this.pendingDeviceCalls.clear()

		this.gemini.close()
		if (this.deviceWs) {
			try {
				this.deviceWs.close()
			} catch {
				// ignore
			}
			this.deviceWs = null
		}
		this.resetCurrentTurnMetrics()
		this.clearPendingAudio()
	}
}

function buildLocationContext(request: Request): string {
	const cf = (request as Request & { cf?: IncomingRequestCfProperties }).cf
	const city = cf?.city || request.headers.get('cf-ipcity') || ''
	const region = cf?.region || request.headers.get('cf-region') || ''
	const country = cf?.country || request.headers.get('cf-ipcountry') || ''

	return [city, region, country].filter(Boolean).join(', ')
}

// ─── Utilities ───

function arrayBufferToBase64(buffer: ArrayBuffer): string {
	const bytes = new Uint8Array(buffer)
	const CHUNK = 8192
	let binary = ''
	for (let i = 0; i < bytes.length; i += CHUNK) {
		const slice = bytes.subarray(i, i + CHUNK)
		binary += String.fromCharCode(...slice)
	}
	return btoa(binary)
}

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64)
	const bytes = new Uint8Array(binary.length)
	for (let i = 0; i < binary.length; i++) {
		bytes[i] = binary.charCodeAt(i)
	}
	return bytes.buffer
}
