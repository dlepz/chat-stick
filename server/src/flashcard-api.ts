import type { LearningResourceSource, LessonContextBundle, LoadLearningResourceContextResponse, SearchLearningResourcesResponse, SearchReaderPassagesResponse } from './learning-types'

export interface FlashcardApiEnv {
	FLASHCARD_APP_BASE_URL?: string
	FLASHCARD_APP_BRIDGE_TOKEN?: string
}

function requireConfig(env: FlashcardApiEnv): { baseUrl: string; token: string } {
	const baseUrl = env.FLASHCARD_APP_BASE_URL?.replace(/\/$/, '')
	const token = env.FLASHCARD_APP_BRIDGE_TOKEN
	if (!baseUrl) throw new Error('FLASHCARD_APP_BASE_URL is not configured')
	if (!token) throw new Error('FLASHCARD_APP_BRIDGE_TOKEN is not configured')
	return { baseUrl, token }
}

function parseResourceId(resourceId: string): { source: string; sourceId: string } {
	const [source, ...rest] = resourceId.split(':')
	const sourceId = rest.join(':')
	if (!source || !sourceId) throw new Error('Invalid resource_id')
	return { source, sourceId }
}

async function readJsonOrText(resp: Response): Promise<unknown> {
	const text = await resp.text()
	try {
		return JSON.parse(text)
	} catch {
		return { error: text }
	}
}

async function flashcardFetch<T>(env: FlashcardApiEnv, deviceId: string, path: string, params: Record<string, string>): Promise<T> {
	const { baseUrl, token } = requireConfig(env)
	const url = new URL(`${baseUrl}${path}`)
	for (const [key, value] of Object.entries(params)) url.searchParams.set(key, value)

	const resp = await fetch(url.toString(), {
		headers: {
			Authorization: `Bearer ${token}`,
			'X-Chat-Stick-Device-Id': deviceId,
			Accept: 'application/json',
		},
	})
	const body = await readJsonOrText(resp)
	if (!resp.ok) {
		const message = typeof body === 'object' && body && 'message' in body
			? String((body as { message?: unknown }).message)
			: typeof body === 'object' && body && 'error' in body
				? String((body as { error?: unknown }).error)
				: `HTTP ${resp.status}`
		throw new Error(`Flashcard app request failed (${resp.status}): ${message}`)
	}
	return body as T
}

async function flashcardPost<T>(env: FlashcardApiEnv, deviceId: string, path: string, body: unknown): Promise<T> {
	const { baseUrl, token } = requireConfig(env)
	const resp = await fetch(`${baseUrl}${path}`, {
		method: 'POST',
		headers: {
			Authorization: `Bearer ${token}`,
			'X-Chat-Stick-Device-Id': deviceId,
			Accept: 'application/json',
			'Content-Type': 'application/json',
		},
		body: JSON.stringify(body),
	})
	const parsed = await readJsonOrText(resp)
	if (!resp.ok) {
		const message = typeof parsed === 'object' && parsed && 'message' in parsed
			? String((parsed as { message?: unknown }).message)
			: typeof parsed === 'object' && parsed && 'error' in parsed
				? String((parsed as { error?: unknown }).error)
				: `HTTP ${resp.status}`
		throw new Error(`Flashcard app request failed (${resp.status}): ${message}`)
	}
	return parsed as T
}

export async function searchLearningResources(input: {
	env: FlashcardApiEnv
	deviceId: string
	query: string
	limit?: number
	source?: LearningResourceSource
}): Promise<SearchLearningResourcesResponse> {
	const params: Record<string, string> = {
		q: input.query,
		limit: String(input.limit ?? 5),
	}
	if (input.source) params.source = input.source
	return flashcardFetch<SearchLearningResourcesResponse>(input.env, input.deviceId, '/api/chat-stick/resources/search', params)
}

