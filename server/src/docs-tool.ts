import { searchDocsKeyword, searchDocsVector } from './docs-search'

interface DocsToolEnv {
	AI: Ai
	VECTORIZE: VectorizeIndex
}

interface DocsSearchResult {
	title: string
	section: string
	content: string
	score: number
}

export async function handleDocsSearchTool(
	env: DocsToolEnv,
	args: Record<string, unknown>
): Promise<{
	response: { results: DocsSearchResult[] }
	logResult: { mode: 'vector' | 'keyword'; count: number; titles: string[] }
}> {
	const query = typeof args.query === 'string' ? args.query : ''
	console.log(`[Gemini] Docs search: "${query}"`)

	let results: DocsSearchResult[] = []
	let searchMode: 'vector' | 'keyword' = 'vector'

	try {
		results = await searchDocsVector(query, env, 3)
	} catch (err) {
		console.warn('[Gemini] Vector search failed, falling back to keyword search:', err)
	}

	if (results.length === 0) {
		searchMode = 'keyword'
		results = searchDocsKeyword(query, 3)
	}

	console.log(
		`[Gemini] Found ${results.length} ${searchMode} results (top: ${results[0]?.title})`
	)

	const searchResults = results.map((r) => ({
		title: r.title,
		section: r.section,
		content: r.content.slice(0, 1000),
		score: r.score,
	}))

	return {
		response: { results: searchResults },
		logResult: {
			mode: searchMode,
			count: searchResults.length,
			titles: searchResults.map((r) => r.title),
		},
	}
}
