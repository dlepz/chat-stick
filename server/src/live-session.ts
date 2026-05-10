import {
	type ToolLogEntry,
	insertToolLog,
	saveConversationExchange,
} from './conversation-store'
import { searchDocsKeyword, searchDocsVector } from './docs-search'
import { type EmailEnv, emailEnabled, sendEmail } from './email'
import { handleFileTool } from './file-tools'
import { USER_INSTRUCTIONS_PATH, ensureUserInstructionsFile } from './files'
import { buildGeminiTools } from './gemini-tools'
import { generateAndProcessImage } from './image-gen'
import {
	AVAILABLE_VOICES,
	DEFAULT_VOICE,
	buildSystemInstructionText,
	findVoice,
	resolveVoice,
} from './prompt-builder'

interface Env extends EmailEnv {
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	HISTORY_API_TOKEN?: string
	STORAGE?: R2Bucket
}

interface DebugAudioMetadata {
	device_id: string
	chat_id: string
	saved_at: string
	reason: string
	pcm_bytes: number
	wav_bytes: number
	samples: number
	avg_abs: number
	chunks: number
	duration_ms: number | null
	dropped_debug_bytes: number
	sample_rate_hz: number
	channels: number
	bits_per_sample: number
}

interface GeminiMessage {
	setupComplete?: Record<string, unknown>
	serverContent?: {
		modelTurn?: {
			parts?: Array<{
				inlineData?: { mimeType: string; data: string }
				text?: string
			}>
		}
		turnComplete?: boolean
		interrupted?: boolean
		inputTranscription?: { text: string }
		outputTranscription?: { text: string }
	}
	toolCall?: {
		functionCalls: Array<{
			name: string
			id: string
			args: Record<string, unknown>
		}>
	}
}

interface WebFetchArgs {
	url?: string
	max_chars?: number
}

const DEFAULT_POWER_TIMEOUTS = {
	dim_ms: 60_000,
	screen_off_ms: 120_000,
	light_sleep_ms: 300_000,
	power_off_ms: 600_000,
} as const

const DEBUG_AUDIO_SAMPLE_RATE_HZ = 16_000
const DEBUG_AUDIO_CHANNELS = 1
const DEBUG_AUDIO_BITS_PER_SAMPLE = 16
const DEBUG_AUDIO_STORAGE_WAV_KEY = 'debug-audio/latest.wav'
const DEBUG_AUDIO_STORAGE_META_KEY = 'debug-audio/latest.json'

export class LiveSession {
	private static readonly MIN_RECONNECT_MS = 1500
	private static readonly IDLE_CLOSE_MS = 120_000
	private static readonly MAX_PENDING_AUDIO_BYTES = 1_100_000
	private static readonly MAX_DEBUG_AUDIO_BYTES = 2_000_000
	private state: DurableObjectState
	private env: Env
	private deviceWs: WebSocket | null = null
	private geminiWs: WebSocket | null = null
	private geminiConnecting = false
	private geminiReady = false
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
			return this.handleDebugAudioRequest(request)
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

	private async handleDebugAudioRequest(request: Request): Promise<Response> {
		const url = new URL(request.url)
		const match = url.pathname.match(/^\/debug\/audio\/([^/]+)\/latest\.(wav|json)$/)
		const requestedDeviceId = match ? decodeURIComponent(match[1]) : ''
		if (!isAuthorizedDebugAudioRequest(request, this.env)) {
			return new Response('Unauthorized', {
				status: 401,
				headers: debugCorsHeaders(),
			})
		}

		if (url.pathname.endsWith('.json')) {
			const metadata = await this.state.storage.get<DebugAudioMetadata>(
				DEBUG_AUDIO_STORAGE_META_KEY
			)
			if (!metadata) {
				return new Response('No debug audio captured yet', {
					status: 404,
					headers: debugCorsHeaders(),
				})
			}
			return new Response(JSON.stringify(metadata), {
				headers: {
					...debugCorsHeaders(),
					'Content-Type': 'application/json',
				},
			})
		}

		const wav = await this.state.storage.get<ArrayBuffer>(DEBUG_AUDIO_STORAGE_WAV_KEY)
		if (!wav) {
			return new Response('No debug audio captured yet', {
				status: 404,
				headers: debugCorsHeaders(),
			})
		}

		const filename = `${requestedDeviceId || 'device'}-latest.wav`
		return new Response(wav, {
			headers: {
				...debugCorsHeaders(),
				'Content-Type': 'audio/wav',
				'Content-Length': String(wav.byteLength),
				'Content-Disposition': `attachment; filename="${filename}"`,
				'Cache-Control': 'no-store',
			},
		})
	}

