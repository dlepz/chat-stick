import { searchDocsKeyword, searchDocsVector } from './docs-search'
import { loadLearningResourceContext, saveFlashcard, searchLearningResources, searchReaderPassages } from './flashcard-api'
import type { LessonContextBundle, LearningResourceSummary } from './learning-types'

interface Env {
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	FLASHCARD_APP_BASE_URL?: string
	FLASHCARD_APP_BRIDGE_TOKEN?: string
	CONVERSATION_END_REVIEW_ENABLED?: string
	TURN_FEEDBACK_ENABLED?: string
	GEMINI_LIVE_API_VERSION?: string
	GEMINI_ENABLE_AFFECTIVE_DIALOG?: string
}

interface ConversationMessage {
	role: 'user' | 'assistant'
	content: string
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

interface TurnFeedback {
	color: 'green' | 'yellow' | 'red' | 'gray'
	correction: string
	reason: string
}

interface PracticeReview {
	grade: string
	overall: string
	mistakes: Array<{
		learner_said: string
		corrected: string
		explanation: string
	}>
	next_practice: string
}

type VoiceMode = 'assistant' | 'quiz_masters'

// Female-presenting Gemini Live prebuilt voices. We pick one per device
// WebSocket session and reuse it for Gemini reconnects inside that session.
const FEMALE_SESSION_VOICES = ['Aoede', 'Kore', 'Leda', 'Zephyr'] as const

const DEFAULT_POWER_TIMEOUTS = {
	dim_ms: 60_000,
	screen_off_ms: 120_000,
	light_sleep_ms: 300_000,
	power_off_ms: 600_000,
} as const

export class LiveSession {
	private static readonly MIN_RECONNECT_MS = 100
	private static readonly IDLE_CLOSE_MS = 10 * 60_000
	private state: DurableObjectState
	private env: Env
	private deviceWs: WebSocket | null = null
	private geminiWs: WebSocket | null = null
	private geminiReady = false
	private geminiConnecting = false
	private deviceId = 'unknown'
	private chatId = ''
	private voiceMode: VoiceMode = 'assistant'
	private sessionVoiceName: (typeof FEMALE_SESSION_VOICES)[number] = 'Aoede'
	private currentUserText = ''
	private currentAssistantText = ''
	private sessionGeneration = 0
	private nextServerTurnId = 1
	private activeServerTurnId: number | null = null
	private deviceRecording = false
	private lastConnectionAt = 0
	private lastActivityMs = 0
	private locationContext = ''
	private recentConversationContext = ''
	private currentTurnAudioBytes = 0
	private currentTurnAbsSum = 0
	private currentTurnSamples = 0
	private currentTurnModelAudioBytes = 0
	private currentTurnHadToolActivity = false
	private pendingTextTurnUntilMs = 0
	private pendingAudioChunks: ArrayBuffer[] = []
	private pendingAudioBytes = 0
	private pendingStartAfterReady = false
	private pendingStopAfterReady = false
	private lastReactiveFaceEmotionAtMs = 0
	private lastTurnFeedbackText = ''
	private lastTurnFeedbackAtMs = 0
	private activeLearningContext: LessonContextBundle | null = null
	private lastLearningSearchResults: LearningResourceSummary[] = []
	private conversationEndReviewEnabled = false
	private turnFeedbackEnabled = false
	// Pending device-side tool calls keyed by call id → { name, args, startMs }
	private pendingDeviceCalls = new Map<string, { name: string; args: unknown; startMs: number }>()

	constructor(state: DurableObjectState, env: Env) {
		this.state = state
		this.env = env
	}

	private async pickSessionVoice(): Promise<(typeof FEMALE_SESSION_VOICES)[number]> {
		const lastVoice = await this.state.storage.get<string>('lastSessionVoiceName')
		const candidates = FEMALE_SESSION_VOICES.filter((voice) => voice !== lastVoice)
		const pool = candidates.length > 0 ? candidates : FEMALE_SESSION_VOICES
		const voice = pool[Math.floor(Math.random() * pool.length)] ?? 'Aoede'
		await this.state.storage.put('lastSessionVoiceName', voice)
		return voice
	}

	private async loadRecentConversationContext(): Promise<string> {
		if (!this.chatId) return ''
		try {
			const row = await this.env.DB.prepare('SELECT messages FROM conversations WHERE chat_id = ?')
				.bind(this.chatId)
				.first<{ messages: string | null }>()
			const messages = row?.messages ? JSON.parse(row.messages) as ConversationMessage[] : []
			const recent = messages.slice(-24)
			if (recent.length === 0) return ''
			return recent.map((message) => `${message.role === 'user' ? 'User' : 'Assistant'}: ${message.content}`).join('\n').slice(-6000)
		} catch (err) {
			console.warn('[DB] Failed to load recent conversation context:', err)
			return ''
		}
	}

