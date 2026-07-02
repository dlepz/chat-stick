import { LiveSession } from './live-session'
import { indexDocs, vectorSearch } from './docs-search'
import {
	gradeFlashcard,
	listInboxFlashcards,
	loadLearningResourceContext,
	saveFlashcard,
	searchLearningResources,
	searchReaderPassages,
} from './flashcard-api'

export { LiveSession }

export interface Env {
	LIVE_SESSION: DurableObjectNamespace
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	HISTORY_API_TOKEN: string
	ADMIN_API_TOKEN?: string
	DEVICE_AUTH_TOKEN?: string
	STORAGE?: R2Bucket
	FLASHCARD_APP_BASE_URL?: string
	FLASHCARD_APP_BRIDGE_TOKEN?: string
	CHAT_STICK_LINK_TOKEN?: string
	CONVERSATION_END_REVIEW_ENABLED?: string
	TURN_FEEDBACK_ENABLED?: string
}

const LEGACY_FIRMWARE_PREFIX = 'chat-stick/firmware/'
const FIRMWARE_DEVICE_IDS = new Set(['m5-stick', 'waveshare'])

async function findLatestFirmware(
	env: Env,
	device: string
): Promise<{ version: number; key: string } | null> {
	if (!env.STORAGE) return null
	const prefixes =
		device === 'm5-stick'
			? [`chat-stick/firmware/${device}/`, LEGACY_FIRMWARE_PREFIX]
			: [`chat-stick/firmware/${device}/`]
	let latest: { version: number; key: string } | null = null
	for (const prefix of prefixes) {
		const list = await env.STORAGE.list({ prefix })
		for (const obj of list.objects) {
			const objectName = obj.key.slice(prefix.length)
			if (objectName.includes('/')) continue
			const match = objectName.match(/^firmware-v(\d+)\.bin$/)
			if (!match) continue
			const version = Number(match[1])
			if (!latest || version > latest.version) {
				latest = { version, key: obj.key }
			}
		}
	}
	return latest
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url)

		if (request.method === 'OPTIONS') {
			return new Response(null, { headers: corsHeaders() })
		}

		switch (url.pathname) {
			case '/device/face-control': {
				if (request.method !== 'POST') {
					return new Response('Method not allowed', { status: 405, headers: corsHeaders() })
				}
				const expectedToken = env.CHAT_STICK_LINK_TOKEN
				if (expectedToken && request.headers.get('Authorization') !== `Bearer ${expectedToken}`) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const deviceId = url.searchParams.get('device_id') || 'm5s3-live'
				const id = env.LIVE_SESSION.idFromName(deviceId)
				const stub = env.LIVE_SESSION.get(id)
				return stub.fetch(request)
			}

			case '/ws': {
				const upgrade = request.headers.get('Upgrade')
				if (upgrade !== 'websocket') {
					return new Response('Expected WebSocket', { status: 426 })
				}
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401 })
				}

				// Route to DO by device_id (one session per device)
				const deviceId = url.searchParams.get('device_id') || 'unknown'
				const id = env.LIVE_SESSION.idFromName(deviceId)
				const stub = env.LIVE_SESSION.get(id)
				// Forward the full URL so DO can read device_id and chat_id
				return stub.fetch(request)
			}

			// Admin: index docs into Vectorize
			case '/admin/index':
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				return indexDocs(env)

			// Admin: test vector search
			case '/admin/search': {
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const q = url.searchParams.get('q') || 'hello'
				return vectorSearch(q, env)
			}

			case '/health':
				return new Response('ok')

			case '/ping':
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				return new Response('pong', {
					headers: {
						...corsHeaders(),
						'Cache-Control': 'no-store',
						'Content-Type': 'text/plain',
					},
				})

			case '/device/learning-resources': {
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const deviceId = url.searchParams.get('device_id') || ''
				if (!deviceId) return new Response('Missing device_id', { status: 400 })
				const q = url.searchParams.get('q') || 'available German lessons and readers'
				const source = url.searchParams.get('source') as
					| 'worksheet'
					| 'roleplay'
					| 'graded_reader'
					| null
				const response = await searchLearningResources({
					env,
					deviceId,
					query: q,
					limit: Number(url.searchParams.get('limit') || 8),
					source: source || undefined,
				})
				return json({
					results: response.results.map((r) => ({
						resource_id: r.resourceId,
						title: r.title,
						subtitle: r.subtitle || r.source,
						source: r.source,
						level: r.level || '',
					})),
				})
			}

			case '/device/flashcards/inbox': {
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const deviceId = url.searchParams.get('device_id') || ''
				if (!deviceId) return new Response('Missing device_id', { status: 400 })
				const modeParam = url.searchParams.get('mode')
				const mode = modeParam === 'all' ? 'all' : 'due'
				const limit = Number(url.searchParams.get('limit') || 20)
				const response = await listInboxFlashcards({ env, deviceId, mode, limit })
				return json(response)
			}

			case '/device/flashcards/save': {
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				if (request.method !== 'POST') {
					return new Response('Method not allowed', { status: 405, headers: corsHeaders() })
				}
				let body: {
					device_id?: unknown
					chat_id?: unknown
					front?: unknown
					back?: unknown
					tags?: unknown
					source?: unknown
					note?: unknown
				}
				try {
					body = (await request.json()) as typeof body
				} catch {
					return new Response('Invalid JSON', { status: 400, headers: corsHeaders() })
				}
				const deviceId = typeof body.device_id === 'string' ? body.device_id : ''
				const front = typeof body.front === 'string' ? body.front.trim() : ''
				const back = typeof body.back === 'string' ? body.back.trim() : ''
				const chatId = typeof body.chat_id === 'string' ? body.chat_id : undefined
				const tags = Array.isArray(body.tags)
					? body.tags.filter((tag): tag is string => typeof tag === 'string')
					: undefined
				const source = typeof body.source === 'string' ? body.source : undefined
				const note = typeof body.note === 'string' ? body.note : undefined
				if (!deviceId || !front || !back) {
					return new Response('Missing device_id, front, or back', {
						status: 400,
						headers: corsHeaders(),
					})
				}
				return json(await saveFlashcard({ env, deviceId, chatId, front, back, tags, source, note }))
			}

			case '/device/flashcards/grade': {
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				if (request.method !== 'POST') {
					return new Response('Method not allowed', { status: 405, headers: corsHeaders() })
				}
				let body: { device_id?: unknown; card_id?: unknown; grade?: unknown }
				try {
					body = (await request.json()) as typeof body
				} catch {
					return new Response('Invalid JSON', { status: 400, headers: corsHeaders() })
				}
				const deviceId = typeof body.device_id === 'string' ? body.device_id : ''
				const cardId = typeof body.card_id === 'string' ? body.card_id : ''
				const grade = body.grade === 'again' || body.grade === 'good' ? body.grade : null
				if (!deviceId || !cardId || !grade) {
					return new Response('Missing device_id, card_id, or grade', {
						status: 400,
						headers: corsHeaders(),
					})
				}
				return json(await gradeFlashcard({ env, deviceId, cardId, grade }))
			}

			case '/admin/learning-search': {
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const q = url.searchParams.get('q') || ''
				const deviceId = url.searchParams.get('device_id') || ''
				if (!q || !deviceId) return new Response('Missing q or device_id', { status: 400 })
				return json(
					await searchLearningResources({
						env,
						deviceId,
						query: q,
						limit: Number(url.searchParams.get('limit') || 5),
					}),
				)
			}

			case '/admin/learning-context': {
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const resourceId = url.searchParams.get('resource_id') || ''
				const deviceId = url.searchParams.get('device_id') || ''
				if (!resourceId || !deviceId) {
					return new Response('Missing resource_id or device_id', { status: 400 })
				}
				const passageIndex = url.searchParams.get('passage_index')
				const sentenceIndex = url.searchParams.get('sentence_index')
				return json(
					await loadLearningResourceContext({
						env,
						deviceId,
						resourceId,
						maxChars: Number(url.searchParams.get('max_chars') || 8000),
						passageIndex: passageIndex == null ? undefined : Number(passageIndex),
						sentenceIndex: sentenceIndex == null ? undefined : Number(sentenceIndex),
					}),
				)
			}

			case '/admin/reader-passage-search': {
				if (!isAuthorizedAdminRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const resourceId = url.searchParams.get('resource_id') || ''
				const deviceId = url.searchParams.get('device_id') || ''
				const q = url.searchParams.get('q') || ''
				if (!resourceId || !deviceId || !q) {
					return new Response('Missing resource_id, device_id, or q', { status: 400 })
				}
				return json(
					await searchReaderPassages({
						env,
						deviceId,
						resourceId,
						query: q,
						limit: Number(url.searchParams.get('limit') || 5),
					}),
				)
			}

			case '/firmware/check': {
				if (!isAuthorizedDeviceRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const currentVersion = Number(url.searchParams.get('version') || '0')
				const device = resolveFirmwareDevice(url)
				const latest = await findLatestFirmware(env, device)
				const updateAvailable = !!latest && latest.version > currentVersion
				return json({
					available: updateAvailable,
					latest_version: latest?.version ?? currentVersion,
					notes: '',
					download_url: updateAvailable
						? `${url.origin}/firmware/download?device=${encodeURIComponent(device)}`
						: '',
				})
			}

			case '/firmware/download': {
				const device = resolveFirmwareDevice(url)
				const latest = await findLatestFirmware(env, device)
				if (!env.STORAGE || !latest) {
					return new Response('Firmware download unavailable', { status: 404 })
				}

				const object = await env.STORAGE.get(latest.key)
				if (!object) {
					return new Response('Firmware not found', { status: 404 })
				}

				const headers = new Headers()
				object.writeHttpMetadata(headers)
				headers.set('etag', object.httpEtag)
				headers.set('content-length', object.size.toString())
				headers.set('content-type', headers.get('content-type') || 'application/octet-stream')
				headers.set(
					'content-disposition',
					`attachment; filename="${latest.key.split('/').pop()}"`
				)
				return new Response(object.body, { headers })
			}

			default: {
				// /history/:deviceId — list recent conversations
				const historyMatch = url.pathname.match(/^\/history\/(.+)$/)
				if (historyMatch) {
					const deviceId = decodeURIComponent(historyMatch[1])
					const authorized =
						isAuthorizedHistoryRequest(request, env) ||
						isAuthenticatedDeviceRequest(request, env)
					if (!authorized) {
						return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
					}

					const rows = await env.DB.prepare(
						`SELECT chat_id, last_message, updated_at
						 FROM conversations
						 WHERE device_id = ? AND last_message IS NOT NULL
						 ORDER BY updated_at DESC
						 LIMIT 10`
					)
						.bind(deviceId)
						.all()
					return new Response(JSON.stringify(rows.results), {
						headers: { 'Content-Type': 'application/json' },
					})
				}

				const sessionMatch = url.pathname.match(/^\/session\/(.+)$/)
				if (sessionMatch) {
					const chatId = decodeURIComponent(sessionMatch[1])
					const row = await env.DB.prepare(
						`SELECT chat_id, device_id, last_message, updated_at
						 FROM conversations
						 WHERE chat_id = ?
						 LIMIT 1`
					)
						.bind(chatId)
						.first<{
							chat_id: string
							device_id: string
							last_message: string | null
							updated_at: string
						}>()

					if (!row) {
						return new Response('Not found', { status: 404, headers: corsHeaders() })
					}

					const authorized =
						isAuthorizedHistoryRequest(request, env) ||
						isAuthenticatedDeviceRequest(request, env)
					if (!authorized) {
						return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
					}

					return new Response(
						JSON.stringify({
							chat_id: row.chat_id,
							device_id: row.device_id,
							last_message: row.last_message,
							updated_at: row.updated_at,
						}),
						{
							headers: { ...corsHeaders(), 'Content-Type': 'application/json' },
						}
					)
				}
				return new Response('Not found', { status: 404 })
			}
		}
	},
}

