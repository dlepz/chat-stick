import {
	MAX_FILE_BYTES,
	USER_INSTRUCTIONS_PATH,
	appendFile,
	canonicalFilePath,
	ensureUserInstructionsFile,
	listFiles,
	readFile,
	resolveFilePath,
	searchFiles,
	writeFile,
} from './files'

export async function handleFileTool(
	db: D1Database,
	deviceId: string,
	name: string,
	args: Record<string, unknown>,
): Promise<Record<string, unknown>> {
	const requestedPath = typeof args.path === 'string' ? args.path.trim() : ''
	const path = requestedPath ? canonicalFilePath(requestedPath) : ''
	const content = typeof args.content === 'string' ? args.content : ''
	const query = typeof args.query === 'string' ? args.query : ''
	try {
		await ensureUserInstructionsFile(db, deviceId)
	} catch (err) {
		return {
			error: `failed to ensure ${USER_INSTRUCTIONS_PATH}: ${
				err instanceof Error ? err.message : String(err)
			}`,
		}
	}

	switch (name) {
		case 'list_files': {
			const files = await listFiles(db, deviceId)
			return { files, count: files.length }
		}
		case 'read_file': {
			if (!path) return { error: 'path is required' }
			const file = await readFile(db, deviceId, path)
			if (file) {
				return { path, content: file.content, updated_at: file.updated_at }
			}
			const resolution = await resolveFilePath(db, deviceId, path)
			if (resolution.kind === 'auto') {
				const matched = await readFile(db, deviceId, resolution.path)
				if (matched) {
					return {
						path: resolution.path,
						content: matched.content,
						updated_at: matched.updated_at,
						note: `no exact match for "${path}" — read closest match "${resolution.path}"`,
					}
				}
			}
			return {
				error: `file not found: ${path}`,
				suggestions: resolution.kind === 'suggestions' ? resolution.suggestions : [],
			}
		}
		case 'write_file': {
			if (!path) return { error: 'path is required' }
			if (content.length > MAX_FILE_BYTES) {
				return {
					error: `content too large: ${content.length} bytes (max ${MAX_FILE_BYTES})`,
				}
			}
			await writeFile(db, deviceId, path, content)
			const saved = await readFile(db, deviceId, path)
			if (!saved || saved.content !== content) {
				return { error: `failed to verify saved file: ${path}` }
			}
			return {
				ok: true,
				path,
				...(requestedPath && requestedPath !== path ? { requested_path: requestedPath } : {}),
				size: content.length,
				...(path === USER_INSTRUCTIONS_PATH ? { applies_to: 'next Gemini session/reconnect' } : {}),
			}
		}
		case 'append_to_file': {
			if (!path) return { error: 'path is required' }
			const existing = await readFile(db, deviceId, path)
			const projectedSize = (existing?.content.length ?? 0) + content.length
			if (projectedSize > MAX_FILE_BYTES) {
				return {
					error: `would exceed file size limit (${projectedSize} > ${MAX_FILE_BYTES} bytes)`,
				}
			}
			await appendFile(db, deviceId, path, content)
			const saved = await readFile(db, deviceId, path)
			if (!saved || saved.content.length !== projectedSize) {
				return { error: `failed to verify appended file: ${path}` }
			}
			return {
				ok: true,
				path,
				...(requestedPath && requestedPath !== path ? { requested_path: requestedPath } : {}),
				size: projectedSize,
				...(path === USER_INSTRUCTIONS_PATH ? { applies_to: 'next Gemini session/reconnect' } : {}),
			}
		}
		case 'search_files': {
			if (!query.trim()) return { error: 'query is required' }
			const hits = await searchFiles(db, deviceId, query)
			return { hits, count: hits.length }
		}
		default:
			return { error: `unknown file tool: ${name}` }
	}
}