	async fetch(request: Request): Promise<Response> {
		const url = new URL(request.url)
		if (url.pathname === '/device/face-control') {
			return this.handleFaceControl(request)
		}

		const now = Date.now()
		if (now - this.lastConnectionAt < LiveSession.MIN_RECONNECT_MS) {
			return new Response('Too many reconnects', { status: 429 })
		}
		this.lastConnectionAt = now

		const sessionGeneration = ++this.sessionGeneration
		await this.saveConversation()
		this.cleanup()

		// Extract device_id and chat_id from URL
		this.deviceId = url.searchParams.get('device_id') || 'unknown'
		this.chatId = url.searchParams.get('chat_id') || crypto.randomUUID()
		this.voiceMode = parseVoiceMode(url.searchParams.get('mode'))
		this.sessionVoiceName = await this.pickSessionVoice()
		this.locationContext = buildLocationContext(request)
		this.recentConversationContext = await this.loadRecentConversationContext()
		this.conversationEndReviewEnabled = parseBooleanFlag(this.env.CONVERSATION_END_REVIEW_ENABLED)
		this.turnFeedbackEnabled = parseBooleanFlag(this.env.TURN_FEEDBACK_ENABLED)
		this.activeLearningContext = (await this.state.storage.get<LessonContextBundle>('activeLearningContext')) ?? null
		this.lastLearningSearchResults = (await this.state.storage.get<LearningResourceSummary[]>('lastLearningSearchResults')) ?? []

		console.log(`[Device] Connected: device=${this.deviceId} chat=${this.chatId} mode=${this.voiceMode} voice=${this.sessionVoiceName} endReview=${this.conversationEndReviewEnabled} turnFeedback=${this.turnFeedbackEnabled}`)

		const pair = new WebSocketPair()
		const [client, server] = Object.values(pair)

		server.accept()
		this.deviceWs = server
		this.lastActivityMs = Date.now()
		await this.state.storage.setAlarm(Date.now() + LiveSession.IDLE_CLOSE_MS)

		// Send chat_id to device (in case it was server-generated)
		this.sendToDevice({ type: 'session', chatId: this.chatId, mode: this.voiceMode })

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
		if (this.geminiWs || this.geminiConnecting) return
		this.geminiConnecting = true
		this.recentConversationContext = await this.loadRecentConversationContext()
		const apiVersion = this.env.GEMINI_LIVE_API_VERSION === 'v1alpha' ? 'v1alpha' : 'v1beta'
		const affectiveDialogEnabled = apiVersion === 'v1alpha' && parseBooleanFlag(this.env.GEMINI_ENABLE_AFFECTIVE_DIALOG)
		const url =
			`https://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.${apiVersion}.GenerativeService.BidiGenerateContent` +
			`?key=${this.env.GEMINI_API_KEY}`
		console.log(`[Gemini] Connecting api=${apiVersion} affective=${affectiveDialogEnabled}`)

		try {
			const resp = await fetch(url, {
				headers: { Upgrade: 'websocket' },
			})

			const ws = resp.webSocket
			if (!ws) {
				this.geminiConnecting = false
				console.error('[Gemini] WebSocket upgrade failed')
				this.sendToDevice({
					type: 'error',
					category: 'gemini_unavailable',
					message: 'Failed to connect to AI',
				})
				return
			}
			if (sessionGeneration !== this.sessionGeneration) {
				this.geminiConnecting = false
				try {
					ws.close()
				} catch {
					// ignore
				}
				return
			}

			ws.accept()
			this.geminiWs = ws
			this.geminiConnecting = false

			ws.addEventListener('message', (event) => {
				if (sessionGeneration !== this.sessionGeneration || this.geminiWs !== ws) return
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
				if (sessionGeneration !== this.sessionGeneration || this.geminiWs !== ws) return
				console.log(`[Gemini] Disconnected: code=${event.code} reason="${event.reason}" wasReady=${this.geminiReady}`)
				this.geminiWs = null
				this.geminiReady = false
				this.geminiConnecting = false
				// Don't send error if we haven't set up yet — connectGemini will retry
			})

			ws.addEventListener('error', (event) => {
				if (sessionGeneration !== this.sessionGeneration || this.geminiWs !== ws) return
				console.error('[Gemini] WebSocket error:', event)
			})

			// Send session setup
			ws.send(
				JSON.stringify({
					setup: {
						model: 'models/gemini-3.1-flash-live-preview',
						...(affectiveDialogEnabled ? { enable_affective_dialog: true } : {}),
						generationConfig: {
							responseModalities: ['AUDIO'],
							// Pin the Live API TTS voice. Without this, Gemini can pick a
							// different default voice after any upstream reconnect/cancel.
							speechConfig: {
								voiceConfig: {
									prebuiltVoiceConfig: { voiceName: this.sessionVoiceName },
								},
							},
						},
						realtimeInputConfig: {
							automaticActivityDetection: { disabled: true },
							activityHandling: 'START_OF_ACTIVITY_INTERRUPTS',
							turnCoverage: 'TURN_INCLUDES_ONLY_ACTIVITY',
						},
						outputAudioTranscription: {},
						inputAudioTranscription: {},
						systemInstruction: {
							parts: [
								{
									text: buildSystemInstruction(this.voiceMode, this.locationContext, this.activeLearningContext, this.conversationEndReviewEnabled, this.recentConversationContext),
								},
							],
						},
						tools: [
							{ googleSearch: {} },
							{
								functionDeclarations: [
									{
										name: 'set_brightness',
										description: 'Set the display backlight brightness',
										parameters: {
											type: 'OBJECT',
											properties: {
												level: {
													type: 'INTEGER',
													description:
														'Brightness level from 0 (off) to 255 (maximum)',
												},
											},
											required: ['level'],
										},
									},
									{
										name: 'set_volume',
										description: 'Set the speaker volume',
										parameters: {
											type: 'OBJECT',
											properties: {
												level: {
													type: 'INTEGER',
													description:
														'Volume level from 0 (mute) to 255 (maximum)',
												},
											},
											required: ['level'],
										},
									},
										{
											name: 'show_text',
										description:
											'Display a short text message on the device screen. Max ~20 characters per line, 7 lines.',
										parameters: {
											type: 'OBJECT',
											properties: {
												text: {
													type: 'STRING',
													description: 'Text to display',
												},
											},
											required: ['text'],
										},
										},
										{
											name: 'play_sound',
											description:
												'Play a named sound effect on the device speaker.',
											parameters: {
												type: 'OBJECT',
												properties: {
													sound: {
														type: 'STRING',
														description:
															'One of: beep, success, error, alert, fanfare',
													},
												},
												required: ['sound'],
											},
										},
										{
											name: 'play_melody',
											description:
												'Play a short melody on the device speaker using note tokens like "C4:200 E4:200 G4:400". Use R for rests.',
											parameters: {
												type: 'OBJECT',
												properties: {
													notes: {
														type: 'STRING',
														description:
															'Space-separated note tokens with durations in milliseconds, for example "C4:200 E4:200 G4:400"',
													},
												},
												required: ['notes'],
											},
										},
										{
											name: 'power_off',
											description: 'Power the device off immediately.',
											parameters: {
												type: 'OBJECT',
												properties: {},
											},
										},
										{
											name: 'get_device_status',
										description:
											'Get current device status including battery level, volume, brightness, WiFi network, and uptime.',
										parameters: {
											type: 'OBJECT',
											properties: {},
										},
									},
									{
										name: 'search_docs',
										description:
											'Search the indexed knowledge base. Returns relevant documentation entries matching the query.',
										parameters: {
											type: 'OBJECT',
											properties: {
												query: {
													type: 'STRING',
													description:
														'Search query — keywords or a question',
												},
											},
											required: ['query'],
										},
									},
									{
										name: 'web_fetch',
										description:
											'Fetch a web page or API endpoint and return cleaned text content. Use this to look up current information, read articles, or access APIs.',
										parameters: {
											type: 'OBJECT',
											properties: {
												url: {
													type: 'STRING',
													description:
														'The full URL to fetch (must include https:// or http://)',
												},
												max_chars: {
													type: 'INTEGER',
													description:
														'Maximum characters of cleaned text to return, from 500 to 10000. Defaults to 4000.',
												},
											},
											required: ['url'],
										},
									},
									{
										name: 'search_learning_resources',
										description: 'Search the user\'s German learning resources: worksheet lessons, roleplays, dialogues, mock-test practice, and graded readers. Use this when the user wants to find or choose a lesson to practice.',
										parameters: {
											type: 'OBJECT',
											properties: {
												query: { type: 'STRING', description: 'Search query, e.g. cafe, doctor appointment, B1 Bildbeschreibung, party planning, available graded readers.' },
												limit: { type: 'INTEGER', description: 'Maximum results to return, default 5.' },
												source: { type: 'STRING', description: 'Optional filter: worksheet, roleplay, or graded_reader. Use graded_reader when the user asks to list or browse available readers/books/stories.' },
											},
											required: ['query'],
										},
									},
									{
										name: 'load_learning_resource',
										description: 'Load the full practice context for a selected learning resource so you can run the lesson, roleplay, dialogue practice, or reader discussion.',
										parameters: {
											type: 'OBJECT',
											properties: {
												resource_id: { type: 'STRING', description: 'Resource id returned by search_learning_resources, e.g. worksheet:cafe or graded_reader:abc123.' },
												max_chars: { type: 'INTEGER', description: 'Maximum prompt context characters to load, default 8000.' },
											},
											required: ['resource_id'],
										},
									},
									{
										name: 'search_reader_passages',
										description: 'Search inside the full passages/sentences of a loaded or selected graded reader. Use this when the user asks for the part where the reader says something, or asks to find a topic inside a long reader/book.',
										parameters: {
											type: 'OBJECT',
											properties: {
												resource_id: { type: 'STRING', description: 'A graded reader resource id, e.g. graded_reader:abc123. If omitted, use the currently loaded graded reader when available.' },
												query: { type: 'STRING', description: 'Terms to find inside the reader passages, e.g. arches, hotel, where it says xyz.' },
												limit: { type: 'INTEGER', description: 'Maximum matching passages to return, default 5.' },
											},
											required: ['query'],
										},
									},
									{
										name: 'load_reader_passage',
										description: 'Load a focused context window around a specific graded reader passage/sentence returned by search_reader_passages so you can read or discuss that part.',
										parameters: {
											type: 'OBJECT',
											properties: {
												resource_id: { type: 'STRING', description: 'A graded reader resource id, e.g. graded_reader:abc123.' },
												passage_index: { type: 'INTEGER', description: 'Zero-based passage index returned by search_reader_passages.' },
												sentence_index: { type: 'INTEGER', description: 'Optional zero-based sentence index returned by search_reader_passages; loads context around this sentence.' },
												max_chars: { type: 'INTEGER', description: 'Maximum context characters, default 8000.' },
											},
											required: ['resource_id', 'passage_index'],
										},
									},
									{
										name: 'get_current_learning_resource',
										description: 'Return the currently loaded lesson/resource context, if one is active.',
										parameters: { type: 'OBJECT', properties: {} },
									},
									{
										name: 'clear_learning_resource',
										description: 'Clear the currently loaded lesson/resource context and return to general assistant mode.',
										parameters: { type: 'OBJECT', properties: {} },
									},
									{
										name: 'save_flashcard',
										description:
											'Save a flashcard derived from the recent exchange. Use this when the learner asks to save a phrase, save a correction, or save the most useful item from the conversation. Pick the highest-value item: a useful target-language phrase the assistant just used, the learner\'s last sentence with its corrected form, or a Redemittel that was explained. Generate concise front/back text in the appropriate languages. After calling, briefly confirm what you saved.',
										parameters: {
											type: 'OBJECT',
											properties: {
												front: {
													type: 'STRING',
													description:
														'Front of the card — the prompt side. For a target-language phrase, put the target-language text. For a correction, put the learner\'s incorrect sentence. For Redemittel, put the English meaning.',
												},
												back: {
													type: 'STRING',
													description:
														'Back of the card — the answer side. The translation, the corrected sentence, or the target-language Redemittel.',
												},
												tags: {
													type: 'ARRAY',
													items: { type: 'STRING' },
													description:
														'Short tags describing the card, e.g. ["roleplay", "im-cafe", "polite-request"].',
												},
												source: {
													type: 'STRING',
													description:
														'Optional context label, e.g. "roleplay:im-cafe" or "correction".',
												},
												note: {
													type: 'STRING',
													description:
														'Optional short note giving usage context, e.g. "More polite café phrasing".',
												},
											},
											required: ['front', 'back'],
										},
									},
									{
										name: 'end_practice_review',
										description:
											'Finish the current German practice session and get a concise transcript-based feedback review with mistakes and a grade. Use when the learner says they are done, asks to end/review/get graded, or when an enabled practice task reaches a natural ending.',
										parameters: {
											type: 'OBJECT',
											properties: {},
										},
									},

									],
								},
							],
					},
				})
			)

			console.log('[Gemini] Setup message sent')
			} catch (err) {
			this.geminiConnecting = false
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
	private static readonly SILENCE_AVG_ABS_THRESHOLD = 150
	private static readonly MAX_PENDING_AUDIO_BYTES = 512_000

	private queueAudioChunk(data: ArrayBuffer) {
		if (this.pendingAudioBytes + data.byteLength > LiveSession.MAX_PENDING_AUDIO_BYTES) {
			console.warn('[Bridge] Pending audio buffer full; dropping oldest chunks')
			while (this.pendingAudioChunks.length && this.pendingAudioBytes + data.byteLength > LiveSession.MAX_PENDING_AUDIO_BYTES) {
				const dropped = this.pendingAudioChunks.shift()
				this.pendingAudioBytes -= dropped?.byteLength ?? 0
			}
		}
		this.pendingAudioChunks.push(data.slice(0))
		this.pendingAudioBytes += data.byteLength
	}

	private sendActivityStartToGemini() {
		if (!this.geminiWs || !this.geminiReady) return false
		this.geminiWs.send(
			JSON.stringify({
				realtimeInput: { activityStart: {} },
			})
		)
		console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Sent activityStart`)
		return true
	}

	private sendActivityEndToGemini() {
		if (!this.geminiWs || !this.geminiReady) return false
		this.geminiWs.send(
			JSON.stringify({
				realtimeInput: { activityEnd: {} },
			})
		)
		console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Sent activityEnd`)
		this.resetCurrentTurnMetrics()
		return true
	}

	private sendAudioChunkToGemini(data: ArrayBuffer) {
		if (!this.geminiWs || !this.geminiReady) return false
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
		return true
	}

	private handleAudioChunk(data: ArrayBuffer) {
		this.audioChunkCount++
		this.currentTurnAudioBytes += data.byteLength
		const view = new Int16Array(data)
		for (const sample of view) {
			this.currentTurnAbsSum += Math.abs(sample)
		}
		this.currentTurnSamples += view.length
		if (this.audioChunkCount <= 3 || this.audioChunkCount % 10 === 0) {
			const firstSamples = Array.from(view.slice(0, 4))
			console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Audio chunk #${this.audioChunkCount}: ${data.byteLength} bytes → Gemini (samples: ${firstSamples})`)
		}

		if (!this.geminiWs) {
			console.warn('[Bridge] Gemini closed; reconnecting and buffering audio')
			this.queueAudioChunk(data)
			this.connectGemini()
			return
		}
		if (!this.geminiReady) {
			console.warn('[Bridge] Gemini not ready yet; buffering audio')
			this.queueAudioChunk(data)
			return
		}

		this.sendAudioChunkToGemini(data)
	}

	private flushPendingAudio() {
		if (!this.geminiWs || !this.geminiReady) return
		if (this.pendingStartAfterReady) {
			this.pendingStartAfterReady = false
			this.sendActivityStartToGemini()
		}
		if (this.pendingAudioChunks.length > 0) {
			console.log(`[Bridge] Flushing ${this.pendingAudioChunks.length} buffered audio chunks (${this.pendingAudioBytes} bytes)`)
			const chunks = this.pendingAudioChunks
			this.pendingAudioChunks = []
			this.pendingAudioBytes = 0
			for (const chunk of chunks) {
				this.sendAudioChunkToGemini(chunk)
			}
		}
		if (this.pendingStopAfterReady) {
			this.pendingStopAfterReady = false
			this.sendActivityEndToGemini()
		}
	}

	private onDeviceMessage(data: string | ArrayBuffer) {
		this.lastActivityMs = Date.now()
		if (data instanceof ArrayBuffer) {
			// Binary frame = raw PCM audio from device mic. If Gemini was closed for
			// idle timeout, reconnect and buffer instead of losing the user's first
			// post-idle utterance.
			this.handleAudioChunk(data)
		} else {
			// Text frame = control message
			try {
				const msg = JSON.parse(data)
				console.log('[Device]', msg.type)

				if (msg.type === 'start') {
					this.activeServerTurnId = this.nextServerTurnId++
					this.deviceRecording = true
					this.currentTurnModelAudioBytes = 0
					this.currentTurnHadToolActivity = false
					this.resetCurrentTurnMetrics()
					console.log(`[Turn ${this.activeServerTurnId}] Device start`)
					if (!this.geminiWs || !this.geminiReady) {
						this.pendingStartAfterReady = true
						console.log(`[Turn ${this.activeServerTurnId}] Start received while Gemini not ready — deferring activityStart`)
						this.connectGemini()
					} else {
						this.sendActivityStartToGemini()
					}
				}

				if (msg.type === 'cancel_turn') {
					const reason = typeof msg.reason === 'string' ? msg.reason : 'device_cancel'
					this.cancelActiveTurn(reason).catch((err) => {
						console.error('[Turn] Failed to cancel active turn:', err)
					})
					return
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

				// Forward text/menu input to Gemini. Gemini Live rejects clientContent on
				// this endpoint/model, so use realtimeInput.text and preserve the pending
				// text turn if a stale/accidental short audio clip arrives immediately after.
				if (msg.type === 'text' && msg.content && this.geminiWs && this.geminiReady) {
					this.activeServerTurnId = this.nextServerTurnId++
					this.deviceRecording = false
					this.currentTurnModelAudioBytes = 0
					this.currentTurnHadToolActivity = false
					this.resetCurrentTurnMetrics()
					console.log(`[Turn ${this.activeServerTurnId}] Device text (${String(msg.content).length} chars)`)
					this.pendingTextTurnUntilMs = Date.now() + 10_000
					this.geminiWs.send(
						JSON.stringify({
							realtimeInput: { text: msg.content },
						})
					)
				}

				// End the explicit push-to-talk activity. Automatic Gemini VAD is disabled
				// for this session, so the physical button release is the authoritative
				// end-of-turn signal. If Gemini is reconnecting, defer activityEnd until
				// after activityStart and buffered audio have been flushed in order.
				if (msg.type === 'stop') {
					this.deviceRecording = false
					console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Device stop bytes=${this.currentTurnAudioBytes} samples=${this.currentTurnSamples}`)
					if (this.currentTurnAudioBytes === 0) {
						console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Ignoring empty stop frame`)
						return
					}
					const ignoreReason = this.getIgnoredTurnReason()
					if (ignoreReason) {
						console.log(`[Bridge] Ignoring accidental clip (${ignoreReason})`)
						this.currentUserText = ''
						this.currentAssistantText = ''
						this.currentTurnModelAudioBytes = 0
						this.currentTurnHadToolActivity = false
						this.pendingAudioChunks = []
						this.pendingAudioBytes = 0
						this.pendingStartAfterReady = false
						this.pendingStopAfterReady = false
						this.sendToDevice({ type: 'ignore_audio', reason: ignoreReason })
						this.resetCurrentTurnMetrics()
						if (Date.now() > this.pendingTextTurnUntilMs) {
							this.reconnectGeminiSession().catch((err) => {
								console.error('[Gemini] Failed to reset ignored turn:', err)
							})
						} else {
							console.log('[Bridge] Preserving pending text/menu turn after accidental clip')
						}
						return
					}

					this.audioChunkCount = 0
					if (!this.geminiWs || !this.geminiReady) {
						console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Stop received while Gemini reconnecting; deferring activityEnd`)
						this.pendingStopAfterReady = true
						this.connectGemini()
						return
					}
					if (this.activeLearningContext?.source === 'roleplay') {
						// Gemini Live can occasionally drift back into generic assistant English after
						// a user greeting. Inject a per-turn reminder before ending the user's audio
						// turn so the next model response remains in-character German.
						this.geminiWs.send(
							JSON.stringify({
								realtimeInput: {
									text:
										'SYSTEM ROLEPLAY REMINDER: A German roleplay is active. Stay in character and reply in German only. If the learner said hello/hallo or greeted you, answer in German in character (for example: Guten Tag! Was möchten Sie bestellen?). Never answer in English unless the learner explicitly asks for English.',
								},
							})
						)
					}
					this.sendActivityEndToGemini()
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
			console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Gemini setupComplete`)
			console.log('[Gemini] Setup complete')
			this.geminiReady = true
			this.sendToDevice({ type: 'ready' })
			this.sendToDevice({
				type: 'settings',
				power: DEFAULT_POWER_TIMEOUTS,
			})
			this.flushPendingAudio()
			return
		}

		if (msg.serverContent) {
			this.pendingTextTurnUntilMs = 0
			const sc = msg.serverContent
			if (this.deviceRecording && sc.modelTurn?.parts?.some((part) => part.inlineData?.data || part.text)) {
				console.warn(`[Turn ${this.activeServerTurnId ?? '-'}] BUG: Gemini model output while device still recording`)
			}
			if (this.deviceRecording && sc.outputTranscription?.text) {
				console.warn(`[Turn ${this.activeServerTurnId ?? '-'}] BUG: Gemini output transcript while device still recording`)
			}

			// Model audio — decode base64 and forward as raw binary
			if (sc.modelTurn?.parts) {
				for (const part of sc.modelTurn.parts) {
					if (part.inlineData?.data) {
						const raw = base64ToArrayBuffer(part.inlineData.data)
						this.currentTurnModelAudioBytes += raw.byteLength
						console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Gemini audio ${raw.byteLength} bytes`)
						this.deviceWs?.send(raw)
					}
				}
			}

			// User interrupted — tell the device to drop any buffered model audio
			// from the turn Gemini was generating.
			if (sc.interrupted) {
				this.sendToDevice({ type: 'drop_audio' })
			}

			// Transcriptions — accumulate for DB storage
			if (sc.inputTranscription?.text) {
				console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Gemini input transcript ${sc.inputTranscription.text.length} chars`)
				this.currentUserText += sc.inputTranscription.text
				this.sendToDevice({
					type: 'transcript',
					source: 'user',
					text: sc.inputTranscription.text,
				})
				// Cheap live face reaction only; grammar feedback remains after turnComplete
				// so it cannot compete with the live response path.
				this.maybeSendReactiveFaceEmotion(sc.inputTranscription.text)
			}
			if (sc.outputTranscription?.text) {
				console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Gemini output transcript ${sc.outputTranscription.text.length} chars`)
				this.currentAssistantText += sc.outputTranscription.text
				this.sendToDevice({
					type: 'transcript',
					source: 'model',
					text: sc.outputTranscription.text,
				})
			}

			// Turn complete — save exchange to D1 and optionally send a quick
			// traffic-light grammar signal for the learner's just-finished turn.
			if (sc.turnComplete) {
				console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Gemini turnComplete`)
				const userForFeedback = this.currentUserText.trim()
				const hadAudio = this.currentTurnModelAudioBytes > 0
				const hadModelText = this.currentAssistantText.trim().length > 0
				const hadToolActivity = this.currentTurnHadToolActivity
				this.sendToDevice({
					type: 'turn_complete',
					turn_id: this.activeServerTurnId ?? 0,
					had_audio: hadAudio,
					had_model_text: hadModelText,
					had_tool_activity: hadToolActivity,
					synthetic: false,
					reason: 'model_turn_complete',
				})
				await this.commitExchange()
				this.currentTurnModelAudioBytes = 0
				this.currentTurnHadToolActivity = false
				this.maybeSendTurnFeedback(userForFeedback, true)
			}
		}

		if (msg.toolCall) {
			this.currentTurnHadToolActivity = true
			if (this.deviceRecording) {
				console.warn(`[Turn ${this.activeServerTurnId ?? '-'}] BUG: Gemini toolCall while device still recording`)
			}
			for (const call of msg.toolCall.functionCalls) {
				console.log(`[Turn ${this.activeServerTurnId ?? '-'}] Gemini toolCall: ${call.name}(${JSON.stringify(call.args)})`)
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
				} else if (call.name === 'search_learning_resources') {
					const args = call.args as { query?: string; limit?: number; source?: 'worksheet' | 'roleplay' | 'graded_reader' }
					try {
						const response = await searchLearningResources({
							env: this.env,
							deviceId: this.deviceId,
							query: args.query || '',
							limit: args.limit || 5,
							source: args.source,
						})
						this.lastLearningSearchResults = response.results
						await this.state.storage.put('lastLearningSearchResults', response.results)
						const results = response.results.map((r) => ({
							resource_id: r.resourceId,
							title: r.title,
							source: r.source,
							level: r.level,
							description: r.description,
							matched_text: r.matchedText,
							score: r.score,
						}))
						this.geminiWs?.send(JSON.stringify({
							toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
								results,
								instruction: 'If the user asked to start/practice a specific lesson now, immediately call load_learning_resource with the best matching resource_id (prefer worksheet over duplicate roleplay). Otherwise ask which lesson they want.'
							} }] },
						}))
						await this.logToolCall({ name: call.name, args: call.args, result: { count: results.length, titles: results.map((r) => r.title) }, handledBy: 'server', durationMs: Date.now() - startMs })
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: { error: message } }] } }))
						await this.logToolCall({ name: call.name, args: call.args, handledBy: 'server', status: 'error', error: message, durationMs: Date.now() - startMs })
					}
				} else if (call.name === 'load_learning_resource') {
					const args = call.args as { resource_id?: string; max_chars?: number }
					try {
						const context = await loadLearningResourceContext({
							env: this.env,
							deviceId: this.deviceId,
							resourceId: args.resource_id || '',
							maxChars: args.max_chars || 8000,
						})
						this.activeLearningContext = context
						await this.state.storage.put('activeLearningContext', context)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							loaded: true,
							resource_id: context.resourceId,
							title: context.title,
							tutor_mode: context.tutorMode,
							prompt_context: context.promptContext,
							instruction: context.source === 'roleplay'
								? 'ROLEPLAY MODE IS ACTIVE NOW. Speak German-only and stay in character. Speak first: assign roles in simple German and say your first in-character German line/question. If the learner says hello/hallo, answer in German in character, never in English. Do not ask what the learner wants to do. Do not wait for the learner to start. Do not use English unless the learner explicitly asks.'
								: 'Start the lesson now. Ask one short question at a time. You may call show_text with the title if helpful.'
						} }] } }))
						await this.logToolCall({ name: call.name, args: call.args, result: { resourceId: context.resourceId, title: context.title, chars: context.promptContext.length, truncated: context.metadata.truncated }, handledBy: 'server', durationMs: Date.now() - startMs })
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: { loaded: false, error: message } }] } }))
						await this.logToolCall({ name: call.name, args: call.args, handledBy: 'server', status: 'error', error: message, durationMs: Date.now() - startMs })
					}
				} else if (call.name === 'search_reader_passages') {
					const args = call.args as { resource_id?: string; query?: string; limit?: number }
					try {
						const resourceId = args.resource_id || (this.activeLearningContext?.source === 'graded_reader' ? this.activeLearningContext.resourceId : '')
						const response = await searchReaderPassages({
							env: this.env,
							deviceId: this.deviceId,
							resourceId,
							query: args.query || '',
							limit: args.limit || 5,
						})
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							resource_id: response.resourceId,
							query: response.query,
							results: response.results.map((r) => ({
								passage_index: r.passageIndex,
								sentence_index: r.sentenceIndex,
								title: r.title,
								quote: r.quote,
								snippet: r.snippet,
								score: r.score,
							})),
							instruction: 'Tell the user the best matching quote briefly. If they want to hear/read/discuss that part, call load_reader_passage with the passage_index and sentence_index.'
						} }] } }))
						await this.logToolCall({ name: call.name, args: call.args, result: { count: response.results.length, top: response.results[0]?.quote }, handledBy: 'server', durationMs: Date.now() - startMs })
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: { error: message } }] } }))
						await this.logToolCall({ name: call.name, args: call.args, handledBy: 'server', status: 'error', error: message, durationMs: Date.now() - startMs })
					}
				} else if (call.name === 'load_reader_passage') {
					const args = call.args as { resource_id?: string; passage_index?: number; sentence_index?: number; max_chars?: number }
					try {
						const context = await loadLearningResourceContext({
							env: this.env,
							deviceId: this.deviceId,
							resourceId: args.resource_id || '',
							maxChars: args.max_chars || 8000,
							passageIndex: args.passage_index ?? 0,
							sentenceIndex: args.sentence_index,
						})
						this.activeLearningContext = context
						await this.state.storage.put('activeLearningContext', context)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							loaded: true,
							resource_id: context.resourceId,
							title: context.title,
							passage_index: context.metadata.loadedPassageIndex,
							sentence_index: context.metadata.loadedSentenceIndex,
							prompt_context: context.promptContext,
							instruction: 'Read or discuss this focused passage window. Start by saying you found the part, then quote or read a short section.'
						} }] } }))
						await this.logToolCall({ name: call.name, args: call.args, result: { resourceId: context.resourceId, passageIndex: context.metadata.loadedPassageIndex, chars: context.promptContext.length }, handledBy: 'server', durationMs: Date.now() - startMs })
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: { loaded: false, error: message } }] } }))
						await this.logToolCall({ name: call.name, args: call.args, handledBy: 'server', status: 'error', error: message, durationMs: Date.now() - startMs })
					}
				} else if (call.name === 'get_current_learning_resource') {
					this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: this.activeLearningContext ? {
						active: true,
						resource_id: this.activeLearningContext.resourceId,
						title: this.activeLearningContext.title,
						tutor_mode: this.activeLearningContext.tutorMode,
						prompt_context: this.activeLearningContext.promptContext.slice(0, 4000),
					} : { active: false } }] } }))
					await this.logToolCall({ name: call.name, args: call.args, result: this.activeLearningContext ? { resourceId: this.activeLearningContext.resourceId, title: this.activeLearningContext.title } : { active: false }, handledBy: 'server', durationMs: Date.now() - startMs })
				} else if (call.name === 'clear_learning_resource') {
					const previous = this.activeLearningContext?.resourceId
					this.activeLearningContext = null
					await this.state.storage.delete('activeLearningContext')
					this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: { cleared: true, previous_resource_id: previous } }] } }))
					await this.logToolCall({ name: call.name, args: call.args, result: { cleared: true, previous }, handledBy: 'server', durationMs: Date.now() - startMs })
				} else if (call.name === 'save_flashcard') {
					const args = call.args as { front?: string; back?: string; tags?: string[]; source?: string; note?: string }
					try {
						if (!args.front || !args.back) {
							throw new Error('front and back are required')
						}
						const result = await saveFlashcard({
							env: this.env,
							deviceId: this.deviceId,
							chatId: this.chatId,
							front: args.front,
							back: args.back,
							tags: args.tags,
							source: args.source ?? (this.activeLearningContext ? `${this.activeLearningContext.source}:${this.activeLearningContext.resourceId}` : undefined),
							note: args.note,
						})
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							saved: result.saved !== false,
							id: result.id,
							instruction: 'Briefly confirm to the learner what you saved (e.g. "Saved: <front> → <back>"). Do not repeat the full card multiple times.'
						} }] } }))
						await this.logToolCall({ name: call.name, args: call.args, result: { id: result.id, saved: result.saved !== false }, handledBy: 'server', durationMs: Date.now() - startMs })
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: { saved: false, error: message } }] } }))
						await this.logToolCall({ name: call.name, args: call.args, handledBy: 'server', status: 'error', error: message, durationMs: Date.now() - startMs })
					}
				} else if (call.name === 'end_practice_review') {
					try {
						await this.commitExchange()
						const review = await this.generatePracticeReview()
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							...review,
							instruction: 'Read this end-of-practice review aloud concisely. Use a warm tutor voice. Do not add extra mistakes beyond the transcript-based review.'
						} }] } }))
						await this.logToolCall({ name: call.name, args: call.args, result: { grade: review.grade, mistakeCount: review.mistakes.length }, handledBy: 'server', durationMs: Date.now() - startMs })
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.geminiWs?.send(JSON.stringify({ toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							grade: 'not enough evidence',
							overall: 'I could not generate a transcript-based review this time.',
							mistakes: [],
							next_practice: 'Try one more short roleplay and ask for a review again.',
							error: message,
							instruction: 'Briefly apologize and give this fallback review. Do not invent specific mistakes.'
						} }] } }))
						await this.logToolCall({ name: call.name, args: call.args, handledBy: 'server', status: 'error', error: message, durationMs: Date.now() - startMs })
					}
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
				} else if (call.name === 'new_conversation' || call.name === 'new_chat') {
					// This tool is no longer declared because accidental calls were splitting
					// active learning sessions. Keep the handler defensive in case an older
					// setup/model still tries to call it.
					console.warn(`[Gemini] Ignoring ${call.name} tool call; chat reset is device-menu only`)
					this.geminiWs?.send(JSON.stringify({
						toolResponse: { functionResponses: [{ name: call.name, id: call.id, response: {
							reset: false,
							instruction: 'Chat reset is disabled from model tools. Tell the user to use the device menu item New conversation if they want a fresh chat.',
						} }] },
					}))
					await this.logToolCall({ name: call.name, args: call.args, result: { reset: false }, handledBy: 'server', durationMs: Date.now() - startMs })
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

	private async handleFaceControl(request: Request): Promise<Response> {
		let body: { emotion?: unknown; look_x?: unknown; look_y?: unknown; eye_spacing?: unknown; anim_speed?: unknown }
		try {
			body = await request.json()
		} catch {
			return new Response('Invalid JSON', { status: 400 })
		}

		const emotion = typeof body.emotion === 'string' ? body.emotion : 'default'
		const normalized = ['default', 'angry', 'focused', 'eepy', 'thinking'].includes(emotion) ? emotion : 'default'
		const lookX = typeof body.look_x === 'number' ? Math.max(-1, Math.min(1, body.look_x)) : 0
		const lookY = typeof body.look_y === 'number' ? Math.max(-1, Math.min(1, body.look_y)) : 0
		const eyeSpacing = typeof body.eye_spacing === 'number' ? Math.max(36, Math.min(70, body.eye_spacing)) : 52
		const animSpeed = typeof body.anim_speed === 'number' ? Math.max(0.25, Math.min(3, body.anim_speed)) : 1
		this.sendToDevice({ type: 'face_control', emotion: normalized, look_x: lookX, look_y: lookY, eye_spacing: eyeSpacing, anim_speed: animSpeed })
		return new Response(JSON.stringify({ ok: true, device_connected: !!this.deviceWs, emotion: normalized, look_x: lookX, look_y: lookY, eye_spacing: eyeSpacing, anim_speed: animSpeed }), {
			headers: { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' },
		})
	}

	private async loadConversationMessages(): Promise<ConversationMessage[]> {
		const row = await this.env.DB.prepare('SELECT messages FROM conversations WHERE chat_id = ?')
			.bind(this.chatId)
			.first<{ messages: string }>()
		return row?.messages ? JSON.parse(row.messages) : []
	}

	private async generatePracticeReview(): Promise<PracticeReview> {
		const messages = await this.loadConversationMessages()
		return generatePracticeReview({
			env: this.env,
			messages,
			activeLearningContext: this.activeLearningContext,
		})
	}

	private maybeSendTurnFeedback(userText: string, force = false) {
		if (!this.turnFeedbackEnabled) return
		const text = userText.trim()
		if (text.length < 2) return
		const now = Date.now()
		if (!force && now - this.lastTurnFeedbackAtMs < 2500) return
		if (text === this.lastTurnFeedbackText) return
		this.lastTurnFeedbackText = text
		this.lastTurnFeedbackAtMs = now
		console.log(`[TurnFeedback] Evaluating: "${text.slice(0, 80)}"`)
		this.sendTurnFeedback(text).catch((err) => {
			console.warn('[TurnFeedback] Failed:', err)
		})
	}

	private async sendTurnFeedback(userText: string) {
		const feedback = await evaluateTurnFeedback({
			env: this.env,
			userText,
			activeLearningContext: this.activeLearningContext,
		})
		console.log(`[TurnFeedback] ${feedback.color}: ${feedback.reason}`)
		this.sendToDevice({ type: 'turn_feedback', ...feedback })
	}

	private maybeSendReactiveFaceEmotion(text: string) {
		const now = Date.now()
		if (now - this.lastReactiveFaceEmotionAtMs < 1500) return
		const emotion = reactiveEmotionForTranscript(text)
		if (!emotion) return
		this.lastReactiveFaceEmotionAtMs = now
		this.sendToDevice({ type: 'face_emotion', emotion })
	}

	private sendToDevice(msg: Record<string, unknown>) {
		this.deviceWs?.send(JSON.stringify(msg))
	}

	private async logToolCall(entry: {
		name: string
		args: unknown
		result?: unknown
		handledBy: 'server' | 'device'
		status?: 'ok' | 'error'
		error?: string
		durationMs?: number
	}) {
		try {
			const argsStr = entry.args === undefined ? null : JSON.stringify(entry.args)
			const resultStr =
				entry.result === undefined
					? null
					: typeof entry.result === 'string'
						? entry.result
						: JSON.stringify(entry.result)
			// Truncate oversized payloads so we don't blow up D1
			const trim = (s: string | null) =>
				s && s.length > 8000 ? s.slice(0, 8000) + '…[truncated]' : s

			await this.env.DB.prepare(
				`INSERT INTO tool_log (device_id, chat_id, tool_name, args, result, handled_by, status, error, duration_ms)
				 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`
			)
				.bind(
					this.deviceId,
					this.chatId,
					entry.name,
					trim(argsStr),
					trim(resultStr),
					entry.handledBy,
					entry.status ?? 'ok',
					entry.error ?? null,
					entry.durationMs ?? null
				)
				.run()
			console.log(
				`[ToolLog] ${entry.name} by=${entry.handledBy} status=${entry.status ?? 'ok'}` +
					(entry.durationMs !== undefined ? ` ${entry.durationMs}ms` : '')
			)
		} catch (err) {
			console.error('[ToolLog] Failed to insert:', err)
		}
	}

	private async commitExchange() {
		const user = this.currentUserText.trim()
		const assistant = this.currentAssistantText.trim()
		this.currentUserText = ''
		this.currentAssistantText = ''
		await this.commitExchangeValues(user, assistant)
	}

	private async commitExchangeValues(user: string, assistant: string) {
		if (!user && !assistant) return

		try {
			// Get existing messages
			const row = await this.env.DB.prepare(
				'SELECT messages FROM conversations WHERE chat_id = ?'
			)
				.bind(this.chatId)
				.first<{ messages: string }>()

			const messages: ConversationMessage[] = row?.messages
				? JSON.parse(row.messages)
				: []

			if (user) messages.push({ role: 'user', content: user })
			if (assistant) messages.push({ role: 'assistant', content: assistant })

			// Keep enough recent turns to survive Live API reconnects and quick backrefs.
			const trimmed = messages.slice(-50)

			await this.env.DB.prepare(
				`INSERT INTO conversations (chat_id, device_id, messages, last_message, updated_at)
				 VALUES (?, ?, ?, ?, datetime('now'))
				 ON CONFLICT(chat_id) DO UPDATE SET
				   messages = excluded.messages,
				   last_message = excluded.last_message,
				   updated_at = excluded.updated_at`
			)
				.bind(this.chatId, this.deviceId, JSON.stringify(trimmed), assistant || null)
				.run()

			// Log the exchange
			await this.env.DB.prepare(
				`INSERT INTO message_log (device_id, chat_id, user_text, assistant_text)
				 VALUES (?, ?, ?, ?)`
			)
				.bind(this.deviceId, this.chatId, user || null, assistant || null)
				.run()

			console.log(`[DB] Saved exchange: chat=${this.chatId} (${trimmed.length} messages)`)
		} catch (err) {
			console.error('[DB] Failed to save exchange:', err)
		}
	}

	private async saveConversation() {
		// Final save on disconnect — commit any pending text
		await this.commitExchange()
	}

	private getIgnoredTurnReason(): 'too_short' | 'silent' | null {
		if (this.currentTurnSamples === 0) {
			return 'too_short'
		}

		const averageAbs = this.currentTurnAbsSum / this.currentTurnSamples
		if (averageAbs < LiveSession.SILENCE_AVG_ABS_THRESHOLD) {
			return 'silent'
		}

		// In active roleplays, very short utterances like "Hallo", "Ja", "Nein"
		// are valid turns. Do not discard them just because they are brief.
		if (this.activeLearningContext?.source === 'roleplay') {
			return null
		}

		if (this.currentTurnAudioBytes < LiveSession.MIN_TURN_BYTES) {
			return 'too_short'
		}

		return null
	}

	private resetCurrentTurnMetrics() {
		this.audioChunkCount = 0
		this.currentTurnAudioBytes = 0
		this.currentTurnAbsSum = 0
		this.currentTurnSamples = 0
	}

	private async cancelActiveTurn(reason: string) {
		console.warn(`[Turn ${this.activeServerTurnId ?? '-'}] Canceled: ${reason}`)
		this.deviceRecording = false

		// The user often interrupts while the assistant is still speaking. Gemini may
		// not send turnComplete in that case, so without this we lose the just-spoken
		// assistant text and any user text that led to it. Save the partial exchange
		// before reconnecting, so the next Live session can restore it as context.
		const user = this.currentUserText.trim()
		const assistant = this.currentAssistantText.trim()
		this.currentUserText = ''
		this.currentAssistantText = ''

		this.currentTurnModelAudioBytes = 0
		this.currentTurnHadToolActivity = false
		this.pendingAudioChunks = []
		this.pendingAudioBytes = 0
		this.pendingStartAfterReady = false
		this.pendingStopAfterReady = false
		for (const [id, pending] of this.pendingDeviceCalls) {
			this.pendingDeviceCalls.delete(id)
			this.logToolCall({
				name: pending.name,
				args: pending.args,
				handledBy: 'device',
				status: 'error',
				error: `turn canceled: ${reason}`,
				durationMs: Date.now() - pending.startMs,
			}).catch(() => {})
		}
		this.sendToDevice({ type: 'drop_audio', reason })

		// Mark Gemini unavailable immediately so a near-simultaneous device "start"
		// buffers audio instead of sending it to a session we're about to close.
		if (this.geminiWs) {
			try {
				this.geminiWs.close()
			} catch {
				// ignore
			}
		}
		this.geminiWs = null
		this.geminiReady = false
		// Hold off any immediate device start from opening a new Gemini session until
		// the partial exchange has been persisted and can be restored as context.
		this.geminiConnecting = true
		this.activeServerTurnId = null

		if (user || assistant) {
			console.log(`[DB] Saving partial canceled exchange: user=${user.length} assistant=${assistant.length}`)
			await this.commitExchangeValues(user, assistant)
		}

		this.geminiConnecting = false
		await this.connectGemini()
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
		await this.connectGemini()
	}

	private sendTrailingSilence() {
		if (!this.geminiWs) return
		// 1s of silence at 16kHz 16-bit mono = 32000 bytes
		const silence = new ArrayBuffer(32000)
		const base64 = arrayBufferToBase64(silence)
		this.geminiWs.send(
			JSON.stringify({
				realtimeInput: {
					audio: { data: base64, mimeType: 'audio/pcm;rate=16000' },
				},
			})
		)
		console.log('[Bridge] Sent 1s trailing silence')
		this.resetCurrentTurnMetrics()
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
		this.activeServerTurnId = null
		this.deviceRecording = false
		this.pendingAudioChunks = []
		this.pendingAudioBytes = 0
		this.pendingStartAfterReady = false
		this.pendingStopAfterReady = false
		this.geminiConnecting = false
	}
}

function buildLocationContext(request: Request): string {
	const cf = (request as Request & { cf?: IncomingRequestCfProperties }).cf
	const city = cf?.city || request.headers.get('cf-ipcity') || ''
	const region = cf?.region || request.headers.get('cf-region') || ''
	const country = cf?.country || request.headers.get('cf-ipcountry') || ''

	return [city, region, country].filter(Boolean).join(', ')
}

function parseVoiceMode(value: string | null): VoiceMode {
	return value === 'quiz_masters' ? 'quiz_masters' : 'assistant'
}

function parseBooleanFlag(value: string | undefined): boolean {
	return /^(1|true|yes|on)$/i.test((value ?? '').trim())
}

function reactiveEmotionForTranscript(text: string): 'angry' | 'eepy' | null {
	const lower = text.toLowerCase()
	if (/\b(fuck|fucking|shit|bullshit|asshole|bitch|bastard|cunt|idiot|stupid|dumb|schei(?:ß|ss)e|arsch|verdammt)\b/i.test(lower)) {
		return 'angry'
	}
	if (/\b(confused|don't understand|dont understand|ich verstehe nicht|keine ahnung)\b/i.test(lower)) {
		return 'eepy'
	}
	return null
}

function buildSystemInstruction(
	mode: VoiceMode,
	locationContext: string,
	activeLearningContext: LessonContextBundle | null | undefined,
	conversationEndReviewEnabled: boolean,
	recentConversationContext: string
): string {
	const base = [
		'You are running on an M5StickS3 — a tiny handheld ESP32-S3 voice device.',
		'',
		'Device specs:',
		'- Display: 135×240 pixel color LCD (ST7789)',
		'- Speaker: 8Ω 1W cavity speaker with AW8737 amplifier',
		'- Microphone: MEMS mic (SPM1423)',
		'- Battery: 250mAh rechargeable',
		'- Connectivity: WiFi 2.4GHz',
		'- Size: 48×24×15mm — fits in a palm',
		'',
	]

	if (locationContext) {
		base.push(
			`Approximate device location: ${locationContext}. Use it only when it helps answer location-sensitive requests.`,
			''
		)
	}

	base.push(
		'The user holds a button to talk and releases to hear your response.',
		'Keep responses concise — the speaker is tiny. Be helpful, warm, and conversational.',
		'Adaptive coaching: listen for the learner\'s confidence, hesitation, frustration, laughter, and energy. If they sound unsure or frustrated, slow down, reassure them, and give one tiny next step. If they sound confident, be a bit more playful and challenging. Never overdo praise; keep it natural.',
		'',
		'You can control the device using the available tools:',
		'- set_brightness: adjust display backlight',
		'- set_volume: adjust speaker volume',
		'- show_text: display a message on the screen',
		'- play_sound: play a named device sound effect',
		'- play_melody: play a short note sequence on the device speaker',
		'- power_off: shut the device down',
		'- get_device_status: check battery, volume, brightness, current voice mode, etc.',
		'- search_docs: search the indexed knowledge base',
		'- web_fetch: fetch a specific URL and read its text content',
		'- search_learning_resources: find German worksheet lessons, roleplays, mock-test practice, and graded readers',
		'- load_learning_resource: load selected lesson context before teaching it',
		'- get_current_learning_resource: check what lesson is active',
		'- search_reader_passages: search inside a loaded/selected graded reader for a topic or quote',
		'- load_reader_passage: load the focused passage window returned by reader passage search',
		'- clear_learning_resource: leave lesson mode',
		'- save_flashcard: save a flashcard (front/back, optional tags) when the learner asks to save a phrase, save a correction, or save the most useful item from the recent exchange',
		'- end_practice_review: finish German practice and get a transcript-based mistakes review plus grade',
		'- google_search: search the web for current information (news, facts, recent events)',
		'',
		'You have a search_docs tool that searches an indexed knowledge base.',
		'Use it when the user asks about topics that may be covered in the indexed documents.',
		'Use tools when the user asks to change device settings or needs information.',
		'If the user asks for a new conversation/reset, tell them to use the device menu item “New conversation”; do not reset the chat yourself.',
		'When the user asks to practice a German lesson, worksheet, dialogue, roleplay, graded reader, exam task, or a topic like cafe, doctor, train station, Bildbeschreibung, or B1 speaking, use search_learning_resources.',
		'If the user asks to list, browse, or choose available graded readers/books/stories/transcripts/videos, call search_learning_resources with source="graded_reader" and a query like "available graded readers". In results, distinguish Video transcript from Graded reader when subtitle/metadata indicates it.',
		'If a graded reader is active and the user asks to find the part where it says something, search for a topic inside the book, or read the relevant section, call search_reader_passages first, then load_reader_passage for the best match.',
		'If the user clearly asks to start a specific lesson or the best search result is obvious, call load_learning_resource immediately after search instead of asking for confirmation. Prefer a worksheet result over a duplicate roleplay result for the same lesson.',
		'When a lesson is loaded: act as a German tutor and roleplay partner; ask one question at a time; keep responses concise; prefer German with short English explanations when needed; correct mistakes gently by recasting; use loaded vocabulary, phrases, dialogues, and roleplay instructions.',
		'IMPORTANT for roleplays/dialogue practice: stay in German by default. Your spoken reply should be German-only unless the learner explicitly asks for English, translation, or explanation. For corrections, recast in German first and optionally add a tiny English gloss only when requested. When a roleplay starts, you must initiate the conversation: briefly assign roles in German, then speak the first in-character German line/question. Do not wait for the learner to start.',
		"When you don't understand the audio, say so briefly rather than guessing.",
		conversationEndReviewEnabled
			? 'Conversation end/review mode is ENABLED. For German practice, treat each lesson, roleplay, or exam task as having a natural finish. If the learner says they are done/finished, asks to end/review/get graded, or the task reaches a natural conclusion, stop advancing the scenario and call end_practice_review. Read the transcript-based tool result aloud; do not invent additional mistakes.'
			: 'Conversation end/review mode is DISABLED. Do not automatically grade sessions or give end-of-conversation mistake reviews unless the learner explicitly asks for feedback. If the learner explicitly asks for a review or grade, call end_practice_review.',
		conversationEndReviewEnabled
			? 'During practice, quietly remember recurring learner mistakes so the final review can be specific. Do not interrupt every turn with a long explanation; recast briefly and continue until the ending.'
			: '',
		''
	)

	if (activeLearningContext) {
		base.push(
			'CURRENTLY LOADED LEARNING RESOURCE:',
			activeLearningContext.promptContext.slice(0, 4000),
			''
		)
	}

	if (recentConversationContext) {
		base.push(
			'RECENT CONVERSATION CONTEXT FROM BEFORE THIS LIVE CONNECTION:',
			recentConversationContext,
			'Use this as memory. Do not announce that you restored context; just continue naturally.',
			''
		)
	}

	if (mode === 'quiz_masters') {
		base.push(
			'Voice mode: Quiz Masters.',
			'You are Quiz Masters, a friendly nature quiz game host for kids.',
			'Run an interactive quiz about nature, animals, plants, weather, space, oceans, forests, bugs, dinosaurs, and simple science.',
			'',
			'Game rules:',
			'- Ask one question at a time.',
			'- Wait for the child to answer before revealing the answer.',
			'- Be encouraging, playful, and kind.',
			'- Keep questions age-appropriate and short.',
			'- If the answer is wrong, give a gentle hint or a simple explanation.',
			'- Track score casually if the child wants, but do not be strict.',
			'- Use show_text to display the current question, answer, or score when helpful.',
			'- Use play_sound or play_melody for correct answers, celebrations, or game start.',
			'- Avoid scary, violent, adult, or inappropriate topics.',
			'',
			'Start by greeting the players and asking what kind of nature quiz they want: animals, oceans, forests, bugs, weather, dinosaurs, space, or mixed.'
		)
		return base.join('\n')
	}

	base.push(
		'Voice mode: Assistant.',
		'You are a helpful, warm voice assistant.',
		'Answer questions, help with tasks, and use tools when needed.'
	)
	return base.join('\n')
}

// ─── Utilities ───

async function evaluateTurnFeedback(input: {
	env: Env
	userText: string
	activeLearningContext: LessonContextBundle | null
}): Promise<TurnFeedback> {
	const text = input.userText.trim()
	if (text.length < 2) {
		return { color: 'gray', correction: '', reason: 'Too short to judge.' }
	}

	const context = input.activeLearningContext
		? `${input.activeLearningContext.source}: ${input.activeLearningContext.title}`
		: 'General German conversation.'
	const prompt = [
		'You are a German tutor giving instant traffic-light grammar feedback for ONE learner utterance.',
		'Classify the learner utterance as:',
		'- green: German is grammatically correct/natural enough for spoken practice, including appropriate short answers.',
		'- yellow: understandable but has grammar/case/article/gender/word-order/ending errors.',
		'- red: mostly not understandable, not German, or structurally very wrong.',
		'- gray: too short, unclear transcript, or cannot judge.',
		'Be encouraging and do not punish likely speech-to-text spelling/capitalization issues. Return ONLY valid JSON: {"color":"green|yellow|red|gray","correction":"corrected German sentence or empty string","reason":"very short English reason"}',
		`Context: ${context}`,
		`Learner utterance: ${text}`,
	].join('\n')

	const url = `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=${input.env.GEMINI_API_KEY}`
	const resp = await fetch(url, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({
			contents: [{ role: 'user', parts: [{ text: prompt }] }],
			generationConfig: { temperature: 0.1, responseMimeType: 'application/json' },
		}),
	})
	if (!resp.ok) throw new Error(`turn feedback model failed: ${resp.status}`)
	const body = await resp.json() as { candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }> }
	const raw = body.candidates?.[0]?.content?.parts?.map((p) => p.text ?? '').join('').trim() ?? ''
	if (!raw) throw new Error('turn feedback model returned empty response')
	return normalizeTurnFeedback(JSON.parse(stripJsonFence(raw)))
}

