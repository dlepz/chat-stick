import { type ToolLogEntry } from './conversation-store'
import { generateAndProcessImage } from './image-gen'
import {
	type ImageSummary,
	getImageById,
	listRecentImages,
	recordImage,
	searchImages,
} from './images'

type ServerToolLogEntry = Omit<ToolLogEntry, 'deviceId' | 'chatId'>

interface GenerateAndSendImageOptions {
	prompt: string
	geminiApiKey: string
	db: D1Database
	storage?: R2Bucket
	deviceId: string
	chatId: string
	targetWidth: number
	targetHeight: number
	toolName: string
	toolArgs: unknown
	startMs: number
	sendToDevice: (msg: Record<string, unknown>) => void
	logToolCall: (entry: ServerToolLogEntry) => Promise<void>
}

interface HandleImageToolOptions {
	name: string
	args: Record<string, unknown>
	db: D1Database
	deviceId: string
	sendToDevice: (msg: Record<string, unknown>) => void
}

export function imagePromptFromArgs(args: Record<string, unknown>): string {
	return typeof args.prompt === 'string' ? args.prompt.trim() : ''
}

export async function generateAndSendImage({
	prompt,
	geminiApiKey,
	db,
	storage,
	deviceId,
	chatId,
	targetWidth,
	targetHeight,
	toolName,
	toolArgs,
	startMs,
	sendToDevice,
	logToolCall,
}: GenerateAndSendImageOptions): Promise<void> {
	const turnChatId = chatId
	const result = await generateAndProcessImage(prompt, geminiApiKey, targetWidth, targetHeight)
	if (!result) {
		sendToDevice({ type: 'show_image_failed' })
		await logToolCall({
			name: toolName,
			args: toolArgs,
			result: 'generation failed',
			handledBy: 'server',
			status: 'error',
			durationMs: Date.now() - startMs,
		})
		return
	}

	// Persist the dithered PNG (what the device shows) and the original full-color
	// PNG (for archival / future re-dither) to R2 if STORAGE is bound. Best-effort;
	// failure here doesn't block sending to the device.
	let ditheredKey: string | undefined
	let originalKey: string | undefined
	if (storage) {
		const stamp = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`
		const basePath = `chat-stick/assets/${deviceId}/images/${turnChatId}-${stamp}`
		ditheredKey = `${basePath}.png`
		originalKey = `${basePath}-original.png`
		try {
			await storage.put(ditheredKey, result.ditheredPng, {
				httpMetadata: { contentType: 'image/png' },
			})
			console.log(`[ImageGen] Stored dithered PNG at ${ditheredKey}`)
		} catch (err) {
			console.error('[ImageGen] R2 upload (dithered) failed:', err)
			ditheredKey = undefined
		}
		try {
			await storage.put(originalKey, result.originalPng, {
				httpMetadata: { contentType: 'image/png' },
			})
			console.log(`[ImageGen] Stored original PNG at ${originalKey}`)
		} catch (err) {
			console.error('[ImageGen] R2 upload (original) failed:', err)
			originalKey = undefined
		}
	}

	// Record the image in D1 so the model can recall it later by prompt search
	// or re-display by id. Packed bits are kept inline so re-display is a single
	// DB read with no R2 round-trip or PNG decode.
	let imageId: number | undefined
	try {
		imageId = await recordImage(db, {
			deviceId,
			chatId: turnChatId || null,
			prompt,
			enhancedPrompt: result.enhancedPrompt,
			ditheredKey: ditheredKey ?? null,
			originalKey: originalKey ?? null,
			packedBits: result.data,
			width: result.width,
			height: result.height,
		})
	} catch (err) {
		console.error('[ImageGen] Failed to record image in D1:', err)
	}

	sendToDevice({
		type: 'show_image',
		data: result.data,
		width: result.width,
		height: result.height,
		...(ditheredKey ? { key: ditheredKey } : {}),
		...(imageId ? { image_id: imageId } : {}),
	})

	await logToolCall({
		name: toolName,
		args: toolArgs,
		result: {
			width: result.width,
			height: result.height,
			key: ditheredKey ?? null,
			original_key: originalKey ?? null,
			image_id: imageId ?? null,
		},
		handledBy: 'server',
		durationMs: Date.now() - startMs,
	})
}

export async function handleImageTool({
	name,
	args,
	db,
	deviceId,
	sendToDevice,
}: HandleImageToolOptions): Promise<Record<string, unknown>> {
	const formatSummary = (img: ImageSummary) => ({
		id: img.id,
		prompt: img.prompt,
		created_at: img.created_at,
		chat_id: img.chat_id,
		width: img.width,
		height: img.height,
	})

	switch (name) {
		case 'list_recent_images': {
			const rawLimit = args.limit
			const limit = typeof rawLimit === 'number' ? rawLimit : 10
			const images = await listRecentImages(db, deviceId, limit)
			return { images: images.map(formatSummary), count: images.length }
		}
		case 'search_images': {
			const query = typeof args.query === 'string' ? args.query : ''
			if (!query.trim()) return { error: 'query is required' }
			const rawLimit = args.limit
			const limit = typeof rawLimit === 'number' ? rawLimit : 10
			const hits = await searchImages(db, deviceId, query, limit)
			return {
				hits: hits.map((hit) => ({
					...formatSummary(hit),
					snippet: hit.snippet,
					match_count: hit.match_count,
				})),
				count: hits.length,
			}
		}
		case 'show_saved_image': {
			const idArg = args.id ?? (args as { image_id?: unknown }).image_id
			const id = typeof idArg === 'number' ? idArg : Number(idArg)
			if (!Number.isFinite(id) || id <= 0) return { error: 'id is required' }
			const image = await getImageById(db, deviceId, id)
			if (!image) return { error: `image not found: ${id}` }
			sendToDevice({
				type: 'show_image',
				data: image.packed_bits,
				width: image.width,
				height: image.height,
				image_id: image.id,
				...(image.dithered_key ? { key: image.dithered_key } : {}),
			})
			return {
				result: 'ok',
				id: image.id,
				prompt: image.prompt,
				width: image.width,
				height: image.height,
			}
		}
		default:
			return { error: `unknown image tool: ${name}` }
	}
}