function corsHeaders(): HeadersInit {
	return {
		'Access-Control-Allow-Origin': '*',
		'Access-Control-Allow-Methods': 'GET, POST, OPTIONS',
		'Access-Control-Allow-Headers': 'Authorization, Content-Type, X-Admin-Token, X-Device-Token, X-History-Token',
	}
}

function json(payload: unknown, init?: ResponseInit): Response {
	return new Response(JSON.stringify(payload), {
		...init,
		headers: {
			...corsHeaders(),
			'Content-Type': 'application/json',
			...(init?.headers || {}),
		},
	})
}

function resolveFirmwareDevice(url: URL): string {
	const requested = url.searchParams.get('device') || 'm5-stick'
	return FIRMWARE_DEVICE_IDS.has(requested) ? requested : 'm5-stick'
}

function isAuthorizedHistoryRequest(request: Request, env: Env): boolean {
	const configuredToken = env.HISTORY_API_TOKEN?.trim()
	if (!configuredToken) return false

	const providedToken = getRequestToken(request, {
		headerNames: ['X-History-Token'],
		queryNames: ['token'],
	})

	return secureTokenEquals(providedToken, configuredToken)
}

function isAuthorizedAdminRequest(request: Request, env: Env): boolean {
	const configuredToken = (env.ADMIN_API_TOKEN || env.HISTORY_API_TOKEN || '').trim()
	if (!configuredToken) return false

	const providedToken = getRequestToken(request, {
		headerNames: ['X-Admin-Token', 'X-History-Token'],
		queryNames: ['admin_token', 'token'],
	})

	return secureTokenEquals(providedToken, configuredToken)
}

