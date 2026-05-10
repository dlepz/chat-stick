import { AVAILABLE_VOICES, findVoice } from './prompt-builder'

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
