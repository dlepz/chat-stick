import { type ToolLogEntry } from './conversation-store'
import { handleDocsSearchTool } from './docs-tool'
import { type EmailEnv } from './email'
import { handleEmailTool } from './email-tool'
import { handleFileTool } from './file-tools'
import { buildToolResponsePayload } from './gemini-tools'
import {
	generateAndSendImage,
	imagePromptFromArgs,
} from './image-tool'
import { handleSetVoiceTool } from './voice-tool'
import { type WebFetchArgs, fetchWebPage } from './web-fetch'

export interface ToolRouterEnv extends EmailEnv {
	GEMINI_API_KEY: string
	AI: Ai
	VECTORIZE: VectorizeIndex
	DB: D1Database
	STORAGE?: R2Bucket
}

export interface GeminiFunctionCall {
	name: string
	id: string
	args: Record<string, unknown>
}

type ServerToolLogEntry = Omit<ToolLogEntry, 'deviceId' | 'chatId'>

interface RouteGeminiToolCallOptions {
	call: GeminiFunctionCall
	env: ToolRouterEnv
	deviceId: string
	chatId: string
	sendToGemini: (payload: string) => boolean
	sendToDevice: (msg: Record<string, unknown>) => void
	logToolCall: (entry: ServerToolLogEntry) => Promise<void>
	applyVoiceChange: (voice: string) => Promise<void>
	startNewConversation: () => Promise<void>
	forwardDeviceToolCall: (call: GeminiFunctionCall, startMs: number) => void
}

export async function routeGeminiToolCall({
	call,
	env,
	deviceId,
	chatId,
	sendToGemini,
	sendToDevice,
	logToolCall,
	applyVoiceChange,
	startNewConversation,
	forwardDeviceToolCall,
}: RouteGeminiToolCallOptions): Promise<void> {
	console.log(`[Gemini] Tool call: ${call.name}(${JSON.stringify(call.args)})`)
	const startMs = Date.now()

	if (call.name === 'search_docs') {
		const result = await handleDocsSearchTool(env, call.args)
		const payload = buildToolResponsePayload(call.name, call.id, result.response)
		console.log(`[Gemini] Sending tool response: ${payload.length} bytes`)
		if (sendToGemini(payload)) {
			console.log('[Gemini] Tool response sent')
		}
		await logToolCall({
			name: call.name,
			args: call.args,
			result: result.logResult,
			handledBy: 'server',
			durationMs: Date.now() - startMs,
		})
		return
	}

	if (call.name === 'web_fetch' || call.name === 'fetch_url') {
		const args = call.args as WebFetchArgs
		const url = args.url || ''
		console.log(`[Gemini] Fetching: ${url}`)
		const result = await fetchWebPage(url, args.max_chars)
		if (sendToGemini(buildToolResponsePayload(call.name, call.id, result))) {
			console.log(`[Gemini] ${call.name} response sent (${result.content.length} chars)`)
		}
		await logToolCall({
			name: call.name,
			args: call.args,
			result: { url, chars: result.content.length },
			handledBy: 'server',
			durationMs: Date.now() - startMs,
		})
		return
	}

	if (
		call.name === 'list_files' ||
		call.name === 'read_file' ||
		call.name === 'write_file' ||
		call.name === 'append_to_file' ||
		call.name === 'search_files'
	) {
		const response = await handleFileTool(
			env.DB,
			deviceId,
			call.name,
			call.args
		)
		sendToGemini(buildToolResponsePayload(call.name, call.id, response))
		await logToolCall({
			name: call.name,
			args: call.args,
			result: response,
			handledBy: 'server',
			status: 'error' in (response as Record<string, unknown>) ? 'error' : 'ok',
			durationMs: Date.now() - startMs,
		})
		return
	}

	if (call.name === 'set_voice') {
		const result = handleSetVoiceTool(call.args)
		sendToGemini(buildToolResponsePayload(call.name, call.id, result.response))
		await logToolCall({
			name: call.name,
			args: call.args,
			result: result.logResult,
			handledBy: 'server',
			status: result.status,
			durationMs: Date.now() - startMs,
		})
		if (result.ok) {
			await applyVoiceChange(result.voice)
		}
		return
	}

	if (call.name === 'email_me') {
		const result = await handleEmailTool(env, call.args)
		sendToGemini(buildToolResponsePayload(call.name, call.id, result.response))
		await logToolCall({
			name: call.name,
			args: result.logArgs,
			result: result.logResult,
			handledBy: 'server',
			status: result.status,
			error: result.error,
			durationMs: Date.now() - startMs,
		})
		return
	}

	if (call.name === 'show_image') {
		const prompt = imagePromptFromArgs(call.args)
		if (!prompt) {
			sendToGemini(
				buildToolResponsePayload(call.name, call.id, {
					result: 'no prompt provided',
				})
			)
			await logToolCall({
				name: call.name,
				args: call.args,
				result: 'no prompt',
				handledBy: 'server',
				status: 'error',
				durationMs: Date.now() - startMs,
			})
			return
		}

		sendToGemini(
			buildToolResponsePayload(call.name, call.id, {
				result: 'image generation started; it will appear on screen shortly',
			})
		)
		sendToDevice({ type: 'show_image_pending' })
		generateAndSendImage({
			prompt,
			geminiApiKey: env.GEMINI_API_KEY,
			storage: env.STORAGE,
			deviceId,
			chatId,
			toolName: call.name,
			toolArgs: call.args,
			startMs,
			sendToDevice,
			logToolCall,
		}).catch((err) => {
			console.error('[ImageGen] Background generation failed:', err)
		})
		return
	}

	if (call.name === 'new_conversation' || call.name === 'new_chat') {
		console.log('[Gemini] Resetting conversation')
		await logToolCall({
			name: call.name,
			args: call.args,
			result: 'conversation reset',
			handledBy: 'server',
			durationMs: Date.now() - startMs,
		})
		await startNewConversation()
		return
	}

	forwardDeviceToolCall(call, startMs)
}
