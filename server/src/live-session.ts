import { type EmailEnv, emailEnabled } from './email'
import {
	type ConversationMessage,
	insertToolLog,
	saveConversationExchange,
} from './conversation-store'
import {
	USER_INSTRUCTIONS_PATH,
	ensureUserInstructionsFile,
} from './files'
import { handleFileTool } from './file-tools'
import {
	DEFAULT_IMAGE_WIDTH,
	DEFAULT_IMAGE_HEIGHT,
} from './image-gen'
import {
	generateAndSendImage,
	handleImageTool,
	imagePromptFromArgs,
} from './image-tool'
import { buildGeminiTools, buildToolResponsePayload } from './gemini-tools'
import {
	DEFAULT_VOICE,
	findVoice,
	handleSetVoiceTool,
	resolveVoice,
} from './voice-tool'
import {
	DEFAULT_THINKING_LEVEL,
	THINKING_LEVEL_LIST_TEXT,
	buildSystemInstructionText,
	resolveThinkingLevel,
	type ThinkingLevel,
	type VoiceMode,
} from './prompt-builder'
import {
	loadLearningResourceContext,
	saveFlashcard,
	searchLearningResources,
	searchReaderPassages,
} from './flashcard-api'
import type { LessonContextBundle, LearningResourceSummary } from './learning-types'
import { appendTranscriptDelta } from './transcript-delta'
import {
	MAX_DEBUG_AUDIO_BYTES,
	type DebugAudioEnv,
	handleDebugAudioRequest,
	saveDebugAudio,
} from './debug-audio-store'
import { handleDocsSearchTool } from './docs-tool'
import { handleEmailTool } from './email-tool'
import { type WebFetchArgs, fetchWebPage } from './web-fetch'
import { GeminiClient } from './gemini-client'
import {
	GEMINI_LIVE_MODEL,
	buildGeminiActivityStartPayload,
	buildGeminiAudioStreamEndPayload,
	buildGeminiRealtimeAudioPayload,
	buildGeminiRealtimeTextPayload,
} from './gemini-live'

interface Env extends EmailEnv, DebugAudioEnv {
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	STORAGE?: R2Bucket
	FLASHCARD_APP_BASE_URL?: string
	FLASHCARD_APP_BRIDGE_TOKEN?: string
	CHAT_STICK_LINK_TOKEN?: string
	CONVERSATION_END_REVIEW_ENABLED?: string
	TURN_FEEDBACK_ENABLED?: string
}

interface GeminiContentTurn {
	role: 'user' | 'model'
	parts: Array<{ text: string }>
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
		generationComplete?: boolean
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
	toolCallCancellation?: {
		ids?: string[]
	}
	goAway?: {
		timeLeft?: unknown
	}
	sessionResumptionUpdate?: {
		newHandle?: string
		resumable?: boolean
	}
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

function parsePositiveInt(value: string | null | undefined, fallback: number): number {
	if (!value) return fallback
	const parsed = Number.parseInt(value, 10)
	return Number.isFinite(parsed) && parsed > 0 ? parsed : fallback
}

export class LiveSession {
	private static readonly MIN_RECONNECT_MS = 1500
	private static readonly IDLE_CLOSE_MS = 120_000
	private static readonly MAX_QUEUED_AUDIO_BYTES = 1_000_000
	private state: DurableObjectState
	private env: Env
	private deviceWs: WebSocket | null = null
	private gemini = new GeminiClient()
	private deviceId = 'unknown'
	private chatId = ''
	private imageTargetWidth: number = DEFAULT_IMAGE_WIDTH
	private imageTargetHeight: number = DEFAULT_IMAGE_HEIGHT
	private voiceMode: VoiceMode = 'assistant'
	private currentVoice = DEFAULT_VOICE
	private currentThinkingLevel: ThinkingLevel = DEFAULT_THINKING_LEVEL
	private currentUserText = ''
	private currentAssistantText = ''
	private sessionGeneration = 0
	private lastConnectionAt = 0
	private lastActivityMs = 0
	private locationContext = ''
	private recentConversationContext = ''
	private currentTurnAudioBytes = 0
	private currentTurnAbsSum = 0
	private currentTurnSamples = 0
	private currentTurnModelAudioBytes = 0
	private currentTurnHadToolActivity = false
	private currentTurnStartedAt = 0
	private currentTurnDebugAudioChunks: ArrayBuffer[] = []
	private currentTurnDebugAudioBytes = 0
	private currentTurnDebugDroppedBytes = 0
	private queuedAudioChunks: ArrayBuffer[] = []
	private queuedAudioBytes = 0
	private pendingStopAfterGeminiReady = false
	private pendingActivityStartAfterGeminiReady = false
	private activityOpen = false
	private pendingReconnectAfterTurn = false
	private pendingInitialHistoryTurns: GeminiContentTurn[] = []
	private sessionResumptionHandle: string | null = null
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

	private async loadRecentConversationContext(): Promise<string> {
		if (!this.chatId) return ''
		try {
			const row = await this.env.DB.prepare('SELECT messages FROM conversations WHERE chat_id = ?')
				.bind(this.chatId)
				.first<{ messages: string | null }>()
			const messages = row?.messages ? (JSON.parse(row.messages) as ConversationMessage[]) : []
			const recent = messages.slice(-24)
			if (recent.length === 0) return ''
			return recent
				.map((message) => `${message.role === 'user' ? 'User' : 'Assistant'}: ${message.content}`)
				.join('\n')
				.slice(-6000)
		} catch (err) {
			console.warn('[DB] Failed to load recent conversation context:', err)
			return ''
		}
	}