function normalizeTurnFeedback(value: unknown): TurnFeedback {
	const obj = value && typeof value === 'object' ? value as Record<string, unknown> : {}
	const rawColor = typeof obj.color === 'string' ? obj.color.toLowerCase() : 'gray'
	const color: TurnFeedback['color'] = rawColor === 'green' || rawColor === 'yellow' || rawColor === 'red' || rawColor === 'gray' ? rawColor : 'gray'
	return {
		color,
		correction: typeof obj.correction === 'string' ? obj.correction.trim().slice(0, 300) : '',
		reason: typeof obj.reason === 'string' ? obj.reason.trim().slice(0, 200) : '',
	}
}

async function generatePracticeReview(input: {
	env: Env
	messages: ConversationMessage[]
	activeLearningContext: LessonContextBundle | null
}): Promise<PracticeReview> {
	const learnerTurns = input.messages.filter((m) => m.role === 'user' && m.content.trim()).length
	if (learnerTurns < 2) {
		return {
			grade: 'not enough evidence',
			overall: 'There was not enough learner speech to grade fairly yet.',
			mistakes: [],
			next_practice: 'Try a short roleplay with at least a few answers, then ask for a review again.',
		}
	}

	const transcript = input.messages
		.map((m) => `${m.role === 'user' ? 'Learner' : 'Tutor'}: ${m.content}`)
		.join('\n')
		.slice(-14000)
	const context = input.activeLearningContext
		? `${input.activeLearningContext.source}: ${input.activeLearningContext.title} (${input.activeLearningContext.resourceId})`
		: 'No specific learning resource loaded.'

	const prompt = [
		'You are a strict but encouraging German tutor. Review this German practice transcript.',
		'Base your review ONLY on the transcript. Do not invent mistakes or learner sentences.',
		'Return ONLY valid JSON with this shape:',
		'{"grade":"A|B|C|D|E|F|not enough evidence","overall":"one concise encouraging sentence","mistakes":[{"learner_said":"exact learner phrase or short paraphrase","corrected":"correct German","explanation":"brief explanation in English"}],"next_practice":"one concise suggestion"}',
		'Rules: max 3 mistakes; prefer high-value recurring or important errors; if the transcript has too little German from the learner, use grade "not enough evidence" and an empty mistakes array; keep every field concise for spoken output.',
		`Learning context: ${context}`,
		'',
		'Transcript:',
		transcript,
	].join('\n')

	const url = `https://generativelanguage.googleapis.com/v1beta/models/gemini-2.5-flash:generateContent?key=${input.env.GEMINI_API_KEY}`
	const resp = await fetch(url, {
		method: 'POST',
		headers: { 'Content-Type': 'application/json' },
		body: JSON.stringify({
			contents: [{ role: 'user', parts: [{ text: prompt }] }],
			generationConfig: {
				temperature: 0.2,
				responseMimeType: 'application/json',
			},
		}),
	})
	if (!resp.ok) {
		throw new Error(`review model failed: ${resp.status} ${await resp.text()}`)
	}
	const body = await resp.json() as {
		candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>
	}
	const text = body.candidates?.[0]?.content?.parts?.map((p) => p.text ?? '').join('').trim() ?? ''
	if (!text) throw new Error('review model returned empty response')
	return normalizePracticeReview(JSON.parse(stripJsonFence(text)))
}

