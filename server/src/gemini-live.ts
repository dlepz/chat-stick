import { buildGeminiTools } from './gemini-tools'

const GEMINI_LIVE_URL =
	'https://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent'

const GEMINI_LIVE_MODEL = 'models/gemini-3.1-flash-live-preview'
const INPUT_AUDIO_MIME_TYPE = 'audio/pcm;rate=16000'

export interface GeminiMessage {
	setupComplete?: Record<string, unknown>
	serverContent?: {
		modelTurn?: {
			parts?: Array<{
				inlineData?: { mimeType: string; data: string }
				text?: string
			}>
		}
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
}

interface GeminiSetupOptions {
	voiceName: string
	systemInstructionText: string
	canEmail: boolean
}

export async function openGeminiLiveWebSocket(
	apiKey: string
): Promise<WebSocket | null> {
	const resp = await fetch(`${GEMINI_LIVE_URL}?key=${apiKey}`, {
		headers: { Upgrade: 'websocket' },
	})
	return resp.webSocket ?? null
}

export function buildGeminiSetupPayload({
	voiceName,
	systemInstructionText,
	canEmail,
}: GeminiSetupOptions): string {
	return JSON.stringify({
		setup: {
			model: GEMINI_LIVE_MODEL,
			generationConfig: {
				responseModalities: ['AUDIO'],
				speechConfig: {
					voiceConfig: {
						prebuiltVoiceConfig: { voiceName },
					},
				},
			},
			outputAudioTranscription: {},
			inputAudioTranscription: {},
			systemInstruction: {
				parts: [
					{
						text: systemInstructionText,
					},
				],
			},
			tools: buildGeminiTools({ canEmail }),
		},
	})
}

export function buildGeminiRealtimeAudioPayload(audioBase64: string): string {
	return JSON.stringify({
		realtimeInput: {
			audio: {
				data: audioBase64,
				mimeType: INPUT_AUDIO_MIME_TYPE,
			},
		},
	})
}

export function buildGeminiRealtimeTextPayload(content: string): string {
	return JSON.stringify({
		realtimeInput: { text: content },
	})
}

export function buildGeminiAudioStreamEndPayload(): string {
	return JSON.stringify({
		realtimeInput: {
			audioStreamEnd: true,
		},
	})
}
