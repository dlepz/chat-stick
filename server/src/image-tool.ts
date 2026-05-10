import { type ToolLogEntry } from './conversation-store'
import { generateAndProcessImage } from './image-gen'

type ServerToolLogEntry = Omit<ToolLogEntry, 'deviceId' | 'chatId'>

interface GenerateAndSendImageOptions {
	prompt: string
	geminiApiKey: string
	storage?: R2Bucket
	deviceId: string
	chatId: string
	toolName: string
	toolArgs: unknown
	startMs: number
	sendToDevice: (msg: Record<string, unknown>) => void
	logToolCall: (entry: ServerToolLogEntry) => Promise<void>
}

export function imagePromptFromArgs(args: Record<string, unknown>): string {
	return typeof args.prompt === 'string' ? args.prompt.trim() : ''
}

export async function generateAndSendImage({
	prompt,
	geminiApiKey,
	storage,
	deviceId,
	chatId,
	toolName,
	toolArgs,
	startMs,
	sendToDevice,
	logToolCall,
}: GenerateAndSendImageOptions): Promise<void> {
	const result = await generateAndProcessImage(prompt, geminiApiKey)
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

	// Persist the dithered PNG to R2 if STORAGE is bound. Best-effort; failure
	// here doesn't block sending to the device.
	let imageKey: string | undefined
	if (storage) {
		const stamp = `${Date.now()}-${Math.random().toString(36).slice(2, 8)}`
		imageKey = `chat-stick/assets/${deviceId}/images/${chatId}-${stamp}.png`
		try {
			await storage.put(imageKey, result.ditheredPng, {
				httpMetadata: { contentType: 'image/png' },
			})
			console.log(`[ImageGen] Stored dithered PNG at ${imageKey}`)
		} catch (err) {
			console.error('[ImageGen] R2 upload failed:', err)
			imageKey = undefined
		}
	}

	sendToDevice({
		type: 'show_image',
		data: result.data,
		width: result.width,
		height: result.height,
		...(imageKey ? { key: imageKey } : {}),
	})

	await logToolCall({
		name: toolName,
		args: toolArgs,
		result: { width: result.width, height: result.height, key: imageKey ?? null },
		handledBy: 'server',
		durationMs: Date.now() - startMs,
	})
}