	async fetch(request: Request): Promise<Response> {
		const url = new URL(request.url)
		if (url.pathname.match(/^\/debug\/audio\/[^/]+\/latest\.(wav|json)$/)) {
			return handleDebugAudioRequest(request, this.state.storage, this.env)
		}

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

		// Extract device_id, chat_id, voice, and image dimensions from URL
		const requestedChatId = url.searchParams.get('chat_id')
		this.deviceId = url.searchParams.get('device_id') || 'unknown'
		this.chatId = requestedChatId || crypto.randomUUID()
		this.voiceMode = parseVoiceMode(url.searchParams.get('mode'))
		this.currentVoice = resolveVoice(url.searchParams.get('voice'))
		this.currentThinkingLevel = requestedChatId
			? await this.loadThinkingLevelForChat(this.chatId)
			: DEFAULT_THINKING_LEVEL
		this.locationContext = buildLocationContext(request)
		this.recentConversationContext = await this.loadRecentConversationContext()
		this.conversationEndReviewEnabled = parseBooleanFlag(this.env.CONVERSATION_END_REVIEW_ENABLED)
		this.turnFeedbackEnabled = parseBooleanFlag(this.env.TURN_FEEDBACK_ENABLED)
		this.activeLearningContext =
			(await this.state.storage.get<LessonContextBundle>('activeLearningContext')) ?? null
		this.lastLearningSearchResults =
			(await this.state.storage.get<LearningResourceSummary[]>('lastLearningSearchResults')) ?? []
		this.imageTargetWidth = parsePositiveInt(
			url.searchParams.get('image_w'),
			DEFAULT_IMAGE_WIDTH
		)
		this.imageTargetHeight = parsePositiveInt(
			url.searchParams.get('image_h'),
			DEFAULT_IMAGE_HEIGHT
		)

		console.log(
			`[Device] Connected: device=${this.deviceId} chat=${this.chatId} ` +
				`mode=${this.voiceMode} image=${this.imageTargetWidth}x${this.imageTargetHeight} ` +
				`endReview=${this.conversationEndReviewEnabled} turnFeedback=${this.turnFeedbackEnabled}`
		)

		const pair = new WebSocketPair()
		const [client, server] = Object.values(pair)

		server.accept()
		this.deviceWs = server
		this.lastActivityMs = Date.now()
		await this.state.storage.setAlarm(Date.now() + LiveSession.IDLE_CLOSE_MS)

		// Send chat_id to device (in case it was server-generated)
		this.sendToDevice({ type: 'session', chatId: this.chatId, mode: this.voiceMode })
		this.sendToDevice({ type: 'server_ready' })

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

		return new Response(null, { status: 101, webSocket: client })
	}

