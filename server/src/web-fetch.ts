export interface WebFetchArgs {
	url?: string
	max_chars?: number
}

export interface WebFetchResult {
	url: string
	status: number
	content_type: string
	title: string | null
	content: string
	truncated: boolean
}

export async function fetchWebPage(
	url: string,
	maxChars = 4000
): Promise<WebFetchResult> {
	const normalizedMaxChars = Math.max(500, Math.min(maxChars || 4000, 10000))

	try {
		if (!/^https?:\/\//i.test(url)) {
			return {
				url,
				status: 0,
				content_type: 'error',
				title: null,
				content: 'Error: URL must start with http:// or https://',
				truncated: false,
			}
		}
		const controller = new AbortController()
		const timeout = setTimeout(() => controller.abort(), 10000)
		const resp = await fetch(url, {
			headers: {
				'User-Agent': 'm5-live-assistant/1.0',
				Accept: 'text/html,application/json,text/plain,*/*',
			},
			signal: controller.signal,
		})
		clearTimeout(timeout)
		const contentType = resp.headers.get('content-type') || ''
		const title = extractHtmlTitle(await resp.clone().text(), contentType)
		let body = await resp.text()

		if (contentType.includes('html')) {
			// Strip scripts/styles and tags; collapse whitespace.
			body = body
				.replace(/<script[\s\S]*?<\/script>/gi, ' ')
				.replace(/<style[\s\S]*?<\/style>/gi, ' ')
				.replace(/<noscript[\s\S]*?<\/noscript>/gi, ' ')
				.replace(/<[^>]+>/g, ' ')
				.replace(/&nbsp;/g, ' ')
				.replace(/&amp;/g, '&')
				.replace(/&lt;/g, '<')
				.replace(/&gt;/g, '>')
				.replace(/&quot;/g, '"')
				.replace(/&#39;/g, "'")
				.replace(/\s+/g, ' ')
				.trim()
		} else {
			body = body.replace(/\s+/g, ' ').trim()
		}

		const truncated = body.length > normalizedMaxChars
		if (truncated) {
			body = body.slice(0, normalizedMaxChars) + '... [truncated]'
		}

		if (!resp.ok) {
			body = body || `Error: HTTP ${resp.status} ${resp.statusText}`
		}

		return {
			url: resp.url,
			status: resp.status,
			content_type: contentType || 'unknown',
			title,
			content: body || '(empty response)',
			truncated,
		}
	} catch (err) {
		return {
			url,
			status: 0,
			content_type: 'error',
			title: null,
			content: `Error fetching URL: ${err instanceof Error ? err.message : String(err)}`,
			truncated: false,
		}
	}
}

function extractHtmlTitle(body: string, contentType: string): string | null {
	if (!contentType.includes('html')) return null

	const match = body.match(/<title[^>]*>([\s\S]*?)<\/title>/i)
	if (!match) return null

	return match[1].replace(/\s+/g, ' ').trim() || null
}
