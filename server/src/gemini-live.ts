export const GEMINI_LIVE_MODEL = 'models/gemini-3.1-flash-live-preview'

const GEMINI_LIVE_URL =
	'https://generativelanguage.googleapis.com/ws/google.ai.generativelanguage.v1beta.GenerativeService.BidiGenerateContent'
const INPUT_AUDIO_MIME_TYPE = 'audio/pcm;rate=16000'

export async function openGeminiLiveWebSocket(apiKey: string): Promise<WebSocket | null> {
	const resp = await fetch(`${GEMINI_LIVE_URL}?key=${apiKey}`, {
		headers: { Upgrade: 'websocket' },
	})
	return resp.webSocket ?? null
}

export function buildGeminiActivityStartPayload(): string {
	return JSON.stringify({
		realtimeInput: { activityStart: {} },
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

export function buildGeminiActivityEndPayload(): string {
	return JSON.stringify({
		realtimeInput: { activityEnd: {} },
	})
}
