// Device-scoped file storage in D1.
// Every query is filtered by device_id so a device only ever sees its own files.

export const MAX_FILE_BYTES = 100_000
export const USER_INSTRUCTIONS_PATH = 'user-instructions.md'

export interface FileSummary {
	path: string
	size: number
	updated_at: string
}

export interface FileRecord {
	content: string
	updated_at: string
}

export interface FileSearchHit {
	path: string
	updated_at: string
	snippet: string
	match_count: number
}

export async function listFiles(
	db: D1Database,
	deviceId: string
): Promise<FileSummary[]> {
	const rows = await db
		.prepare(
			`SELECT path, length(content) AS size, updated_at
       FROM files WHERE device_id = ?
       ORDER BY updated_at DESC`
		)
		.bind(deviceId)
		.all<FileSummary>()
	return rows.results ?? []
}

export async function readFile(
	db: D1Database,
	deviceId: string,
	path: string
): Promise<FileRecord | null> {
	const row = await db
		.prepare(
			`SELECT content, updated_at FROM files
       WHERE device_id = ? AND path = ?`
		)
		.bind(deviceId, path)
		.first<FileRecord>()
	return row ?? null
}

// Strips everything but lowercase letters and digits so "Todo.md", "to-do.md",
// and "TO_DO.MD" all collapse to the same key.
function normalizePath(path: string): string {
	return path.toLowerCase().replace(/[^a-z0-9]/g, '')
}

function levenshtein(a: string, b: string): number {
	if (a === b) return 0
	if (!a.length) return b.length
	if (!b.length) return a.length
	const prev = new Array<number>(b.length + 1)
	for (let j = 0; j <= b.length; j++) prev[j] = j
	for (let i = 1; i <= a.length; i++) {
		let prevDiag = prev[0]
		prev[0] = i
		for (let j = 1; j <= b.length; j++) {
			const tmp = prev[j]
			const cost = a.charCodeAt(i - 1) === b.charCodeAt(j - 1) ? 0 : 1
			prev[j] = Math.min(prev[j] + 1, prev[j - 1] + 1, prevDiag + cost)
			prevDiag = tmp
		}
	}
	return prev[b.length]
}

export type FilePathResolution =
	| { kind: 'auto'; path: string }
	| { kind: 'suggestions'; suggestions: string[] }

// Tries to find a sensible match for a path the model asked for but doesn't
// exactly exist. If exactly one stored file normalizes to the same key, returns
// it as an auto-resolution. Otherwise returns up to `limit` closest paths
// ranked by edit distance on the normalized form.
export async function resolveFilePath(
	db: D1Database,
	deviceId: string,
	requested: string,
	limit = 5
): Promise<FilePathResolution> {
	const all = await listFiles(db, deviceId)
	if (all.length === 0) return { kind: 'suggestions', suggestions: [] }

	const want = normalizePath(requested)
	const normalized = all.map((f) => ({ path: f.path, key: normalizePath(f.path) }))

	const exactNormalized = normalized.filter((f) => f.key === want)
	if (exactNormalized.length === 1) {
		return { kind: 'auto', path: exactNormalized[0].path }
	}

	const ranked = normalized
		.map((f) => ({ path: f.path, dist: levenshtein(want, f.key) }))
		.sort((a, b) => a.dist - b.dist)
		.slice(0, limit)
		.map((f) => f.path)

	return { kind: 'suggestions', suggestions: ranked }
}

export async function ensureUserInstructionsFile(
	db: D1Database,
	deviceId: string
): Promise<FileRecord> {
	await db
		.prepare(
			`INSERT OR IGNORE INTO files (device_id, path, content, created_at, updated_at)
       VALUES (?, ?, '', datetime('now'), datetime('now'))`
		)
		.bind(deviceId, USER_INSTRUCTIONS_PATH)
		.run()

	const file = await readFile(db, deviceId, USER_INSTRUCTIONS_PATH)
	if (!file) {
		throw new Error(`failed to ensure ${USER_INSTRUCTIONS_PATH}`)
	}
	return file
}

export async function writeFile(
	db: D1Database,
	deviceId: string,
	path: string,
	content: string
): Promise<void> {
	await db
		.prepare(
			`INSERT INTO files (device_id, path, content, created_at, updated_at)
       VALUES (?, ?, ?, datetime('now'), datetime('now'))
       ON CONFLICT(device_id, path) DO UPDATE SET
         content = excluded.content,
         updated_at = excluded.updated_at`
		)
		.bind(deviceId, path, content)
		.run()
}

export async function appendFile(
	db: D1Database,
	deviceId: string,
	path: string,
	content: string
): Promise<void> {
	// Atomic append via SQL concat — no read-then-write needed.
	await db
		.prepare(
			`INSERT INTO files (device_id, path, content, created_at, updated_at)
       VALUES (?, ?, ?, datetime('now'), datetime('now'))
       ON CONFLICT(device_id, path) DO UPDATE SET
         content = files.content || excluded.content,
         updated_at = excluded.updated_at`
		)
		.bind(deviceId, path, content)
		.run()
}

// Substring search across all files for a device. Case-insensitive.
// Returns up to `limit` files containing `query`, each with a short snippet
// around the first match and a count of matches in that file.
export async function searchFiles(
	db: D1Database,
	deviceId: string,
	query: string,
	limit = 10
): Promise<FileSearchHit[]> {
	const q = query.trim()
	if (!q) return []

	const like = `%${q.replace(/[\\%_]/g, (c) => '\\' + c)}%`
	const rows = await db
		.prepare(
			`SELECT path, content, updated_at FROM files
       WHERE device_id = ?
         AND LOWER(content) LIKE LOWER(?) ESCAPE '\\'
       ORDER BY updated_at DESC
       LIMIT ?`
		)
		.bind(deviceId, like, limit)
		.all<{ path: string; content: string; updated_at: string }>()

	const needle = q.toLowerCase()
	const SNIPPET_PAD = 60

	return (rows.results ?? []).map((row) => {
		const haystack = row.content.toLowerCase()
		const idx = haystack.indexOf(needle)
		const start = Math.max(0, idx - SNIPPET_PAD)
		const end = Math.min(row.content.length, idx + needle.length + SNIPPET_PAD)
		const prefix = start > 0 ? '…' : ''
		const suffix = end < row.content.length ? '…' : ''
		const snippet = prefix + row.content.slice(start, end) + suffix

		// Count matches without regex (avoids escaping issues with user input).
		let count = 0
		let from = 0
		while (true) {
			const next = haystack.indexOf(needle, from)
			if (next === -1) break
			count++
			from = next + needle.length
		}

		return {
			path: row.path,
			updated_at: row.updated_at,
			snippet,
			match_count: count,
		}
	})
}
