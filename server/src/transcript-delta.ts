export interface TranscriptDelta {
	text: string
	delta: string
}

export function appendTranscriptDelta(current: string, incoming: string): TranscriptDelta {
	if (!incoming) {
		return { text: current, delta: '' }
	}
	if (!current) {
		return { text: incoming, delta: incoming }
	}

	// Live transcription messages can arrive as true deltas or as cumulative /
	// overlapping partial hypotheses. The device expects append-only text.
	if (incoming === current || current.startsWith(incoming)) {
		return { text: current, delta: '' }
	}
	if (incoming.startsWith(current)) {
		const delta = incoming.slice(current.length)
		return { text: incoming, delta }
	}
	if (current.endsWith(incoming)) {
		return { text: current, delta: '' }
	}

	const maxOverlap = Math.min(current.length, incoming.length)
	for (let len = maxOverlap; len >= 1; len--) {
		if (current.endsWith(incoming.slice(0, len))) {
			const delta = incoming.slice(len)
			return { text: current + delta, delta }
		}
	}

	return { text: current + incoming, delta: incoming }
}
