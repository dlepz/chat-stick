import { openGeminiLiveWebSocket } from './gemini-live'

interface GeminiClientConnectOptions {
	apiKey: string
	setupPayload: string
	isCurrent: () => boolean
	onMessage: (text: string) => void
	onClose: (event: CloseEvent, wasReady: boolean) => void
	onError: (event: Event) => void
}

type GeminiConnectResult = 'connected' | 'already_connected' | 'stale' | 'unavailable'

export class GeminiClient {
	private ws: WebSocket | null = null
	private connecting = false
	private ready = false

	get isConnected(): boolean {
		return this.ws !== null
	}

	get isConnecting(): boolean {
		return this.connecting
	}

	get isReady(): boolean {
		return this.ready
	}

	async connect({
		apiKey,
		setupPayload,
		isCurrent,
		onMessage,
		onClose,
		onError,
	}: GeminiClientConnectOptions): Promise<GeminiConnectResult> {
		if (this.connecting || this.ws) {
			return 'already_connected'
		}

		this.connecting = true
		try {
			const ws = await openGeminiLiveWebSocket(apiKey)
			if (!ws) {
				return 'unavailable'
			}
			if (!isCurrent()) {
				closeWebSocket(ws)
				return 'stale'
			}

			ws.accept()
			this.ws = ws
			this.ready = false

			ws.addEventListener('message', (event) => {
				if (this.ws !== ws || !isCurrent()) return
				const raw = event.data
				const text = typeof raw === 'string' ? raw : new TextDecoder().decode(raw as ArrayBuffer)
				onMessage(text)
			})

			ws.addEventListener('close', (event) => {
				if (this.ws !== ws) return
				const wasReady = this.ready
				this.ws = null
				this.ready = false
				onClose(event, wasReady)
			})

			ws.addEventListener('error', (event) => {
				if (this.ws !== ws || !isCurrent()) return
				onError(event)
			})

			ws.send(setupPayload)
			return 'connected'
		} finally {
			this.connecting = false
		}
	}

	markReady() {
		if (this.ws) {
			this.ready = true
		}
	}

	send(payload: string): boolean {
		if (!this.ws) {
			return false
		}
		this.ws.send(payload)
		return true
	}

	close() {
		const ws = this.ws
		this.ws = null
		this.ready = false
		if (ws) {
			closeWebSocket(ws)
		}
	}
}

function closeWebSocket(ws: WebSocket) {
	try {
		ws.close()
	} catch {
		// ignore
	}
}
