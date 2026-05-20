/**
 * Image generation and processing for the M5StickS3 display.
 *
 * Generates images via Google's Imagen API, then processes them to a 1-bit
 * dithered bitmap matching the on-device image bounding box defined in
 * `designs.md` (232x112 — rows 1–7 of the 240x135 LCD).
 */

import UPNG from 'upng-js'

// Target dimensions match the chat text area bounding box (rows 1–7).
// See designs.md "Layout and Spacing" / "Images".
const TARGET_WIDTH = 232
const TARGET_HEIGHT = 112

// Imagen calls take ~5–15s; cap at 30s.
const IMAGEN_TIMEOUT_MS = 30000

export interface ImageResult {
	data: string // base64 1-bit packed pixels for device
	width: number
	height: number
	ditheredPng: ArrayBuffer // dithered PNG for storage (matches what device shows)
	originalPng: ArrayBuffer // full-color Imagen PNG (pre-dither) for archival
	enhancedPrompt: string // the prompt actually sent to Imagen (with style suffix)
}

/**
 * Generate an image from a prompt and process it for the device display.
 * Returns null on any failure; the caller decides how to surface that.
 */
export async function generateAndProcessImage(
	prompt: string,
	apiKey: string
): Promise<ImageResult | null> {
	try {
		console.log(`[ImageGen] Generating image for: "${prompt}"`)
		const enhancedPrompt = buildEnhancedPrompt(prompt)
		const imageBase64 = await generateImage(enhancedPrompt, apiKey)
		if (!imageBase64) return null

		const pngBuffer = base64ToArrayBuffer(imageBase64)
		const decoded = UPNG.decode(pngBuffer)
		const rgba = new Uint8Array(UPNG.toRGBA8(decoded)[0])
		console.log(`[ImageGen] Decoded PNG: ${decoded.width}x${decoded.height}`)

		const resized = resizeImageCover(
			rgba,
			decoded.width,
			decoded.height,
			TARGET_WIDTH,
			TARGET_HEIGHT
		)

		const grayscale = toGrayscale(resized, TARGET_WIDTH, TARGET_HEIGHT)
		const dithered = floydSteinbergDither(grayscale, TARGET_WIDTH, TARGET_HEIGHT)
		const packed = packBits(dithered)
		const base64 = arrayBufferToBase64(packed)
		console.log(`[ImageGen] Packed to ${packed.length} bytes (${base64.length}b64 chars)`)

		const ditheredRgba = new Uint8Array(TARGET_WIDTH * TARGET_HEIGHT * 4)
		for (let i = 0; i < dithered.length; i++) {
			const v = dithered[i] ? 255 : 0
			ditheredRgba[i * 4] = v
			ditheredRgba[i * 4 + 1] = v
			ditheredRgba[i * 4 + 2] = v
			ditheredRgba[i * 4 + 3] = 255
		}
		const ditheredPng = UPNG.encode(
			[ditheredRgba.buffer],
			TARGET_WIDTH,
			TARGET_HEIGHT,
			0
		)

		return {
			data: base64,
			width: TARGET_WIDTH,
			height: TARGET_HEIGHT,
			ditheredPng,
			originalPng: pngBuffer,
			enhancedPrompt,
		}
	} catch (error) {
		console.error('[ImageGen] Error processing image:', error)
		return null
	}
}

function buildEnhancedPrompt(prompt: string): string {
	// Tuned for monochrome dithered output: bright high-contrast subjects on black.
	return `${prompt}. Style: white artwork on solid black background, high contrast, simple composition, clear silhouettes, dark mode aesthetic with bright white elements against pure black.`
}

async function generateImage(enhancedPrompt: string, apiKey: string): Promise<string | null> {
	const controller = new AbortController()
	const timeoutId = setTimeout(() => controller.abort(), IMAGEN_TIMEOUT_MS)

	let response: Response
	try {
		response = await fetch(
			'https://generativelanguage.googleapis.com/v1beta/models/imagen-4.0-fast-generate-001:predict',
			{
				method: 'POST',
				headers: {
					'Content-Type': 'application/json',
					'x-goog-api-key': apiKey,
				},
				body: JSON.stringify({
					instances: [{ prompt: enhancedPrompt }],
					// 232:112 ≈ 2.07; 16:9 (1.78) is the closest preset. Cover scaling
					// will crop a small amount top/bottom — acceptable for B&W output.
					parameters: { sampleCount: 1, aspectRatio: '16:9' },
				}),
				signal: controller.signal,
			}
		)
	} catch (error) {
		if (error instanceof Error && error.name === 'AbortError') {
			console.error(`[ImageGen] Imagen API timed out after ${IMAGEN_TIMEOUT_MS}ms`)
			return null
		}
		throw error
	} finally {
		clearTimeout(timeoutId)
	}

	if (!response.ok) {
		const errorText = await response.text()
		console.error(`[ImageGen] Imagen API error: ${response.status} ${errorText}`)
		return null
	}

	const result = (await response.json()) as {
		predictions?: Array<{ bytesBase64Encoded?: string }>
	}
	const imageData = result.predictions?.[0]?.bytesBase64Encoded
	if (!imageData) {
		console.error('[ImageGen] No image data in Imagen response')
		return null
	}
	return imageData
}