	private async connectGemini(sessionGeneration = this.sessionGeneration) {
		if (this.gemini.isConnecting) {
			return
		}

		if (this.gemini.isConnected) {
			console.log('[Gemini] Closing prior session before opening new one')
			this.gemini.close()
		}

		const userInstructions = await this.getUserInstructionsForPrompt()
		this.sessionResumptionHandle = await this.loadSessionResumptionHandle()
		this.pendingInitialHistoryTurns = this.sessionResumptionHandle
			? []
			: await this.getConversationHistoryForPrompt()
		this.activityOpen = false

		try {
			const resumptionHandleForAttempt = this.sessionResumptionHandle
			const voice = findVoice(this.currentVoice) ?? findVoice(DEFAULT_VOICE)!
			const canEmail = emailEnabled(this.env)
			const systemInstructionText = buildSystemInstructionText({
				voice,
				thinkingLevel: this.currentThinkingLevel,
				voiceMode: this.voiceMode,
				locationContext: this.locationContext,
				userInstructions,
				canEmail,
				activeLearningContext: this.activeLearningContext,
				conversationEndReviewEnabled: this.conversationEndReviewEnabled,
				recentConversationContext: this.recentConversationContext,
			})

			const setup: Record<string, unknown> = {
				model: GEMINI_LIVE_MODEL,
				generationConfig: {
					responseModalities: ['AUDIO'],
					speechConfig: {
						voiceConfig: {
							prebuiltVoiceConfig: { voiceName: voice.name },
						},
					},
					thinkingConfig: {
						thinkingLevel: this.currentThinkingLevel,
					},
				},
				realtimeInputConfig: {
					automaticActivityDetection: { disabled: true },
					activityHandling: 'START_OF_ACTIVITY_INTERRUPTS',
				},
				sessionResumption: this.sessionResumptionHandle
					? { handle: this.sessionResumptionHandle }
					: {},
				contextWindowCompression: { slidingWindow: {} },
				outputAudioTranscription: {},
				inputAudioTranscription: {},
				systemInstruction: {
					parts: [
						{
							text: systemInstructionText,
						},
					],
				},
			}

			if (this.pendingInitialHistoryTurns.length > 0) {
				setup.historyConfig = { initialHistoryInClientContent: true }
			}

			const result = await this.gemini.connect({
				apiKey: this.env.GEMINI_API_KEY,
				setupPayload: JSON.stringify({
					setup: {
						...setup,
						tools: buildGeminiTools({ canEmail }),
					},
				}),
				isCurrent: () => sessionGeneration === this.sessionGeneration,
				onMessage: (text) => {
					this.onGeminiMessage(text).catch((err) => {
						console.error('[Gemini] Message handler error:', err)
					})
				},
				onClose: (event, wasReady) => {
					if (sessionGeneration !== this.sessionGeneration) return
					console.log(
						`[Gemini] Disconnected: code=${event.code} reason="${event.reason}" wasReady=${wasReady}`,
					)
					this.activityOpen = false
					// Don't send error if we haven't set up yet — connectGemini will retry
					if (!wasReady && resumptionHandleForAttempt) {
						this.clearSessionResumptionHandle()
							.then(() => {
								if (sessionGeneration === this.sessionGeneration) {
									this.connectGemini(sessionGeneration).catch((err) => {
										console.error('[Gemini] Failed to retry without session resumption:', err)
									})
								}
							})
							.catch((err) => {
								console.error('[Gemini] Failed to clear bad session handle:', err)
							})
					}
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

	private ensureGeminiSession(sessionGeneration = this.sessionGeneration) {
		if (this.gemini.isConnected || this.gemini.isConnecting) return
		console.log('[Gemini] Starting session on first device input')
		this.connectGemini(sessionGeneration).catch((err) => {
			console.error('[Gemini] Failed to start session:', err)
		})
	}

	private trackIncomingAudio(data: ArrayBuffer) {
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
		if (this.audioChunkCount <= 3 || this.audioChunkCount % 10 === 0) {
			const firstSamples = Array.from(view.slice(0, 4))
			console.log(
				`[Bridge] Audio chunk #${this.audioChunkCount}: ${data.byteLength} bytes (samples: ${firstSamples})`,
			)
		}
	}

	private queueAudioChunk(data: ArrayBuffer) {
		while (
			this.queuedAudioBytes + data.byteLength > LiveSession.MAX_QUEUED_AUDIO_BYTES &&
			this.queuedAudioChunks.length > 0
		) {
			const dropped = this.queuedAudioChunks.shift()
			this.queuedAudioBytes -= dropped?.byteLength ?? 0
		}

		if (data.byteLength > LiveSession.MAX_QUEUED_AUDIO_BYTES) {
			console.warn(`[Bridge] Dropping oversized queued audio chunk (${data.byteLength} bytes)`)
			return
		}

		this.queuedAudioChunks.push(data)
		this.queuedAudioBytes += data.byteLength
	}

	private flushQueuedAudioToGemini() {
		if (!this.gemini.isReady || this.queuedAudioChunks.length === 0) {
			return
		}

		const chunks = this.queuedAudioChunks
		const bytes = this.queuedAudioBytes
		this.queuedAudioChunks = []
		this.queuedAudioBytes = 0
		console.log(`[Bridge] Flushing ${chunks.length} queued audio chunks (${bytes} bytes)`)
		for (const chunk of chunks) {
			this.sendAudioChunkToGemini(chunk)
		}
	}

	private sendActivityStartToGemini(): boolean {
		if (!this.gemini.isReady) return false
		if (this.activityOpen) return true
		this.gemini.send(buildGeminiActivityStartPayload())
		this.activityOpen = true
		console.log('[Bridge] Sent activityStart')
		return true
	}

	private sendActivityEndToGemini(): boolean {
		if (!this.gemini.isReady) return false
		if (!this.activityOpen) return true
		this.gemini.send(buildGeminiAudioStreamEndPayload())
		this.activityOpen = false
		console.log('[Bridge] Sent audioStreamEnd')
		return true
	}

	private sendAudioChunkToGemini(data: ArrayBuffer): boolean {
		if (!this.gemini.isReady) {
			return false
		}
		if (!this.activityOpen) {
			this.sendActivityStartToGemini()
		}

		const base64 = arrayBufferToBase64(data)
		this.gemini.send(buildGeminiRealtimeAudioPayload(base64))
		return true
	}

	private handleStopSignal() {
		const ignoreReason = this.getIgnoredTurnReason()
		this.saveCurrentTurnDebugAudio(ignoreReason ? `ignored:${ignoreReason}` : 'stop')
		if (ignoreReason) {
			const avgAbs = this.currentTurnAverageAbs()
			console.log(
				`[Bridge] Ignoring accidental clip (${ignoreReason}, bytes=${this.currentTurnAudioBytes}, avg_abs=${avgAbs}, chunks=${this.audioChunkCount})`,
			)
			this.currentUserText = ''
			this.currentAssistantText = ''
			this.sendToDevice({
				type: 'ignore_audio',
				reason: ignoreReason,
				bytes: this.currentTurnAudioBytes,
				avg_abs: avgAbs,
				chunks: this.audioChunkCount,
			})
			this.reconnectGeminiSession({ clearResumptionHandle: true }).catch((err) => {
				console.error('[Gemini] Failed to reset ignored turn:', err)
			})
			return
		}

		console.log(
			`[Bridge] Ending audio stream bytes=${this.currentTurnAudioBytes} avg_abs=${this.currentTurnAverageAbs()} chunks=${this.audioChunkCount}`,
		)
		this.audioChunkCount = 0
		this.sendActivityEndToGemini()
		this.resetCurrentTurnMetrics()
	}

	private onDeviceMessage(data: string | ArrayBuffer) {
		this.lastActivityMs = Date.now()
		if (data instanceof ArrayBuffer) {
			this.trackIncomingAudio(data)
			if (!this.gemini.isReady) {
				this.queueAudioChunk(data)
				this.ensureGeminiSession()
				return
			}
			this.sendAudioChunkToGemini(data)
		} else {
			// Text frame = control message
			try {
				const msg = JSON.parse(data)
				console.log('[Device]', msg.type)

				if (msg.type === 'start') {
					this.resetCurrentTurnMetrics()
					this.currentTurnStartedAt = Date.now()
					this.queuedAudioChunks = []
					this.queuedAudioBytes = 0
					this.pendingStopAfterGeminiReady = false
					this.pendingActivityStartAfterGeminiReady = true
					this.ensureGeminiSession()
					if (this.gemini.isReady) {
						this.pendingActivityStartAfterGeminiReady = false
						this.sendActivityStartToGemini()
					}
				}

				// Forward tool response to Gemini
				if (msg.type === 'tool_response' && this.gemini.isReady) {
					const pending = this.pendingDeviceCalls.get(msg.id)
					if (!pending) {
						console.warn(`[Bridge] Ignoring stale tool response: ${msg.name} (${msg.id})`)
						return
					}
					console.log(`[Bridge] Tool response: ${msg.name} → Gemini`)
					this.gemini.send(
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
						}),
					)
					this.pendingDeviceCalls.delete(msg.id)
					this.logToolCall({
						name: pending.name,
						args: pending.args,
						result: msg.result,
						handledBy: 'device',
						durationMs: Date.now() - pending.startMs,
					}).catch(() => {})
				}

				// Forward text input to Gemini
				if (msg.type === 'text' && msg.content && this.gemini.isReady) {
					this.sendActivityStartToGemini()
					this.gemini.send(buildGeminiRealtimeTextPayload(msg.content))
					this.sendActivityEndToGemini()
				}

				if (msg.type === 'stop') {
					if (!this.gemini.isReady) {
						this.pendingStopAfterGeminiReady = true
						this.ensureGeminiSession()
						return
					}
					this.handleStopSignal()
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
			if (this.pendingInitialHistoryTurns.length > 0 && this.gemini.isConnected) {
				this.gemini.send(
					JSON.stringify({
						clientContent: {
							turns: this.pendingInitialHistoryTurns,
							turnComplete: true,
						},
					}),
				)
				console.log(`[Gemini] Seeded ${this.pendingInitialHistoryTurns.length} history turns`)
				this.pendingInitialHistoryTurns = []
			}
			if (this.pendingStopAfterGeminiReady) {
				const ignoreReason = this.getIgnoredTurnReason()
				if (ignoreReason) {
					this.pendingStopAfterGeminiReady = false
					this.pendingActivityStartAfterGeminiReady = false
					this.handleStopSignal()
					return
				}
			}
			if (this.pendingActivityStartAfterGeminiReady) {
				this.pendingActivityStartAfterGeminiReady = false
				this.sendActivityStartToGemini()
			}
			this.sendToDevice({ type: 'ready' })
			this.flushQueuedAudioToGemini()
			if (this.pendingStopAfterGeminiReady) {
				this.pendingStopAfterGeminiReady = false
				this.handleStopSignal()
			}
			return
		}

		if (msg.serverContent) {
			const sc = msg.serverContent

			// Model audio — decode base64 and forward as raw binary
			if (sc.modelTurn?.parts) {
				for (const part of sc.modelTurn.parts) {
					if (part.inlineData?.data) {
						const raw = base64ToArrayBuffer(part.inlineData.data)
						this.currentTurnModelAudioBytes += raw.byteLength
						this.deviceWs?.send(raw)
					}
				}
			}

			// User interrupted — tell the device to drop any buffered model audio
			// from the turn Gemini was generating.
			if (sc.interrupted) {
				this.sendToDevice({ type: 'drop_audio' })
			}

			// Transcriptions - accumulate append-only text for captions and DB storage.
			if (sc.inputTranscription?.text) {
				const next = appendTranscriptDelta(this.currentUserText, sc.inputTranscription.text)
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
				const next = appendTranscriptDelta(this.currentAssistantText, sc.outputTranscription.text)
				this.currentAssistantText = next.text
				if (next.delta) {
					this.maybeSendReactiveFaceEmotion(next.delta)
					this.sendToDevice({
						type: 'transcript',
						source: 'model',
						text: next.delta,
					})
				}
			}

			// Turn complete — save exchange to D1 after consuming all parts in this event.
			if (sc.turnComplete) {
				const userForFeedback = this.currentUserText.trim()
				const hadModelText = this.currentAssistantText.trim().length > 0
				const hadAudio = this.currentTurnModelAudioBytes > 0
				const hadToolActivity = this.currentTurnHadToolActivity
				this.sendToDevice({ type: 'turn_complete' })
				await this.commitExchange()
				if (userForFeedback && (hadAudio || hadModelText || hadToolActivity)) {
					this.maybeSendTurnFeedback(userForFeedback, true)
				}
				this.currentTurnModelAudioBytes = 0
				this.currentTurnHadToolActivity = false
			}
			if (sc.turnComplete && this.pendingReconnectAfterTurn) {
				this.pendingReconnectAfterTurn = false
				await this.reconnectGeminiSession()
			}
		}

		if (msg.toolCallCancellation?.ids?.length) {
			for (const id of msg.toolCallCancellation.ids) {
				const pending = this.pendingDeviceCalls.get(id)
				if (!pending) continue
				this.pendingDeviceCalls.delete(id)
				await this.logToolCall({
					name: pending.name,
					args: pending.args,
					handledBy: 'device',
					status: 'error',
					error: 'tool call canceled by Gemini',
					durationMs: Date.now() - pending.startMs,
				})
			}
		}

		if (msg.sessionResumptionUpdate) {
			const update = msg.sessionResumptionUpdate
			if (update.resumable && update.newHandle) {
				this.sessionResumptionHandle = update.newHandle
				await this.saveSessionResumptionHandle(update.newHandle)
			}
		}

		if (msg.goAway) {
			console.log('[Gemini] GoAway received; will reconnect after current turn')
			this.pendingReconnectAfterTurn = true
		}

		if (msg.toolCall) {
			this.currentTurnHadToolActivity = true
			for (const call of msg.toolCall.functionCalls) {
				console.log(`[Gemini] Tool call: ${call.name}(${JSON.stringify(call.args)})`)
				const startMs = Date.now()

				if (call.name === 'search_docs') {
					const result = await handleDocsSearchTool(this.env, call.args)
					const payload = buildToolResponsePayload(call.name, call.id, result.response)
					console.log(`[Gemini] Sending tool response: ${payload.length} bytes`)

					if (this.gemini.send(payload)) {
						console.log(`[Gemini] Tool response sent`)
					}
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: result.logResult,
						handledBy: 'server',
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'search_learning_resources') {
					const args = call.args as {
						query?: string
						limit?: number
						source?: 'worksheet' | 'roleplay' | 'graded_reader'
					}
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
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												results,
												instruction:
													'If the user asked to start/practice a specific lesson now, immediately call load_learning_resource with the best matching resource_id. Otherwise ask which lesson they want.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: { count: results.length, titles: results.map((r) => r.title) },
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{ name: call.name, id: call.id, response: { error: message } },
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							handledBy: 'server',
							status: 'error',
							error: message,
							durationMs: Date.now() - startMs,
						})
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
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												loaded: true,
												resource_id: context.resourceId,
												title: context.title,
												tutor_mode: context.tutorMode,
												prompt_context: context.promptContext,
												instruction:
													context.source === 'roleplay'
														? 'ROLEPLAY MODE IS ACTIVE NOW. Speak German-only and stay in character. Speak first with the first in-character German line/question.'
														: 'Start the lesson now. Ask one short question at a time. You may call show_text with the title if helpful.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: {
								resourceId: context.resourceId,
								title: context.title,
								chars: context.promptContext.length,
								truncated: context.metadata.truncated,
							},
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: { loaded: false, error: message },
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							handledBy: 'server',
							status: 'error',
							error: message,
							durationMs: Date.now() - startMs,
						})
					}
				} else if (call.name === 'search_reader_passages') {
					const args = call.args as { resource_id?: string; query?: string; limit?: number }
					try {
						const resourceId =
							args.resource_id ||
							(this.activeLearningContext?.source === 'graded_reader'
								? this.activeLearningContext.resourceId
								: '')
						const response = await searchReaderPassages({
							env: this.env,
							deviceId: this.deviceId,
							resourceId,
							query: args.query || '',
							limit: args.limit || 5,
						})
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
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
												instruction:
													'Tell the user the best matching quote briefly. If they want to hear/read/discuss that part, call load_reader_passage with the passage_index and sentence_index.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: { count: response.results.length, top: response.results[0]?.quote },
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{ name: call.name, id: call.id, response: { error: message } },
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							handledBy: 'server',
							status: 'error',
							error: message,
							durationMs: Date.now() - startMs,
						})
					}
				} else if (call.name === 'load_reader_passage') {
					const args = call.args as {
						resource_id?: string
						passage_index?: number
						sentence_index?: number
						max_chars?: number
					}
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
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												loaded: true,
												resource_id: context.resourceId,
												title: context.title,
												passage_index: context.metadata.loadedPassageIndex,
												sentence_index: context.metadata.loadedSentenceIndex,
												prompt_context: context.promptContext,
												instruction:
													'Read or discuss this focused passage window. Start by saying you found the part, then quote or read a short section.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: {
								resourceId: context.resourceId,
								passageIndex: context.metadata.loadedPassageIndex,
								chars: context.promptContext.length,
							},
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: { loaded: false, error: message },
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							handledBy: 'server',
							status: 'error',
							error: message,
							durationMs: Date.now() - startMs,
						})
					}
				} else if (call.name === 'get_current_learning_resource') {
					this.gemini.send(
						JSON.stringify({
							toolResponse: {
								functionResponses: [
									{
										name: call.name,
										id: call.id,
										response: this.activeLearningContext
											? {
													active: true,
													resource_id: this.activeLearningContext.resourceId,
													title: this.activeLearningContext.title,
													tutor_mode: this.activeLearningContext.tutorMode,
													prompt_context: this.activeLearningContext.promptContext.slice(0, 4000),
												}
											: { active: false },
									},
								],
							},
						}),
					)
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: this.activeLearningContext
							? {
									resourceId: this.activeLearningContext.resourceId,
									title: this.activeLearningContext.title,
								}
							: { active: false },
						handledBy: 'server',
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'clear_learning_resource') {
					const previous = this.activeLearningContext?.resourceId
					this.activeLearningContext = null
					await this.state.storage.delete('activeLearningContext')
					this.gemini.send(
						JSON.stringify({
							toolResponse: {
								functionResponses: [
									{
										name: call.name,
										id: call.id,
										response: { cleared: true, previous_resource_id: previous },
									},
								],
							},
						}),
					)
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: { cleared: true, previous },
						handledBy: 'server',
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'save_flashcard') {
					const args = call.args as {
						front?: string
						back?: string
						tags?: string[]
						source?: string
						note?: string
					}
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
							source:
								args.source ??
								(this.activeLearningContext
									? `${this.activeLearningContext.source}:${this.activeLearningContext.resourceId}`
									: undefined),
							note: args.note,
						})
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												saved: result.saved !== false,
												id: result.id,
												instruction:
													'Briefly confirm to the learner what you saved. Do not repeat the full card multiple times.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: { id: result.id, saved: result.saved !== false },
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: { saved: false, error: message },
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							handledBy: 'server',
							status: 'error',
							error: message,
							durationMs: Date.now() - startMs,
						})
					}
				} else if (call.name === 'end_practice_review') {
					try {
						await this.commitExchange()
						const review = await this.generatePracticeReview()
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												...review,
												instruction:
													'Read this end-of-practice review aloud concisely. Use a warm tutor voice. Do not add extra mistakes beyond the transcript-based review.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: { grade: review.grade, mistakeCount: review.mistakes.length },
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					} catch (err) {
						const message = err instanceof Error ? err.message : String(err)
						this.gemini.send(
							JSON.stringify({
								toolResponse: {
									functionResponses: [
										{
											name: call.name,
											id: call.id,
											response: {
												grade: 'not enough evidence',
												overall: 'I could not generate a transcript-based review this time.',
												mistakes: [],
												next_practice: 'Try one more short roleplay and ask for a review again.',
												error: message,
												instruction:
													'Briefly apologize and give this fallback review. Do not invent specific mistakes.',
											},
										},
									],
								},
							}),
						)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							handledBy: 'server',
							status: 'error',
							error: message,
							durationMs: Date.now() - startMs,
						})
					}
				} else if (call.name === 'web_fetch' || call.name === 'fetch_url') {
					const args = call.args as WebFetchArgs
					const url = args.url || ''
					console.log(`[Gemini] Fetching: ${url}`)
					const result = await fetchWebPage(url, args.max_chars)
					const payload = buildToolResponsePayload(call.name, call.id, result)
					if (this.gemini.send(payload)) {
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
						call.args,
					)
					const payload = buildToolResponsePayload(call.name, call.id, response)
					this.gemini.send(payload)
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: response,
						handledBy: 'server',
						status: 'error' in (response as Record<string, unknown>) ? 'error' : 'ok',
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'set_voice') {
					const result = handleSetVoiceTool(call.args)
					if (!result.ok) {
						const payload = buildToolResponsePayload(call.name, call.id, result.response)
						this.gemini.send(payload)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: result.logResult,
							handledBy: 'server',
							status: result.status,
							durationMs: Date.now() - startMs,
						})
					} else {
						console.log(`[Gemini] Switching voice → ${result.voice}`)
						this.currentVoice = result.voice
						const payload = buildToolResponsePayload(call.name, call.id, result.response)
						this.gemini.send(payload)
						this.sendToDevice({ type: 'voice_changed', voice: result.voice })
						await this.clearSessionResumptionHandle()
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: result.logResult,
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
						this.gemini.close()
						await this.connectGemini()
					}
				} else if (call.name === 'set_thinking_level') {
					const requested = (call.args as { level?: unknown }).level
					const level = resolveThinkingLevel(requested)
					if (!level) {
						const payload = buildToolResponsePayload(call.name, call.id, {
							result: `Unknown thinking level "${String(requested ?? '')}". Available: ${THINKING_LEVEL_LIST_TEXT}.`,
						})
						this.gemini.send(payload)
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: `unknown thinking level: ${String(requested ?? '')}`,
							handledBy: 'server',
							status: 'error',
							durationMs: Date.now() - startMs,
						})
					} else {
						this.currentThinkingLevel = level
						await this.saveThinkingLevelForChat(level)
						await this.clearSessionResumptionHandle()
						const payload = buildToolResponsePayload(call.name, call.id, {
							result: `Thinking level set to ${level}. It will apply on the next turn; new conversations still start at ${DEFAULT_THINKING_LEVEL}.`,
						})
						this.gemini.send(payload)
						this.pendingReconnectAfterTurn = true
						await this.logToolCall({
							name: call.name,
							args: call.args,
							result: level,
							handledBy: 'server',
							durationMs: Date.now() - startMs,
						})
					}
				} else if (call.name === 'email_me') {
					const result = await handleEmailTool(this.env, call.args)
					const payload = buildToolResponsePayload(call.name, call.id, result.response)
					this.gemini.send(payload)
					await this.logToolCall({
						name: call.name,
						args: result.logArgs,
						result: result.logResult,
						handledBy: 'server',
						status: result.status,
						error: result.error,
						durationMs: Date.now() - startMs,
					})
				} else if (call.name === 'show_image') {
					const prompt = imagePromptFromArgs(call.args)
					if (!prompt) {
						const payload = buildToolResponsePayload(call.name, call.id, {
							result: 'no prompt provided',
						})
						this.gemini.send(payload)
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
						const ackPayload = buildToolResponsePayload(call.name, call.id, {
							result: 'image generation started; it will appear on screen shortly',
						})
						this.gemini.send(ackPayload)
						// Tell the device an image is coming so it can show the pulse animation.
						this.sendToDevice({ type: 'show_image_pending' })
						// Run the pipeline in the background and push the result when ready.
						generateAndSendImage({
							prompt,
							geminiApiKey: this.env.GEMINI_API_KEY,
							db: this.env.DB,
							storage: this.env.STORAGE,
							deviceId: this.deviceId,
							chatId: this.chatId,
							targetWidth: this.imageTargetWidth,
							targetHeight: this.imageTargetHeight,
							toolName: call.name,
							toolArgs: call.args,
							startMs,
							sendToDevice: (msg) => this.sendToDevice(msg),
							logToolCall: (entry) => this.logToolCall(entry),
						}).catch((err) => {
							console.error('[ImageGen] Background generation failed:', err)
						})
					}
				} else if (
					call.name === 'list_recent_images' ||
					call.name === 'search_images' ||
					call.name === 'show_saved_image'
				) {
					const response = await handleImageTool({
						name: call.name,
						args: call.args,
						db: this.env.DB,
						deviceId: this.deviceId,
						sendToDevice: (msg) => this.sendToDevice(msg),
					})
					const payload = buildToolResponsePayload(call.name, call.id, response)
					this.gemini.send(payload)
					await this.logToolCall({
						name: call.name,
						args: call.args,
						result: response,
						handledBy: 'server',
						status: 'error' in (response as Record<string, unknown>) ? 'error' : 'ok',
						durationMs: Date.now() - startMs,
					})
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
					await this.clearSessionResumptionHandle()
					this.chatId = crypto.randomUUID()
					this.currentThinkingLevel = DEFAULT_THINKING_LEVEL
					this.sessionResumptionHandle = null
					this.pendingInitialHistoryTurns = []
					this.currentUserText = ''
					this.currentAssistantText = ''
					this.sendToDevice({
						type: 'session',
						chatId: this.chatId,
						reset: true,
					})
					this.gemini.close()
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

	private async handleFaceControl(request: Request): Promise<Response> {
		let body: {
			emotion?: unknown
			look_x?: unknown
			look_y?: unknown
			eye_spacing?: unknown
			anim_speed?: unknown
		}
		try {
			body = await request.json()
		} catch {
			return new Response('Invalid JSON', { status: 400 })
		}

		const emotion = typeof body.emotion === 'string' ? body.emotion : 'default'
		const normalized = ['default', 'angry', 'focused', 'eepy', 'thinking'].includes(emotion)
			? emotion
			: 'default'
		const lookX = typeof body.look_x === 'number' ? Math.max(-1, Math.min(1, body.look_x)) : 0
		const lookY = typeof body.look_y === 'number' ? Math.max(-1, Math.min(1, body.look_y)) : 0
		const eyeSpacing =
			typeof body.eye_spacing === 'number' ? Math.max(36, Math.min(70, body.eye_spacing)) : 52
		const animSpeed =
			typeof body.anim_speed === 'number' ? Math.max(0.25, Math.min(3, body.anim_speed)) : 1
		this.sendToDevice({
			type: 'face_control',
			emotion: normalized,
			look_x: lookX,
			look_y: lookY,
			eye_spacing: eyeSpacing,
			anim_speed: animSpeed,
		})
		return new Response(
			JSON.stringify({
				ok: true,
				device_connected: !!this.deviceWs,
				emotion: normalized,
				look_x: lookX,
				look_y: lookY,
				eye_spacing: eyeSpacing,
				anim_speed: animSpeed,
			}),
			{
				headers: { 'Content-Type': 'application/json', 'Access-Control-Allow-Origin': '*' },
			},
		)
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

	private async getUserInstructionsForPrompt(): Promise<string> {
		try {
			const file = await ensureUserInstructionsFile(this.env.DB, this.deviceId)
			console.log(`[Files] Loaded ${USER_INSTRUCTIONS_PATH}: ${file.content.length} bytes`)
			return file.content
		} catch (err) {
			console.error(`[Files] Failed to load ${USER_INSTRUCTIONS_PATH}:`, err)
			return ''
		}
	}

	private storageKey(kind: 'thinking' | 'resumption', chatId = this.chatId): string {
		return `${kind}:${this.deviceId}:${chatId}`
	}

	private async loadThinkingLevelForChat(chatId = this.chatId): Promise<ThinkingLevel> {
		try {
			const saved = await this.state.storage.get<string>(this.storageKey('thinking', chatId))
			return resolveThinkingLevel(saved) ?? DEFAULT_THINKING_LEVEL
		} catch (err) {
			console.error('[Session] Failed to load thinking level:', err)
			return DEFAULT_THINKING_LEVEL
		}
	}

	private async saveThinkingLevelForChat(level: ThinkingLevel): Promise<void> {
		try {
			await this.state.storage.put(this.storageKey('thinking'), level)
		} catch (err) {
			console.error('[Session] Failed to save thinking level:', err)
		}
	}

	private async loadSessionResumptionHandle(): Promise<string | null> {
		try {
			const handle = await this.state.storage.get<string>(this.storageKey('resumption'))
			return typeof handle === 'string' && handle ? handle : null
		} catch (err) {
			console.error('[Session] Failed to load session handle:', err)
			return null
		}
	}

	private async saveSessionResumptionHandle(handle: string): Promise<void> {
		try {
			await this.state.storage.put(this.storageKey('resumption'), handle)
		} catch (err) {
			console.error('[Session] Failed to save session handle:', err)
		}
	}

	private async clearSessionResumptionHandle(chatId = this.chatId): Promise<void> {
		this.sessionResumptionHandle = null
		try {
			await this.state.storage.delete(this.storageKey('resumption', chatId))
		} catch (err) {
			console.error('[Session] Failed to clear session handle:', err)
		}
	}

	private async getConversationHistoryForPrompt(): Promise<GeminiContentTurn[]> {
		if (!this.chatId || !this.deviceId) return []

		try {
			const row = await this.env.DB.prepare(
				`SELECT messages
				 FROM conversations
				 WHERE chat_id = ? AND device_id = ?`,
			)
				.bind(this.chatId, this.deviceId)
				.first<{ messages: string }>()

			if (!row?.messages) return []
			const parsed = JSON.parse(row.messages) as unknown
			if (!Array.isArray(parsed)) return []

			const messages = parsed
				.filter((message): message is ConversationMessage => {
					return (
						typeof message === 'object' &&
						message !== null &&
						((message as ConversationMessage).role === 'user' ||
							(message as ConversationMessage).role === 'assistant') &&
						typeof (message as ConversationMessage).content === 'string'
					)
				})
				.slice(-12)

			if (messages.length === 0) return []

			return messages
				.map((message) => {
					const content = message.content.replace(/\s+/g, ' ').trim()
					return {
						role: message.role === 'user' ? 'user' : 'model',
						parts: [{ text: content.slice(0, 1200) }],
					} satisfies GeminiContentTurn
				})
				.filter((turn) => turn.parts[0]?.text)
		} catch (err) {
			console.error('[DB] Failed to load conversation context:', err)
			return []
		}
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

	private resetCurrentTurnMetrics() {
		this.audioChunkCount = 0
		this.currentTurnAudioBytes = 0
		this.currentTurnAbsSum = 0
		this.currentTurnSamples = 0
		this.currentTurnModelAudioBytes = 0
		this.currentTurnHadToolActivity = false
		this.currentTurnStartedAt = 0
		this.currentTurnDebugAudioChunks = []
		this.currentTurnDebugAudioBytes = 0
		this.currentTurnDebugDroppedBytes = 0
	}

	private async reconnectGeminiSession(options: { clearResumptionHandle?: boolean } = {}) {
		this.resetCurrentTurnMetrics()
		this.queuedAudioChunks = []
		this.queuedAudioBytes = 0
		this.pendingStopAfterGeminiReady = false
		this.pendingActivityStartAfterGeminiReady = false
		this.activityOpen = false
		if (options.clearResumptionHandle) {
			await this.clearSessionResumptionHandle()
		}
		this.gemini.close()
		await this.connectGemini()
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
		this.queuedAudioChunks = []
		this.queuedAudioBytes = 0
		this.pendingStopAfterGeminiReady = false
		this.pendingActivityStartAfterGeminiReady = false
		this.pendingReconnectAfterTurn = false
		this.pendingInitialHistoryTurns = []
		this.activityOpen = false

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

function parseVoiceMode(value: string | null): VoiceMode {
	return value === 'quiz_masters' ? 'quiz_masters' : 'assistant'
}

function parseBooleanFlag(value: string | undefined): boolean {
	if (!value) return false
	return ['1', 'true', 'yes', 'on'].includes(value.trim().toLowerCase())
}

function reactiveEmotionForTranscript(text: string): 'angry' | 'eepy' | null {
	const lower = text.toLowerCase()
	if (/\b(angry|mad|furious|annoyed|frustrated|wütend|sauer)\b/i.test(lower)) {
		return 'angry'
	}
	if (/\b(confused|don't understand|dont understand|ich verstehe nicht|keine ahnung)\b/i.test(lower)) {
		return 'eepy'
	}
	return null
}

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
	const body = (await resp.json()) as {
		candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>
	}
	const raw = body.candidates?.[0]?.content?.parts?.map((p) => p.text ?? '').join('').trim() ?? ''
	if (!raw) throw new Error('turn feedback model returned empty response')
	return normalizeTurnFeedback(JSON.parse(stripJsonFence(raw)))
}

function normalizeTurnFeedback(value: unknown): TurnFeedback {
	const obj = value && typeof value === 'object' ? (value as Record<string, unknown>) : {}
	const rawColor = typeof obj.color === 'string' ? obj.color.toLowerCase() : 'gray'
	const color: TurnFeedback['color'] =
		rawColor === 'green' || rawColor === 'yellow' || rawColor === 'red' || rawColor === 'gray'
			? rawColor
			: 'gray'
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
	const body = (await resp.json()) as {
		candidates?: Array<{ content?: { parts?: Array<{ text?: string }> } }>
	}
	const text = body.candidates?.[0]?.content?.parts?.map((p) => p.text ?? '').join('').trim() ?? ''
	if (!text) throw new Error('review model returned empty response')
	return normalizePracticeReview(JSON.parse(stripJsonFence(text)))
}

function normalizePracticeReview(value: unknown): PracticeReview {
	const obj = value && typeof value === 'object' ? (value as Record<string, unknown>) : {}
	const mistakesRaw = Array.isArray(obj.mistakes) ? obj.mistakes : []
	return {
		grade:
			typeof obj.grade === 'string' && obj.grade.trim()
				? obj.grade.trim().slice(0, 32)
				: 'not enough evidence',
		overall: typeof obj.overall === 'string' ? obj.overall.trim().slice(0, 500) : '',
		mistakes: mistakesRaw
			.slice(0, 3)
			.map((m) => {
				const item = m && typeof m === 'object' ? (m as Record<string, unknown>) : {}
				return {
					learner_said:
						typeof item.learner_said === 'string'
							? item.learner_said.trim().slice(0, 250)
							: '',
					corrected:
						typeof item.corrected === 'string' ? item.corrected.trim().slice(0, 250) : '',
					explanation:
						typeof item.explanation === 'string'
							? item.explanation.trim().slice(0, 300)
							: '',
				}
			})
			.filter((m) => m.learner_said || m.corrected || m.explanation),
		next_practice:
			typeof obj.next_practice === 'string' ? obj.next_practice.trim().slice(0, 400) : '',
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

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64)
	const bytes = new Uint8Array(binary.length)
	for (let i = 0; i < binary.length; i++) {
		bytes[i] = binary.charCodeAt(i)
	}
	return bytes.buffer
}
