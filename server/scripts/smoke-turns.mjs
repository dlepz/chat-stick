import { existsSync } from 'node:fs'
import { readFile, stat } from 'node:fs/promises'
import { spawnSync } from 'node:child_process'
import { tmpdir } from 'node:os'
import { join } from 'node:path'

const WS_URL = process.env.SMOKE_WS_URL || 'ws://127.0.0.1:8787/ws'
const PCM_PATH =
	process.env.SMOKE_PCM_PATH || join(tmpdir(), 'chat-stick-smoke.pcm')
const CHUNK_BYTES = 3200

function wait(ms) {
	return new Promise((resolve) => setTimeout(resolve, ms))
}

function withTimeout(label, ms, promiseFactory) {
	let timer
	return Promise.race([
		promiseFactory().finally(() => clearTimeout(timer)),
		new Promise((_, reject) => {
			timer = setTimeout(() => reject(new Error(`${label} timed out`)), ms)
		}),
	])
}

function deviceUrl(name) {
	const params = new URLSearchParams({
		device_id: `${name}-${Date.now()}`,
		voice: 'assistant',
		mode: 'assistant',
		image_w: '232',
		image_h: '112',
	})
	return `${WS_URL}?${params}`
}

async function ensurePcm() {
	if (existsSync(PCM_PATH)) {
		const info = await stat(PCM_PATH)
		if (info.size > 0) return PCM_PATH
	}

	const aiffPath = join(tmpdir(), 'chat-stick-smoke.aiff')
	const say = spawnSync('say', ['-o', aiffPath, 'hello can you hear me'], {
		stdio: 'ignore',
	})
	if (say.status !== 0) {
		throw new Error(
			`No PCM fixture at ${PCM_PATH}, and macOS say could not generate one. Set SMOKE_PCM_PATH.`,
		)
	}

	const ffmpeg = spawnSync(
		'ffmpeg',
		['-y', '-loglevel', 'error', '-i', aiffPath, '-f', 's16le', '-ac', '1', '-ar', '16000', PCM_PATH],
		{ stdio: 'ignore' },
	)
	if (ffmpeg.status !== 0) {
		throw new Error(
			`No PCM fixture at ${PCM_PATH}, and ffmpeg could not generate one. Set SMOKE_PCM_PATH.`,
		)
	}

	return PCM_PATH
}

function openSocket(name) {
	return new WebSocket(deviceUrl(name))
}

function sendOnceWhenReady(ws, send) {
	let sent = false
	return (msg) => {
		if (sent) return
		if (msg.type !== 'ready' && msg.type !== 'server_ready') return
		sent = true
		void send()
	}
}

async function runTextIntro() {
	return withTimeout('text intro smoke', 45_000, () =>
		new Promise((resolve, reject) => {
			const ws = openSocket('codex-text-smoke')
			let audio = 0
			let text = ''
			let done = false
			const sendIntro = sendOnceWhenReady(ws, async () => {
				const hook =
					'Hallo! Ich bin bereit. Was lesen wir heute zusammen? Gibt es ein Buch oder einen Text, ueber den du sprechen moechtest?'
				const prompt =
					`For this conversation, become my German reading assistant and roleplay partner. ` +
					`Start now with this exact hook sentence, then stop and wait for me: "${hook}" ` +
					`After that, help with German reading: vocabulary, grammar, translation, pronunciation, ` +
					`and quick quiz questions when useful. Keep the opening under 10 seconds.`
				ws.send(JSON.stringify({ type: 'text', content: prompt }))
			})

			ws.addEventListener('message', (event) => {
				if (typeof event.data !== 'string') {
					audio += event.data.size ?? event.data.byteLength ?? 0
					return
				}
				const msg = JSON.parse(event.data)
				sendIntro(msg)
				if (msg.type === 'transcript' && msg.source === 'model') {
					text += msg.text || ''
				}
				if (msg.type === 'turn_complete') {
					done = true
					ws.close()
					resolve({ audio, text: text.slice(0, 120) })
				}
				if (msg.type === 'error') {
					reject(new Error(`text intro error ${msg.category}: ${msg.message}`))
				}
			})
			ws.addEventListener('close', () => {
				if (!done) reject(new Error('text intro socket closed early'))
			})
			ws.addEventListener('error', (event) => {
				reject(new Error(`text intro websocket error ${event.message || event}`))
			})
		}),
	)
}

async function runSilent() {
	return withTimeout('silent smoke', 15_000, () =>
		new Promise((resolve, reject) => {
			const ws = openSocket('codex-silent-smoke')
			let done = false
			const start = sendOnceWhenReady(ws, async () => {
				ws.send(JSON.stringify({ type: 'start' }))
				const zero = Buffer.alloc(CHUNK_BYTES)
				for (let i = 0; i < 3; i++) {
					ws.send(zero)
					await wait(60)
				}
				ws.send(JSON.stringify({ type: 'stop' }))
			})

			ws.addEventListener('message', (event) => {
				if (typeof event.data !== 'string') return
				const msg = JSON.parse(event.data)
				start(msg)
				if (msg.type === 'ignore_audio') {
					done = true
					ws.close()
					resolve(msg)
				}
				if (msg.type === 'error') {
					reject(new Error(`silent error ${msg.category}: ${msg.message}`))
				}
			})
			ws.addEventListener('close', () => {
				if (!done) reject(new Error('silent socket closed early'))
			})
			ws.addEventListener('error', (event) => {
				reject(new Error(`silent websocket error ${event.message || event}`))
			})
		}),
	)
}

async function runSpoken() {
	const pcmPath = await ensurePcm()
	const pcm = await readFile(pcmPath)
	return withTimeout('spoken smoke', 45_000, () =>
		new Promise((resolve, reject) => {
			const ws = openSocket('codex-spoken-smoke')
			let audio = 0
			let user = ''
			let model = ''
			let done = false
			const start = sendOnceWhenReady(ws, async () => {
				ws.send(JSON.stringify({ type: 'start' }))
				for (let offset = 0; offset < pcm.length; offset += CHUNK_BYTES) {
					ws.send(pcm.subarray(offset, Math.min(offset + CHUNK_BYTES, pcm.length)))
					await wait(80)
				}
				ws.send(JSON.stringify({ type: 'stop' }))
			})

			ws.addEventListener('message', (event) => {
				if (typeof event.data !== 'string') {
					audio += event.data.size ?? event.data.byteLength ?? 0
					return
				}
				const msg = JSON.parse(event.data)
				start(msg)
				if (msg.type === 'transcript' && msg.source === 'user') {
					user += msg.text || ''
				}
				if (msg.type === 'transcript' && msg.source === 'model') {
					model += msg.text || ''
				}
				if (msg.type === 'turn_complete') {
					done = true
					ws.close()
					resolve({ audio, user, model: model.slice(0, 120) })
				}
				if (msg.type === 'ignore_audio') {
					reject(new Error(`spoken audio was ignored: ${msg.reason}`))
				}
				if (msg.type === 'error') {
					reject(new Error(`spoken error ${msg.category}: ${msg.message}`))
				}
			})
			ws.addEventListener('close', () => {
				if (!done) reject(new Error('spoken socket closed early'))
			})
			ws.addEventListener('error', (event) => {
				reject(new Error(`spoken websocket error ${event.message || event}`))
			})
		}),
	)
}

const text = await runTextIntro()
console.log('TEXT_OK', JSON.stringify(text))

const silent = await runSilent()
console.log('SILENT_OK', JSON.stringify(silent))

const spoken = await runSpoken()
console.log('SPOKEN_OK', JSON.stringify(spoken))
