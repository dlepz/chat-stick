export interface ConversationMessage {
	role: 'user' | 'assistant'
	content: string
}

export interface ToolLogEntry {
	deviceId: string
	chatId: string
	name: string
	args: unknown
	result?: unknown
	handledBy: 'server' | 'device'
	status?: 'ok' | 'error'
	error?: string
	durationMs?: number
}

export async function saveConversationExchange(
	db: D1Database,
	deviceId: string,
	chatId: string,
	userText: string,
	assistantText: string,
): Promise<void> {
	const user = userText.trim()
	const assistant = assistantText.trim()
	if (!user && !assistant) return

	try {
		const row = await db.prepare('SELECT messages FROM conversations WHERE chat_id = ?')
			.bind(chatId)
			.first<{ messages: string }>()

		const messages: ConversationMessage[] = row?.messages ? JSON.parse(row.messages) : []

		if (user) messages.push({ role: 'user', content: user })
		if (assistant) messages.push({ role: 'assistant', content: assistant })

		const trimmed = messages.slice(-20)

		await db.prepare(
			`INSERT INTO conversations (chat_id, device_id, messages, last_message, updated_at)
			 VALUES (?, ?, ?, ?, datetime('now'))
			 ON CONFLICT(chat_id) DO UPDATE SET
			   messages = excluded.messages,
			   last_message = excluded.last_message,
			   updated_at = excluded.updated_at`,
		)
			.bind(chatId, deviceId, JSON.stringify(trimmed), assistant || null)
			.run()

		await db.prepare(
			`INSERT INTO message_log (device_id, chat_id, user_text, assistant_text)
			 VALUES (?, ?, ?, ?)`,
		)
			.bind(deviceId, chatId, user || null, assistant || null)
			.run()

		console.log(`[DB] Saved exchange: chat=${chatId} (${trimmed.length} messages)`)
	} catch (err) {
		console.error('[DB] Failed to save exchange:', err)
	}
}

export async function insertToolLog(db: D1Database, entry: ToolLogEntry): Promise<void> {
	try {
		const argsStr = entry.args === undefined ? null : JSON.stringify(entry.args)
		const resultStr =
			entry.result === undefined
				? null
				: typeof entry.result === 'string'
					? entry.result
					: JSON.stringify(entry.result)
		const trim = (s: string | null) =>
			s && s.length > 8000 ? s.slice(0, 8000) + '…[truncated]' : s

		await db.prepare(
			`INSERT INTO tool_log (device_id, chat_id, tool_name, args, result, handled_by, status, error, duration_ms)
			 VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?)`,
		)
			.bind(
				entry.deviceId,
				entry.chatId,
				entry.name,
				trim(argsStr),
				trim(resultStr),
				entry.handledBy,
				entry.status ?? 'ok',
				entry.error ?? null,
				entry.durationMs ?? null,
			)
			.run()
		console.log(
			`[ToolLog] ${entry.name} by=${entry.handledBy} status=${entry.status ?? 'ok'}` +
				(entry.durationMs !== undefined ? ` ${entry.durationMs}ms` : ''),
		)
	} catch (err) {
		console.error('[ToolLog] Failed to insert:', err)
	}
}