/**
 * Resize with cover semantics: image fills the target box, centered, overflow
 * clipped on the long axis. Matches the "cover" rule in designs.md.
 */
function resizeImageCover(
	pixels: Uint8Array,
	srcW: number,
	srcH: number,
	dstW: number,
	dstH: number
): Uint8Array {
	const dst = new Uint8Array(dstW * dstH * 4)
	// Cover: pick the SMALLER scale ratio so the image is large enough to fill
	// both dimensions; the larger axis overflows and gets clipped.
	const scale = Math.min(srcW / dstW, srcH / dstH)
	const cropOffsetX = (srcW - dstW * scale) / 2
	const cropOffsetY = (srcH - dstH * scale) / 2

	for (let y = 0; y < dstH; y++) {
		for (let x = 0; x < dstW; x++) {
			const dstIdx = (y * dstW + x) * 4
			const srcX = cropOffsetX + x * scale
			const srcY = cropOffsetY + y * scale

			const x0 = Math.max(0, Math.min(srcW - 1, Math.floor(srcX)))
			const y0 = Math.max(0, Math.min(srcH - 1, Math.floor(srcY)))
			const x1 = Math.min(x0 + 1, srcW - 1)
			const y1 = Math.min(y0 + 1, srcH - 1)
			const xw = srcX - Math.floor(srcX)
			const yw = srcY - Math.floor(srcY)

			for (let c = 0; c < 4; c++) {
				const v00 = pixels[(y0 * srcW + x0) * 4 + c]
				const v10 = pixels[(y0 * srcW + x1) * 4 + c]
				const v01 = pixels[(y1 * srcW + x0) * 4 + c]
				const v11 = pixels[(y1 * srcW + x1) * 4 + c]
				dst[dstIdx + c] = Math.round(
					v00 * (1 - xw) * (1 - yw) +
						v10 * xw * (1 - yw) +
						v01 * (1 - xw) * yw +
						v11 * xw * yw
				)
			}
		}
	}
	return dst
}

function toGrayscale(pixels: Uint8Array, width: number, height: number): Float32Array {
	const gray = new Float32Array(width * height)
	for (let i = 0; i < width * height; i++) {
		// ITU-R BT.601 luminance
		gray[i] = 0.299 * pixels[i * 4] + 0.587 * pixels[i * 4 + 1] + 0.114 * pixels[i * 4 + 2]
	}
	return gray
}

function floydSteinbergDither(
	gray: Float32Array,
	width: number,
	height: number
): Uint8Array {
	const result = new Uint8Array(width * height)
	const buffer = Float32Array.from(gray)
	for (let y = 0; y < height; y++) {
		for (let x = 0; x < width; x++) {
			const idx = y * width + x
			const oldPixel = Math.max(0, Math.min(255, buffer[idx]))
			const newPixel = oldPixel < 128 ? 0 : 255
			result[idx] = newPixel === 255 ? 1 : 0
			const error = oldPixel - newPixel
			if (x + 1 < width) buffer[idx + 1] += (error * 7) / 16
			if (y + 1 < height) {
				if (x > 0) buffer[idx + width - 1] += (error * 3) / 16
				buffer[idx + width] += (error * 5) / 16
				if (x + 1 < width) buffer[idx + width + 1] += (error * 1) / 16
			}
		}
	}
	return result
}

/** Pack 1-bit pixels into bytes, MSB first (bit 7 = first pixel of each byte). */
function packBits(pixels: Uint8Array): Uint8Array {
	const packed = new Uint8Array(Math.ceil(pixels.length / 8))
	for (let i = 0; i < pixels.length; i++) {
		if (pixels[i]) packed[Math.floor(i / 8)] |= 1 << (7 - (i % 8))
	}
	return packed
}

function base64ToArrayBuffer(base64: string): ArrayBuffer {
	const binary = atob(base64)
	const bytes = new Uint8Array(binary.length)
	for (let i = 0; i < binary.length; i++) bytes[i] = binary.charCodeAt(i)
	return bytes.buffer
}

function arrayBufferToBase64(buffer: Uint8Array): string {
	let binary = ''
	for (let i = 0; i < buffer.length; i++) binary += String.fromCharCode(buffer[i])
	return btoa(binary)
}
