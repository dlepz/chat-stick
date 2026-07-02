import { LiveSession } from './live-session'
import { indexDocs, vectorSearch } from './docs-search'
import { gradeFlashcard, listInboxFlashcards, loadLearningResourceContext, saveFlashcard, searchLearningResources, searchReaderPassages } from './flashcard-api'

export { LiveSession }

export interface Env {
	LIVE_SESSION: DurableObjectNamespace
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	HISTORY_API_TOKEN: string
	OTA_BUCKET?: R2Bucket
	FIRMWARE_LATEST_VERSION?: string
	FIRMWARE_NOTES?: string
	FIRMWARE_OBJECT_KEY?: string
	FLASHCARD_APP_BASE_URL?: string
	FLASHCARD_APP_BRIDGE_TOKEN?: string
	CHAT_STICK_LINK_TOKEN?: string
	GEMINI_LIVE_API_VERSION?: string
	GEMINI_ENABLE_AFFECTIVE_DIALOG?: string
}

export default {
	async fetch(request: Request, env: Env): Promise<Response> {
		const url = new URL(request.url)

		if (request.method === 'OPTIONS') {
			return new Response(null, { headers: corsHeaders() })
		}

		switch (url.pathname) {
			case '/device/face-control': {
				if (request.method !== 'POST') return new Response('Method not allowed', { status: 405, headers: corsHeaders() })
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

				// Route to DO by device_id (one session per device)
				const deviceId = url.searchParams.get('device_id') || 'unknown'
				const id = env.LIVE_SESSION.idFromName(deviceId)
				const stub = env.LIVE_SESSION.get(id)
				// Forward the full URL so DO can read device_id and chat_id
				return stub.fetch(request)
			}

			// Admin: index docs into Vectorize
			case '/admin/index':
				return indexDocs(env)

			// Admin: test vector search
			case '/admin/search': {
				const q = url.searchParams.get('q') || 'hello'
				return vectorSearch(q, env)
			}

			case '/device/learning-resources': {
				const deviceId = url.searchParams.get('device_id') || ''
				if (!deviceId) return new Response('Missing device_id', { status: 400 })
				const q = url.searchParams.get('q') || 'available German lessons and readers'
				const source = url.searchParams.get('source') as 'worksheet' | 'roleplay' | 'graded_reader' | null
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
				const deviceId = url.searchParams.get('device_id') || ''
				if (!deviceId) return new Response('Missing device_id', { status: 400 })
				const modeParam = url.searchParams.get('mode')
				const mode = modeParam === 'all' ? 'all' : 'due'
				const limit = Number(url.searchParams.get('limit') || 20)
				const response = await listInboxFlashcards({ env, deviceId, mode, limit })
				return json(response)
			}

			case '/device/flashcards/save': {
				if (request.method !== 'POST') {
					return new Response('Method not allowed', { status: 405, headers: corsHeaders() })
				}
				let body: { device_id?: unknown; chat_id?: unknown; front?: unknown; back?: unknown; tags?: unknown; source?: unknown; note?: unknown }
				try {
					body = (await request.json()) as typeof body
				} catch {
					return new Response('Invalid JSON', { status: 400, headers: corsHeaders() })
				}
				const deviceId = typeof body.device_id === 'string' ? body.device_id : ''
				const front = typeof body.front === 'string' ? body.front.trim() : ''
				const back = typeof body.back === 'string' ? body.back.trim() : ''
				const chatId = typeof body.chat_id === 'string' ? body.chat_id : undefined
				const tags = Array.isArray(body.tags) ? body.tags.filter((tag): tag is string => typeof tag === 'string') : undefined
				const source = typeof body.source === 'string' ? body.source : undefined
				const note = typeof body.note === 'string' ? body.note : undefined
				if (!deviceId || !front || !back) {
					return new Response('Missing device_id, front, or back', { status: 400, headers: corsHeaders() })
				}
				return json(await saveFlashcard({ env, deviceId, chatId, front, back, tags, source, note }))
			}

			case '/device/flashcards/grade': {
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
					return new Response('Missing device_id, card_id, or grade', { status: 400, headers: corsHeaders() })
				}
				return json(await gradeFlashcard({ env, deviceId, cardId, grade }))
			}

			case '/admin/learning-search': {
				if (!isAuthorizedHistoryRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const q = url.searchParams.get('q') || ''
				const deviceId = url.searchParams.get('device_id') || ''
				if (!q || !deviceId) return new Response('Missing q or device_id', { status: 400 })
				return json(await searchLearningResources({ env, deviceId, query: q, limit: Number(url.searchParams.get('limit') || 5) }))
			}

			case '/admin/learning-context': {
				if (!isAuthorizedHistoryRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const resourceId = url.searchParams.get('resource_id') || ''
				const deviceId = url.searchParams.get('device_id') || ''
				if (!resourceId || !deviceId) return new Response('Missing resource_id or device_id', { status: 400 })
				const passageIndex = url.searchParams.get('passage_index')
				const sentenceIndex = url.searchParams.get('sentence_index')
				return json(await loadLearningResourceContext({
					env,
					deviceId,
					resourceId,
					maxChars: Number(url.searchParams.get('max_chars') || 8000),
					passageIndex: passageIndex == null ? undefined : Number(passageIndex),
					sentenceIndex: sentenceIndex == null ? undefined : Number(sentenceIndex),
				}))
			}

			case '/admin/reader-passage-search': {
				if (!isAuthorizedHistoryRequest(request, env)) {
					return new Response('Unauthorized', { status: 401, headers: corsHeaders() })
				}
				const resourceId = url.searchParams.get('resource_id') || ''
				const deviceId = url.searchParams.get('device_id') || ''
				const q = url.searchParams.get('q') || ''
				if (!resourceId || !deviceId || !q) return new Response('Missing resource_id, device_id, or q', { status: 400 })
				return json(await searchReaderPassages({ env, deviceId, resourceId, query: q, limit: Number(url.searchParams.get('limit') || 5) }))
			}

			case '/health':
				return new Response('ok')

			case '/firmware/check': {
				const currentVersion = Number(url.searchParams.get('version') || '0')
				const latestVersion = Number(env.FIRMWARE_LATEST_VERSION || currentVersion || 0)
				const hasDownload = !!env.OTA_BUCKET && !!env.FIRMWARE_OBJECT_KEY
				return json({
					available: hasDownload && latestVersion > currentVersion,
					latest_version: latestVersion,
					notes: env.FIRMWARE_NOTES || '',
					download_url:
						hasDownload && latestVersion > currentVersion
							? `${url.origin}/firmware/download`
							: '',
				})
			}

			case '/firmware/download': {
				if (!env.OTA_BUCKET || !env.FIRMWARE_OBJECT_KEY) {
					return new Response('Firmware download unavailable', { status: 404 })
				}

				const object = await env.OTA_BUCKET.get(env.FIRMWARE_OBJECT_KEY)
				if (!object) {
					return new Response('Firmware not found', { status: 404 })
				}

				const headers = new Headers()
				object.writeHttpMetadata(headers)
				headers.set('etag', object.httpEtag)
				headers.set(
					'content-disposition',
					`attachment; filename="${env.FIRMWARE_OBJECT_KEY.split('/').pop() || 'firmware.bin'}"`
				)
				return new Response(object.body, { headers })
			}

			default: {
				// /history/:deviceId — list recent conversations
				const historyMatch = url.pathname.match(/^\/history\/(.+)$/)
				if (historyMatch) {
					const deviceId = decodeURIComponent(historyMatch[1])
					const requestedDeviceId = url.searchParams.get('device_id') ?? ''
					const authorized =
						isAuthorizedHistoryRequest(request, env) ||
						(!!requestedDeviceId && requestedDeviceId === deviceId)
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
					const requestedDeviceId = url.searchParams.get('device_id') ?? ''
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
						(!!requestedDeviceId && requestedDeviceId === row.device_id)
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
		'Access-Control-Allow-Headers': 'Content-Type, X-History-Token',
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

function isAuthorizedHistoryRequest(request: Request, env: Env): boolean {
	const configuredToken = env.HISTORY_API_TOKEN?.trim()
	if (!configuredToken) return false

	const url = new URL(request.url)
	const providedToken =
		request.headers.get('X-History-Token') ??
		url.searchParams.get('token') ??
		''

	return providedToken === configuredToken
}