	private async connectGemini(sessionGeneration = this.sessionGeneration) {
		if (this.geminiConnecting) {
			return
		}
		if (this.geminiWs) {
			return
		}

		this.geminiConnecting = true
		try {
			const url =
				'https://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent' +
				`?key=${this.env.GEMINI_API_KEY}`
			const userInstructions = await this.getUserInstructionsForPrompt()

			const resp = await fetch(url, {
				headers: { Upgrade: 'websocket' },
			})

			const ws = resp.webSocket
			if (!ws) {
				console.error('[Gemini] WebSocket upgrade failed')
				this.sendToDevice({
					type: 'error',
					category: 'gemini_unavailable',
					message: 'Failed to connect to AI',
				})
				return
			}
			if (sessionGeneration !== this.sessionGeneration) {
				try {
					ws.close()
				} catch {
					// ignore
				}
				return
			}

			ws.accept()
			this.geminiWs = ws

			ws.addEventListener('message', (event) => {
				if (sessionGeneration !== this.sessionGeneration) return
				const raw = event.data
				const text =
					typeof raw === 'string'
						? raw
						: new TextDecoder().decode(raw as ArrayBuffer)
				this.onGeminiMessage(text).catch((err) => {
					console.error('[Gemini] Message handler error:', err)
				})
			})

			ws.addEventListener('close', (event) => {
				if (sessionGeneration !== this.sessionGeneration) return
				console.log(`[Gemini] Disconnected: code=${event.code} reason="${event.reason}" wasReady=${this.geminiReady}`)
				this.geminiWs = null
				this.geminiReady = false
				// Don't send error if we haven't set up yet — connectGemini will retry
			})

			ws.addEventListener('error', (event) => {
				if (sessionGeneration !== this.sessionGeneration) return
				console.error('[Gemini] WebSocket error:', event)
			})

			const voice = findVoice(this.currentVoice) ?? findVoice(DEFAULT_VOICE)!
			const systemInstructionText = buildSystemInstructionText({
				voice,
				locationContext: this.locationContext,
				userInstructions,
				canEmail: emailEnabled(this.env),
			})

			// Send session setup
			ws.send(
				JSON.stringify({
					setup: {
						model: 'models/gemini-3.1-flash-live-preview',
						generationConfig: {
							responseModalities: ['AUDIO'],
							speechConfig: {
								voiceConfig: {
									prebuiltVoiceConfig: { voiceName: voice.name },
								},
							},
						},
						outputAudioTranscription: {},
						inputAudioTranscription: {},
						systemInstruction: {
							parts: [
								{
									text: systemInstructionText,
								},
							],
						},
						tools: buildGeminiTools({ canEmail: emailEnabled(this.env) }),
					},
				})
			)

			console.log('[Gemini] Setup message sent')
		} catch (err) {
			console.error('[Gemini] Connection error:', err)
			this.sendToDevice({
				type: 'error',
				category: 'gemini_unavailable',
				message: `AI connection failed: ${err}`,
			})
		} finally {
			this.geminiConnecting = false
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
				const destination = this.geminiWs && this.geminiReady ? 'Gemini' : 'buffer'
				console.log(
					`[Bridge] Audio chunk #${this.audioChunkCount}: ${chunk.byteLength} bytes → ${destination} (samples: ${firstSamples})`
				)
			}

			if (!this.geminiWs || !this.geminiReady) {
				this.bufferAudioChunk(chunk)
				if (!this.geminiWs) {
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
					if (!this.geminiWs) {
						console.log('[Bridge] Start received while Gemini closed — reconnecting')
						this.connectGemini()
					}
				}

				// Forward tool response to Gemini
				if (msg.type === 'tool_response' && this.geminiWs && this.geminiReady) {
					console.log(`[Bridge] Tool response: ${msg.name} → Gemini`)
					this.geminiWs.send(
						JSON.stringify({
							toolResponse: {
								functionResponses: [
									{
										name: msg.name,
										id: msg.id,
										response: { result: msg.result },
									},
								],
							},
						})
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
				if (msg.type === 'text' && msg.content && this.geminiWs && this.geminiReady) {
					this.geminiWs.send(
						JSON.stringify({
							realtimeInput: { text: msg.content },
						})
					)
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

	private appendTranscriptDelta(current: string, incoming: string) {
		if (!incoming) {
			return { text: current, delta: '' }
		}
		if (!current) {
			return { text: incoming, delta: incoming }
		}

		// Live transcription messages can arrive as either true deltas or as
		// cumulative/overlapping partial hypotheses. The device reveal animation
		// expects append-only deltas, so strip any already-emitted prefix here.
		if (incoming === current || current.startsWith(incoming)) {
			return { text: current, delta: '' }
		}
		if (incoming.startsWith(current)) {
			const delta = incoming.slice(current.length)
			return { text: incoming, delta }
		}
		if (current.endsWith(incoming)) {
			return { text: current, delta: '' }
		}

		const maxOverlap = Math.min(current.length, incoming.length)
		for (let len = maxOverlap; len >= 1; len--) {
			if (current.endsWith(incoming.slice(0, len))) {
				const delta = incoming.slice(len)
				return { text: current + delta, delta }
			}
		}

		return { text: current + incoming, delta: incoming }
	}

	private async handleGeminiMessage(msg: GeminiMessage) {

		// Session ready
		if (msg.setupComplete) {
			console.log('[Gemini] Setup complete')
			this.geminiReady = true
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
				const next = this.appendTranscriptDelta(
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
				const next = this.appendTranscriptDelta(
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
				console.log(`[Gemini] Tool call: ${call.name}(${JSON.stringify(call.args)})`)
				const startMs = Date.now()

				if (call.name === 'search_docs') {
					const query = (call.args as { query?: string }).query || ''
					console.log(`[Gemini] Docs search: "${query}"`)

					let results: { title: string; section: string; content: string; score: number }[] = []
					let searchMode: 'vector' | 'keyword' = 'vector'

					try {
						results = await searchDocsVector(query, this.env, 3)
					} catch (err) {
						console.warn('[Gemini] Vector search failed, falling back to keyword search:', err)
					}

					if (results.length === 0) {
						searchMode = 'keyword'
						results = searchDocsKeyword(query, 3)
					}

					console.log(
						`[Gemini] Found ${results.length} ${searchMode} results (top: ${results[0]?.title})`
					)
					const searchResults = results.map((r) => ({
						title: r.title,
						section: r.section,
						content: r.content.slice(0, 1000),
						score: r.score,
					}))
					const payload = JSON.stringify({
						toolResponse: {
							functionResponses: [
								{
									name: call.name,
									id: call.id,
									response: { results: searchResults },
								},
							],
						},
					})
					console.log(`[Gemini] Sending tool response: ${payload.length} bytes`)

					if (this.geminiWs) {
						this.geminiWs.send(payload)
						console.log(`[Gemini] Tool response sent`)
					}
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: {
							mode: searchMode,
							count: searchResults.length,
							titles: searchResults.map((r) => r.title),
						},
						handledBy: 'server',
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'web_fetch' || call.name === 'fetch_url') {
						const args = call.args as WebFetchArgs
						const url = args.url || ''
						console.log(`[Gemini] Fetching: ${url}`)
						const result = await fetchWebPage(url, args.max_chars)
						const payload = JSON.stringify({
							toolResponse: {
								functionResponses: [
									{
										name: call.name,
										id: call.id,
										response: result,
									},
								],
							},
						})
						if (this.geminiWs) {
							this.geminiWs.send(payload)
							console.log(`[Gemini] ${call.name} response sent (${result.content.length} chars)`)
						}
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: { url, chars: result.content.length },
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
				} else if (
						call.name === 'list_files' ||
						call.name === 'read_file' ||
						call.name === 'write_file' ||
						call.name === 'append_to_file' ||
						call.name === 'search_files'
					) {
						const response = await handleFileTool(
							this.env.DB,
							this.deviceId,
							call.name,
							call.args
						)
						const payload = JSON.stringify({
							toolResponse: {
								functionResponses: [
									{ name: call.name, id: call.id, response },
								],
							},
						})
						if (this.geminiWs) this.geminiWs.send(payload)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: response,
							handledBy: 'server',
							status: 'error' in (response as Record<string, unknown>) ? 'error' : 'ok',
							durationMs: Date.now() - startMs,
						})
				} else if (call.name === 'set_voice') {
						const requested = (call.args as { name?: string }).name || ''
						const match = findVoice(requested)
						if (!match) {
							const payload = JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												result: `Unknown voice "${requested}". Available: ${AVAILABLE_VOICES.map((v) => v.name).join(', ')}.`,
											},
										},
									],
								},
							})
							if (this.geminiWs) this.geminiWs.send(payload)
							await this.logToolCall({
								name: call.name,
								args: call.args,
								result: `unknown voice: ${requested}`,
								handledBy: 'server',
								status: 'error',
								durationMs: Date.now() - startMs,
							})
						} else {
							console.log(`[Gemini] Switching voice → ${match.name}`)
							this.currentVoice = match.name
							const payload = JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												result: `Voice set to ${match.name} (${match.description}). Reconnecting.`,
											},
										},
									],
								},
							})
							if (this.geminiWs) this.geminiWs.send(payload)
							this.sendToDevice({ type: 'voice_changed', voice: match.name })
							await this.logToolCall({
								name: call.name,
								args: call.args,
								result: match.name,
								handledBy: 'server',
								durationMs: Date.now() - startMs,
							})
							if (this.geminiWs) {
								try { this.geminiWs.close() } catch { /* ignore */ }
								this.geminiWs = null
								this.geminiReady = false
							}
							await this.connectGemini()
						}
				} else if (call.name === 'email_me') {
						const args = call.args as { subject?: string; body?: string }
						const result = await sendEmail(
							this.env,
							args.subject || '',
							args.body || ''
						)
						const responsePayload =
							'ok' in result
								? { result: `email sent to ${result.recipient}` }
								: { result: `email failed: ${result.error}` }
						const payload = JSON.stringify({
							toolResponse: {
								functionResponses: [
									{ name: call.name, id: call.id, response: responsePayload },
								],
							},
						})
						if (this.geminiWs) this.geminiWs.send(payload)
						await this.logToolCall({
							name: call.name,
							args: { subject: args.subject, body_chars: (args.body || '').length },
							result: 'ok' in result ? 'sent' : result.error,
							handledBy: 'server',
							status: 'ok' in result ? 'ok' : 'error',
							error: 'ok' in result ? undefined : result.error,
							durationMs: Date.now() - startMs,
						})
				} else if (call.name === 'show_image') {
						const args = call.args as { prompt?: string }
						const prompt = (args.prompt || '').trim()
						if (!prompt) {
							const payload = JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: { result: 'no prompt provided' },
										},
									],
								},
							})
							if (this.geminiWs) this.geminiWs.send(payload)
							await this.logToolCall({
								name: call.name,
								args: call.args,
								result: 'no prompt',
								handledBy: 'server',
								status: 'error',
								durationMs: Date.now() - startMs,
							})
						} else {
							// Tell Gemini the image is on its way so it can keep talking.
							const ackPayload = JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												result: 'image generation started; it will appear on screen shortly',
											},
										},
									],
								},
							})
							if (this.geminiWs) this.geminiWs.send(ackPayload)
							// Tell the device an image is coming so it can show the pulse animation.
							this.sendToDevice({ type: 'show_image_pending' })
							// Run the pipeline in the background and push the result when ready.
							this.generateAndSendImage(prompt, call.name, call.args, startMs).catch(
								(err) => {
									console.error('[ImageGen] Background generation failed:', err)
								}
							)
						}
				} else if (call.name === 'new_conversation' || call.name === 'new_chat') {
						// Handle server-side: close Gemini session and open a fresh one
						console.log('[Gemini] Resetting conversation')
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: 'conversation reset',
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
						await this.commitExchange()
						this.chatId = crypto.randomUUID()
						this.currentUserText = ''
						this.currentAssistantText = ''
						this.sendToDevice({
							type: 'session',
							chatId: this.chatId,
							reset: true,
						})
						if (this.geminiWs) {
							try { this.geminiWs.close() } catch { /* ignore */ }
							this.geminiWs = null
							this.geminiReady = false
						}
					await this.connectGemini()
				} else {
					// Forward tool call to device for execution — log when response arrives
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
			}
		}
	}

	private sendToDevice(msg: Record<string, unknown>) {
		this.deviceWs?.send(JSON.stringify(msg))
	}

	private async generateAndSendImage(
		prompt: string,
		toolName: string,
		toolArgs: unknown,
		startMs: number
	): Promise<void> {
		const turnChatId = this.chatId
		const result = await generateAndProcessImage(prompt, this.env.GEMINI_API_KEY)
		if (!result) {
			this.sendToDevice({ type: 'show_image_failed' })
			await this.logToolCall({
				name: toolName,
				args: toolArgs,
				result: 'generation failed',
				handledBy: 'server',
				status: 'error',
				durationMs: Date.now() - startMs,
			})
			return
		}

		// Persist the dithered PNG to R2 if STORAGE is bound. Best-effort; failure
		// here doesn't block sending to the device.
		let imageKey: string | undefined
		if (this.env.STORAGE) {
			const stamp = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`
			imageKey = `chat-stick/assets/${this.deviceId}/images/${turnChatId}-${stamp}.png`
			try {
				await this.env.STORAGE.put(imageKey, result.ditheredPng, {
					httpMetadata: { contentType: 'image/png' },
				})
				console.log(`[ImageGen] Stored dithered PNG at ${imageKey}`)
			} catch (err) {
				console.error('[ImageGen] R2 upload failed:', err)
				imageKey = undefined
			}
		}

		this.sendToDevice({
			type: 'show_image',
			data: result.data,
			width: result.width,
			height: result.height,
			...(imageKey ? { key: imageKey } : {}),
		})

		await this.logToolCall({
			name: toolName,
			args: toolArgs,
			result: { width: result.width, height: result.height, key: imageKey ?? null },
			handledBy: 'server',
			durationMs: Date.now() - startMs,
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
			LiveSession.MAX_DEBUG_AUDIO_BYTES
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
		if (this.currentTurnDebugAudioBytes <= 0 || this.currentTurnDebugAudioChunks.length === 0) {
			return
		}

		const chunks = this.currentTurnDebugAudioChunks.slice()
		const pcmBytes = this.currentTurnDebugAudioBytes
		const metadataBase: Omit<DebugAudioMetadata, 'wav_bytes'> = {
			device_id: this.deviceId,
			chat_id: this.chatId,
			saved_at: new Date().toISOString(),
			reason,
			pcm_bytes: pcmBytes,
			samples: this.currentTurnSamples,
			avg_abs: this.currentTurnAverageAbs(),
			chunks: this.audioChunkCount,
			duration_ms:
				this.currentTurnStartedAt > 0 ? Date.now() - this.currentTurnStartedAt : null,
			dropped_debug_bytes: this.currentTurnDebugDroppedBytes,
			sample_rate_hz: DEBUG_AUDIO_SAMPLE_RATE_HZ,
			channels: DEBUG_AUDIO_CHANNELS,
			bits_per_sample: DEBUG_AUDIO_BITS_PER_SAMPLE,
		}
		const wav = createPcm16Wav(
			chunks,
			pcmBytes,
			DEBUG_AUDIO_SAMPLE_RATE_HZ,
			DEBUG_AUDIO_CHANNELS,
			DEBUG_AUDIO_BITS_PER_SAMPLE
		)
		const metadata: DebugAudioMetadata = {
			...metadataBase,
			wav_bytes: wav.byteLength,
		}

		Promise.all([
			this.state.storage.put(DEBUG_AUDIO_STORAGE_WAV_KEY, wav),
			this.state.storage.put(DEBUG_AUDIO_STORAGE_META_KEY, metadata),
		])
			.then(() => {
				console.log(
					`[DebugAudio] Saved ${pcmBytes} PCM bytes (${wav.byteLength} WAV bytes), avg_abs=${metadata.avg_abs}, chunks=${metadata.chunks}`
				)
			})
			.catch((err) => {
				console.error('[DebugAudio] Failed to save recording:', err)
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
		if (!this.geminiWs || !this.geminiReady) return
		const base64 = arrayBufferToBase64(data)
		this.geminiWs.send(
			JSON.stringify({
				realtimeInput: {
					audio: {
						data: base64,
						mimeType: 'audio/pcm;rate=16000',
					},
				},
			})
		)
	}

	private flushPendingAudioIfReady() {
		if (!this.geminiWs || !this.geminiReady || this.pendingAudioChunks.length === 0) {
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
		if (!this.geminiWs || !this.geminiReady) {
			this.pendingStop = true
			if (!this.geminiWs) {
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
		if (this.geminiWs) {
			try {
				this.geminiWs.close()
			} catch {
				// ignore
			}
		}
		this.geminiWs = null
		this.geminiReady = false
		this.clearPendingAudio()
		await this.connectGemini()
	}

	private sendAudioStreamEnd() {
		if (!this.geminiWs) return
		this.geminiWs.send(
			JSON.stringify({
				realtimeInput: {
					audioStreamEnd: true,
				},
			})
		)
		console.log('[Bridge] Sent audio stream end')
		this.resetCurrentTurnMetrics()
		this.clearPendingAudio()
	}

	async alarm() {
		if (!this.geminiWs && !this.deviceWs) return
		const idle = Date.now() - this.lastActivityMs
		if (idle >= LiveSession.IDLE_CLOSE_MS) {
			if (this.geminiWs) {
				console.log(`[Session] Idle ${Math.floor(idle / 1000)}s — closing Gemini`)
				await this.commitExchange()
				try {
					this.geminiWs.close()
				} catch {
					// ignore
				}
				this.geminiWs = null
				this.geminiReady = false
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

		if (this.geminiWs) {
			try {
				this.geminiWs.close()
			} catch {
				// ignore
			}
			this.geminiWs = null
			this.geminiReady = false
		}
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

function debugCorsHeaders(): HeadersInit {
	return {
		'Access-Control-Allow-Origin': '*',
		'Access-Control-Allow-Methods': 'GET, OPTIONS',
		'Access-Control-Allow-Headers': 'X-History-Token',
	}
}

function isAuthorizedDebugAudioRequest(request: Request, env: Env): boolean {
	const url = new URL(request.url)
	if (url.hostname === 'localhost' || url.hostname === '127.0.0.1' || url.hostname === '[::1]') {
		return true
	}

	const configuredToken = env.HISTORY_API_TOKEN?.trim()
	if (!configuredToken) return false

	const providedToken =
		request.headers.get('X-History-Token') ?? url.searchParams.get('token') ?? ''
	return providedToken === configuredToken
}

function createPcm16Wav(
	chunks: ArrayBuffer[],
	pcmBytes: number,
	sampleRate: number,
	channels: number,
	bitsPerSample: number
): ArrayBuffer {
	const headerBytes = 44
	const wav = new ArrayBuffer(headerBytes + pcmBytes)
	const view = new DataView(wav)
	const bytes = new Uint8Array(wav)
	const blockAlign = (channels * bitsPerSample) / 8
	const byteRate = sampleRate * blockAlign

	writeAscii(bytes, 0, 'RIFF')
	view.setUint32(4, 36 + pcmBytes, true)
	writeAscii(bytes, 8, 'WAVE')
	writeAscii(bytes, 12, 'fmt ')
	view.setUint32(16, 16, true)
	view.setUint16(20, 1, true)
	view.setUint16(22, channels, true)
	view.setUint32(24, sampleRate, true)
	view.setUint32(28, byteRate, true)
	view.setUint16(32, blockAlign, true)
	view.setUint16(34, bitsPerSample, true)
	writeAscii(bytes, 36, 'data')
	view.setUint32(40, pcmBytes, true)

	let offset = headerBytes
	for (const chunk of chunks) {
		const source = new Uint8Array(chunk)
		const writable = Math.min(source.byteLength, bytes.byteLength - offset)
		if (writable <= 0) break
		bytes.set(source.subarray(0, writable), offset)
		offset += writable
	}

	return wav
}

function writeAscii(bytes: Uint8Array, offset: number, text: string) {
	for (let i = 0; i < text.length; i++) {
		bytes[offset + i] = text.charCodeAt(i)
	}
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

async function fetchWebPage(url: string, maxChars = 4000): Promise<{
	url: string
	status: number
	content_type: string
	title: string | null
	content: string
	truncated: boolean
}> {
	const normalizedMaxChars = Math.max(500, Math.min(maxChars || 4000, 10000))

	try {
		if (!/^https?:\/\//i.test(url)) {
			return {
				url,
				status: 0,
				content_type: 'error',
				title: null,
				content: 'Error: URL must start with http:// or https://',
				truncated: false,
			}
		}
		const controller = new AbortController()
		const timeout = setTimeout(() => controller.abort(), 10000)
		const resp = await fetch(url, {
			headers: {
				'User-Agent': 'm5-live-assistant/1.0',
				Accept: 'text/html,application/json,text/plain,*/*',
			},
			signal: controller.signal,
		})
		clearTimeout(timeout)
		const contentType = resp.headers.get('content-type') || ''
		const title = extractHtmlTitle(await resp.clone().text(), contentType)
		let body = await resp.text()

		if (contentType.includes('html')) {
			// Strip scripts/styles and tags; collapse whitespace
			body = body
				.replace(/<script[\s\S]*?<\/script>/gi, ' ')
				.replace(/<style[\s\S]*?<\/style>/gi, ' ')
				.replace(/<noscript[\s\S]*?<\/noscript>/gi, ' ')
				.replace(/<[^>]+>/g, ' ')
				.replace(/&nbsp;/g, ' ')
				.replace(/&amp;/g, '&')
				.replace(/&lt;/g, '<')
				.replace(/&gt;/g, '>')
				.replace(/&quot;/g, '"')
				.replace(/&#39;/g, "'")
				.replace(/\s+/g, ' ')
				.trim()
		} else {
			body = body.replace(/\s+/g, ' ').trim()
		}

		const truncated = body.length > normalizedMaxChars
		if (truncated) {
			body = body.slice(0, normalizedMaxChars) + '... [truncated]'
		}

		if (!resp.ok) {
			body = body || `Error: HTTP ${resp.status} ${resp.statusText}`
		}

		return {
			url: resp.url,
			status: resp.status,
			content_type: contentType || 'unknown',
			title,
			content: body || '(empty response)',
			truncated,
		}
	} catch (err) {
		return {
			url,
			status: 0,
			content_type: 'error',
			title: null,
			content: `Error fetching URL: ${err instanceof Error ? err.message : String(err)}`,
			truncated: false,
		}
	}
}

function extractHtmlTitle(body: string, contentType: string): string | null {
	if (!contentType.includes('html')) return null

	const match = body.match(/<title[^>]*>([\s\S]*?)<\/title>/i)
	if (!match) return null

	return match[1].replace(/\s+/g, ' ').trim() || null
}

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64)
	const bytes = new Uint8Array(binary.length)
	for (let i = 0; i < binary.length; i++) {
		bytes[i] = binary.charCodeAt(i)
	}
	return bytes.buffer
}