function normalizePracticeReview(value: unknown): PracticeReview {
	const obj = value && typeof value === 'object' ? value as Record<string, unknown> : {}
	const mistakesRaw = Array.isArray(obj.mistakes) ? obj.mistakes : []
	return {
		grade: typeof obj.grade === 'string' && obj.grade.trim() ? obj.grade.trim().slice(0, 32) : 'not enough evidence',
		overall: typeof obj.overall === 'string' ? obj.overall.trim().slice(0, 500) : '',
		mistakes: mistakesRaw.slice(0, 3).map((m) => {
			const item = m && typeof m === 'object' ? m as Record<string, unknown> : {}
			return {
				learner_said: typeof item.learner_said === 'string' ? item.learner_said.trim().slice(0, 250) : '',
				corrected: typeof item.corrected === 'string' ? item.corrected.trim().slice(0, 250) : '',
				explanation: typeof item.explanation === 'string' ? item.explanation.trim().slice(0, 300) : '',
			}
		}).filter((m) => m.learner_said || m.corrected || m.explanation),
		next_practice: typeof obj.next_practice === 'string' ? obj.next_practice.trim().slice(0, 400) : '',
	}
}

function stripJsonFence(text: string): string {
	return text.replace(/^```(?:json)?\s*/i, '').replace(/\s*```$/, '').trim()
}

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