function isAuthorizedDeviceRequest(request: Request, env: Env): boolean {
	const configuredToken = env.DEVICE_AUTH_TOKEN?.trim()
	if (!configuredToken) return true

	const providedToken = getRequestToken(request, {
		headerNames: ['X-Device-Token'],
		queryNames: ['device_token'],
	})

	return secureTokenEquals(providedToken, configuredToken)
}

function isAuthenticatedDeviceRequest(request: Request, env: Env): boolean {
	return Boolean(env.DEVICE_AUTH_TOKEN?.trim()) && isAuthorizedDeviceRequest(request, env)
}

function getRequestToken(
	request: Request,
	options: { headerNames: string[]; queryNames: string[] }
): string {
	for (const headerName of options.headerNames) {
		const value = request.headers.get(headerName)
		if (value) return value.trim()
	}

	const auth = request.headers.get('Authorization') || ''
	const bearer = auth.match(/^Bearer\s+(.+)$/i)?.[1]
	if (bearer) return bearer.trim()

	const url = new URL(request.url)
	for (const queryName of options.queryNames) {
		const value = url.searchParams.get(queryName)
		if (value) return value.trim()
	}

	return ''
}

function secureTokenEquals(providedToken: string, configuredToken: string): boolean {
	if (!providedToken || providedToken.length !== configuredToken.length) return false

	let diff = 0
	for (let i = 0; i < configuredToken.length; i++) {
		diff |= configuredToken.charCodeAt(i) ^ providedToken.charCodeAt(i)
	}
	return diff === 0
}