export async function loadLearningResourceContext(input: {
	env: FlashcardApiEnv
	deviceId: string
	resourceId: string
	maxChars?: number
	passageIndex?: number
	sentenceIndex?: number
}): Promise<LessonContextBundle> {
	const { source, sourceId } = parseResourceId(input.resourceId)
	const params: Record<string, string> = { max_chars: String(input.maxChars ?? 8000) }
	if (input.passageIndex != null) params.passage_index = String(input.passageIndex)
	if (input.sentenceIndex != null) params.sentence_index = String(input.sentenceIndex)
	const response = await flashcardFetch<LoadLearningResourceContextResponse>(
		input.env,
		input.deviceId,
		`/api/chat-stick/resources/${encodeURIComponent(source)}/${encodeURIComponent(sourceId)}/context`,
		params
	)
	return response.context
}

export async function searchReaderPassages(input: {
	env: FlashcardApiEnv
	deviceId: string
	resourceId: string
	query: string
	limit?: number
}): Promise<SearchReaderPassagesResponse> {
	const { source, sourceId } = parseResourceId(input.resourceId)
	return flashcardFetch<SearchReaderPassagesResponse>(
		input.env,
		input.deviceId,
		`/api/chat-stick/resources/${encodeURIComponent(source)}/${encodeURIComponent(sourceId)}/passages/search`,
		{ q: input.query, limit: String(input.limit ?? 5) }
	)
}

export interface SaveFlashcardInput {
	env: FlashcardApiEnv
	deviceId: string
	chatId?: string
	front: string
	back: string
	tags?: string[]
	source?: string
	note?: string
}

export interface SaveFlashcardResponse {
	id?: string
	saved?: boolean
	[key: string]: unknown
}

export async function saveFlashcard(input: SaveFlashcardInput): Promise<SaveFlashcardResponse> {
	return flashcardPost<SaveFlashcardResponse>(input.env, input.deviceId, '/api/chat-stick/flashcards', {
		device_id: input.deviceId,
		chat_id: input.chatId,
		front: input.front,
		back: input.back,
		tags: input.tags ?? [],
		source: input.source,
		note: input.note,
	})
}

export interface InboxFlashcardSummary {
	id: string
	front: string
	back: string
	tags?: string[]
	source?: string | null
	note?: string | null
	status?: string
	intervalDays?: number
	ease?: number
	nextReviewAt?: string | null
	lastReviewedAt?: string | null
	lapseCount?: number
	reviewCount?: number
	createdAt?: string
	[key: string]: unknown
}

export interface ListInboxFlashcardsResponse {
	cards: InboxFlashcardSummary[]
	counts: { due: number; total: number }
	mode: 'due' | 'all'
	limit: number
}

export async function listInboxFlashcards(input: {
	env: FlashcardApiEnv
	deviceId: string
	mode?: 'due' | 'all'
	limit?: number
}): Promise<ListInboxFlashcardsResponse> {
	const params: Record<string, string> = {
		mode: input.mode ?? 'due',
		limit: String(input.limit ?? 20),
	}
	return flashcardFetch<ListInboxFlashcardsResponse>(input.env, input.deviceId, '/api/chat-stick/flashcards', params)
}

export interface GradeFlashcardResponse {
	graded?: boolean
	card?: InboxFlashcardSummary
	[key: string]: unknown
}

export async function gradeFlashcard(input: {
	env: FlashcardApiEnv
	deviceId: string
	cardId: string
	grade: 'again' | 'good'
}): Promise<GradeFlashcardResponse> {
	return flashcardPost<GradeFlashcardResponse>(
		input.env,
		input.deviceId,
		`/api/chat-stick/flashcards/${encodeURIComponent(input.cardId)}/grade`,
		{ grade: input.grade }
	)
}

/* old load signature kept above */
export async function loadLearningResourceContextLegacy(input: {
	env: FlashcardApiEnv
	deviceId: string
	resourceId: string
	maxChars?: number
}): Promise<LessonContextBundle> {
	return loadLearningResourceContext(input)
}
