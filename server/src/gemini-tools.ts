export function buildToolResponsePayload(
	name: string,
	id: string,
	response: unknown,
): string {
	return JSON.stringify({
		toolResponse: {
			functionResponses: [{ name, id, response }],
		},
	})
}
