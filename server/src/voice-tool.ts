export const AVAILABLE_VOICES = [
	{ name: 'Zephyr', description: 'Bright' },
	{ name: 'Puck', description: 'Upbeat' },
	{ name: 'Charon', description: 'Informative' },
	{ name: 'Kore', description: 'Firm' },
	{ name: 'Fenrir', description: 'Excitable' },
	{ name: 'Leda', description: 'Youthful' },
	{ name: 'Orus', description: 'Firm' },
	{ name: 'Aoede', description: 'Breezy' },
	{ name: 'Callirrhoe', description: 'Easy-going' },
	{ name: 'Autonoe', description: 'Bright' },
	{ name: 'Enceladus', description: 'Breathy' },
	{ name: 'Iapetus', description: 'Clear' },
	{ name: 'Umbriel', description: 'Easy-going' },
	{ name: 'Algieba', description: 'Smooth' },
	{ name: 'Despina', description: 'Smooth' },
	{ name: 'Erinome', description: 'Clear' },
	{ name: 'Algenib', description: 'Gravelly' },
	{ name: 'Rasalgethi', description: 'Informative' },
	{ name: 'Laomedeia', description: 'Upbeat' },
	{ name: 'Achernar', description: 'Soft' },
	{ name: 'Alnilam', description: 'Firm' },
	{ name: 'Schedar', description: 'Even' },
	{ name: 'Gacrux', description: 'Mature' },
	{ name: 'Pulcherrima', description: 'Forward' },
	{ name: 'Achird', description: 'Friendly' },
	{ name: 'Zubenelgenubi', description: 'Casual' },
	{ name: 'Vindemiatrix', description: 'Gentle' },
	{ name: 'Sadachbia', description: 'Lively' },
	{ name: 'Sadaltager', description: 'Knowledgeable' },
	{ name: 'Sulafat', description: 'Warm' },
] as const

export type VoiceOption = (typeof AVAILABLE_VOICES)[number]

export const DEFAULT_VOICE = 'Aoede'
export const VOICE_LIST_TEXT = AVAILABLE_VOICES.map((v) => `${v.name} (${v.description})`).join(', ')

const VOICE_NAMES = new Set<string>(AVAILABLE_VOICES.map((v) => v.name))

export function resolveVoice(requested: string | null | undefined): string {
	if (requested && VOICE_NAMES.has(requested)) return requested
	return DEFAULT_VOICE
}

export function findVoice(name: string): VoiceOption | undefined {
	return AVAILABLE_VOICES.find((v) => v.name === name)
}

export function handleSetVoiceTool(args: Record<string, unknown>):
	| {
			ok: true
			voice: string
			response: { result: string }
			logResult: string
			status?: undefined
	  }
	| {
			ok: false
			response: { result: string }
			logResult: string
			status: 'error'
	  } {
	const requested = typeof args.name === 'string' ? args.name : ''
	const match = findVoice(requested)
	if (!match) {
		return {
			ok: false,
			response: {
				result: `Unknown voice "${requested}". Available: ${AVAILABLE_VOICES.map((v) => v.name).join(', ')}.`,
			},
			logResult: `unknown voice: ${requested}`,
			status: 'error',
		}
	}

	return {
		ok: true,
		voice: match.name,
		response: {
			result: `Voice set to ${match.name} (${match.description}). Reconnecting.`,
		},
		logResult: match.name,
	}
}
