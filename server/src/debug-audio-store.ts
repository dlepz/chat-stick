export interface DebugAudioEnv {
	HISTORY_API_TOKEN?: string
}

interface DebugAudioMetadata {
	device_id: string
	chat_id: string
	saved_at: string
	reason: string
	pcm_bytes: number
	wav_bytes: number
	samples: number
	avg_abs: number
	chunks: number
	duration_ms: number | null
	dropped_debug_bytes: number
	sample_rate_hz: number
	channels: number
	bits_per_sample: number
}

interface SaveDebugAudioOptions {
	storage: DurableObjectStorage
	deviceId: string
	chatId: string
	reason: string
	chunks: ArrayBuffer[]
	pcmBytes: number
	samples: number
	avgAbs: number
	chunkCount: number
	durationMs: number | null
	droppedDebugBytes: number
}

export const MAX_DEBUG_AUDIO_BYTES = 2_000_000

const DEBUG_AUDIO_SAMPLE_RATE_HZ = 16_000
const DEBUG_AUDIO_CHANNELS = 1
const DEBUG_AUDIO_BITS_PER_SAMPLE = 16
const DEBUG_AUDIO_STORAGE_WAV_KEY = 'debug-audio/latest.wav'
const DEBUG_AUDIO_STORAGE_META_KEY = 'debug-audio/latest.json'

export async function handleDebugAudioRequest(
	request: Request,
	storage: DurableObjectStorage,
	env: DebugAudioEnv
): Promise<Response> {
	const url = new URL(request.url)
	const match = url.pathname.match(/^\/debug\/audio\/([^/]+)\/latest\.(wav|json)$/)
	const requestedDeviceId = match ? decodeURIComponent(match[1]) : ''
	if (!isAuthorizedDebugAudioRequest(request, env)) {
		return new Response('Unauthorized', {
			status: 401,
			headers: debugCorsHeaders(),
		})
	}

	if (url.pathname.endsWith('.json')) {
		const metadata = await storage.get<DebugAudioMetadata>(
			DEBUG_AUDIO_STORAGE_META_KEY
		)
		if (!metadata) {
			return new Response('No debug audio captured yet', {
				status: 404,
				headers: debugCorsHeaders(),
			})
		}
		return new Response(JSON.stringify(metadata), {
			headers: {
				...debugCorsHeaders(),
				'Content-Type': 'application/json',
			},
		})
	}

	const wav = await storage.get<ArrayBuffer>(DEBUG_AUDIO_STORAGE_WAV_KEY)
	if (!wav) {
		return new Response('No debug audio captured yet', {
			status: 404,
			headers: debugCorsHeaders(),
		})
	}

	const filename = `${requestedDeviceId || 'device'}-latest.wav`
	return new Response(wav, {
		headers: {
			...debugCorsHeaders(),
			'Content-Type': 'audio/wav',
			'Content-Length': String(wav.byteLength),
			'Content-Disposition': `attachment; filename="${filename}"`,
			'Cache-Control': 'no-store',
		},
	})
}

export function saveDebugAudio(options: SaveDebugAudioOptions): void {
	if (options.pcmBytes <= 0 || options.chunks.length === 0) {
		return
	}

	const chunks = options.chunks.slice()
	const metadataBase: Omit<DebugAudioMetadata, 'wav_bytes'> = {
		device_id: options.deviceId,
		chat_id: options.chatId,
		saved_at: new Date().toISOString(),
		reason: options.reason,
		pcm_bytes: options.pcmBytes,
		samples: options.samples,
		avg_abs: options.avgAbs,
		chunks: options.chunkCount,
		duration_ms: options.durationMs,
		dropped_debug_bytes: options.droppedDebugBytes,
		sample_rate_hz: DEBUG_AUDIO_SAMPLE_RATE_HZ,
		channels: DEBUG_AUDIO_CHANNELS,
		bits_per_sample: DEBUG_AUDIO_BITS_PER_SAMPLE,
	}
	const wav = createPcm16Wav(
		chunks,
		options.pcmBytes,
		DEBUG_AUDIO_SAMPLE_RATE_HZ,
		DEBUG_AUDIO_CHANNELS,
		DEBUG_AUDIO_BITS_PER_SAMPLE
	)
	const metadata: DebugAudioMetadata = {
		...metadataBase,
		wav_bytes: wav.byteLength,
	}

	Promise.all([
		options.storage.put(DEBUG_AUDIO_STORAGE_WAV_KEY, wav),
		options.storage.put(DEBUG_AUDIO_STORAGE_META_KEY, metadata),
	])
		.then(() => {
			console.log(
				`[DebugAudio] Saved ${options.pcmBytes} PCM bytes (${wav.byteLength} WAV bytes), avg_abs=${metadata.avg_abs}, chunks=${metadata.chunks}`
			)
		})
		.catch((err) => {
			console.error('[DebugAudio] Failed to save recording:', err)
		})
}

function debugCorsHeaders(): HeadersInit {
	return {
		'Access-Control-Allow-Origin': '*',
		'Access-Control-Allow-Methods': 'GET, OPTIONS',
		'Access-Control-Allow-Headers': 'X-History-Token',
	}
}

function isAuthorizedDebugAudioRequest(
	request: Request,
	env: DebugAudioEnv
): boolean {
	const url = new URL(request.url)
	if (url.hostname === 'localhost' || url.hostname === '127.0.0.1' || url.hostname === '[::1]') {
		return true
	}

	const configuredToken = env.HISTORY_API_TOKEN?.trim()
	if (!configuredToken) return false

	const providedToken =
		request.headers.get('X-History-Token') ?? url.searchParams.get('token') ?? ''
	return providedToken === configuredToken
}

function createPcm16Wav(
	chunks: ArrayBuffer[],
	pcmBytes: number,
	sampleRate: number,
	channels: number,
	bitsPerSample: number
): ArrayBuffer {
	const headerBytes = 44
	const wav = new ArrayBuffer(headerBytes + pcmBytes)
	const view = new DataView(wav)
	const bytes = new Uint8Array(wav)
	const blockAlign = (channels * bitsPerSample) / 8
	const byteRate = sampleRate * blockAlign

	writeAscii(bytes, 0, 'RIFF')
	view.setUint32(4, 36 + pcmBytes, true)
	writeAscii(bytes, 8, 'WAVE')
	writeAscii(bytes, 12, 'fmt ')
	view.setUint32(16, 16, true)
	view.setUint16(20, 1, true)
	view.setUint16(22, channels, true)
	view.setUint32(24, sampleRate, true)
	view.setUint32(28, byteRate, true)
	view.setUint16(32, blockAlign, true)
	view.setUint16(34, bitsPerSample, true)
	writeAscii(bytes, 36, 'data')
	view.setUint32(40, pcmBytes, true)

	let offset = headerBytes
	for (const chunk of chunks) {
		const source = new Uint8Array(chunk)
		const writable = Math.min(source.byteLength, bytes.byteLength - offset)
		if (writable <= 0) break
		bytes.set(source.subarray(0, writable), offset)
		offset += writable
	}

	return wav
}

function writeAscii(bytes: Uint8Array, offset: number, text: string) {
	for (let i = 0; i < text.length; i++) {
		bytes[offset + i] = text.charCodeAt(i)
	}
}
