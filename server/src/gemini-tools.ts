import { USER_INSTRUCTIONS_PATH } from './files'
import { VOICE_LIST_TEXT } from './prompt-builder'

type FunctionDeclaration = {
	name: string
	description: string
	parameters: Record<string, unknown>
}

export type GeminiTool =
	| { googleSearch: Record<string, never> }
	| { functionDeclarations: FunctionDeclaration[] }

export function buildGeminiTools({
	canEmail,
}: {
	canEmail: boolean
}): GeminiTool[] {
	const declarations: FunctionDeclaration[] = [
		{
			name: 'set_brightness',
			description: 'Set the display backlight brightness',
			parameters: {
				type: 'OBJECT',
				properties: {
					level: {
						type: 'INTEGER',
						description: 'Brightness level from 0 (off) to 255 (maximum)',
					},
				},
				required: ['level'],
			},
		},
		{
			name: 'set_volume',
			description: 'Set the speaker volume',
			parameters: {
				type: 'OBJECT',
				properties: {
					level: {
						type: 'INTEGER',
						description: 'Volume level from 0 (mute) to 255 (maximum)',
					},
				},
				required: ['level'],
			},
		},
		{
			name: 'set_voice',
			description:
				'Change the voice used for speech output. Persists across sessions. Causes a brief reconnect, so say a short acknowledgement before calling. The new voice will speak the next response.',
			parameters: {
				type: 'OBJECT',
				properties: {
					name: {
						type: 'STRING',
						description: `Voice name. Must match exactly (case-sensitive). Available voices and character: ${VOICE_LIST_TEXT}.`,
					},
				},
				required: ['name'],
			},
		},
		{
			name: 'show_text',
			description:
				'Display a short text message on the device screen. Max ~30 characters per line and about 26 visible lines with header/footer.',
			parameters: {
				type: 'OBJECT',
				properties: {
					text: {
						type: 'STRING',
						description: 'Text to display',
					},
				},
				required: ['text'],
			},
		},
		{
			name: 'show_image',
			description:
				"Generate and display an image on the device screen as a 1-bit dithered bitmap. Provide a detailed visual description. Use only when the user explicitly asks for a picture, drawing, or visual — examples: 'a cute cartoon cat sitting', 'a simple mountain landscape'. Generation takes ~10 seconds; you can keep talking while it generates and the device shows a pulse animation. The image and any text from this turn are paged together.",
			parameters: {
				type: 'OBJECT',
				properties: {
					prompt: {
						type: 'STRING',
						description:
							'Visual description for the image. Be specific and concrete — output is monochrome with high contrast.',
					},
				},
				required: ['prompt'],
			},
		},
		{
			name: 'play_sound',
			description: 'Play a named sound effect on the device speaker.',
			parameters: {
				type: 'OBJECT',
				properties: {
					sound: {
						type: 'STRING',
						description: 'One of: beep, success, error, alert, fanfare',
					},
				},
				required: ['sound'],
			},
		},
		{
			name: 'play_melody',
			description:
				'Play a short melody on the device speaker using note tokens like "C4:200 E4:200 G4:400". Use R for rests.',
			parameters: {
				type: 'OBJECT',
				properties: {
					notes: {
						type: 'STRING',
						description:
							'Space-separated note tokens with durations in milliseconds, for example "C4:200 E4:200 G4:400"',
					},
				},
				required: ['notes'],
			},
		},
		{
			name: 'power_off',
			description: 'Power the device off immediately.',
			parameters: {
				type: 'OBJECT',
				properties: {},
			},
		},
		{
			name: 'get_device_status',
			description:
				'Get current device status including firmware_version, battery level, power source, volume, brightness, voice, WiFi network, and uptime.',
			parameters: {
				type: 'OBJECT',
				properties: {},
			},
		},
		{
			name: 'search_docs',
			description:
				'Search the indexed knowledge base. Returns relevant documentation entries matching the query.',
			parameters: {
				type: 'OBJECT',
				properties: {
					query: {
						type: 'STRING',
						description: 'Search query — keywords or a question',
					},
				},
				required: ['query'],
			},
		},
		{
			name: 'web_fetch',
			description:
				'Fetch a web page or API endpoint and return cleaned text content. Use this to look up current information, read articles, or access APIs.',
			parameters: {
				type: 'OBJECT',
				properties: {
					url: {
						type: 'STRING',
						description: 'The full URL to fetch (must include https:// or http://)',
					},
					max_chars: {
						type: 'INTEGER',
						description:
							'Maximum characters of cleaned text to return, from 500 to 10000. Defaults to 4000.',
					},
				},
				required: ['url'],
			},
		},
		{
			name: 'list_files',
			description: `List all files this device has saved on the server, including the reserved ${USER_INSTRUCTIONS_PATH} behavior-instructions file. Returns each file's path, byte size, and last-updated time. Use this to show the user what they have stored, or before reading a file when you don't know the exact path.`,
			parameters: {
				type: 'OBJECT',
				properties: {},
				required: [],
			},
		},
		{
			name: 'read_file',
			description: `Read the full contents of a previously-saved file by path, including ${USER_INSTRUCTIONS_PATH} when you need to inspect behavior instructions.`,
			parameters: {
				type: 'OBJECT',
				properties: {
					path: {
						type: 'STRING',
						description:
							'The file path to read, e.g. "notes.txt" or "journal/2026-05.md".',
					},
				},
				required: ['path'],
			},
		},
		{
			name: 'write_file',
			description: `Create or overwrite a file with the given content. Use this when the user asks to save, store, or remember something as a named file. Overwrites the entire file if it exists. Prefer append_to_file for adding to logs or journals. Use ${USER_INSTRUCTIONS_PATH} for persistent behavior instructions, and do not delete it.`,
			parameters: {
				type: 'OBJECT',
				properties: {
					path: {
						type: 'STRING',
						description:
							'The file path. Short, descriptive paths like "shopping-list.txt" or "ideas/marketing.md" work well.',
					},
					content: {
						type: 'STRING',
						description: 'The full text content to write to the file.',
					},
				},
				required: ['path', 'content'],
			},
		},
		{
			name: 'append_to_file',
			description: `Append content to the end of a file (creates it if missing). Use this for journals, logs, running lists, or adding persistent behavior preferences to ${USER_INSTRUCTIONS_PATH} without rewriting it. Add your own newlines or separators in the content if needed.`,
			parameters: {
				type: 'OBJECT',
				properties: {
					path: {
						type: 'STRING',
						description: 'The file path to append to.',
					},
					content: {
						type: 'STRING',
						description:
							'Text to append. Include leading newline if you want a line break before it.',
					},
				},
				required: ['path', 'content'],
			},
		},
		{
			name: 'search_files',
			description:
				"Search across all of this device's saved files for a phrase or keyword. Returns matching file paths with a short snippet of context. Use this when the user asks about something they wrote down but you don't know which file it's in.",
			parameters: {
				type: 'OBJECT',
				properties: {
					query: {
						type: 'STRING',
						description: 'Phrase or keyword to find (case-insensitive substring match).',
					},
				},
				required: ['query'],
			},
		},
		{
			name: 'new_conversation',
			description:
				'Start a fresh conversation. Call this when the user wants to reset the chat or change topics completely. Say a brief goodbye before calling this tool.',
			parameters: {
				type: 'OBJECT',
				properties: {},
			},
		},
		{
			name: 'new_chat',
			description: 'Start a fresh conversation. Alias for new_conversation.',
			parameters: {
				type: 'OBJECT',
				properties: {},
			},
		},
	]

	if (canEmail) {
		declarations.push({
			name: 'email_me',
			description:
				"Send a short plain-text email to the user's configured email address. Use only when the user explicitly asks to be emailed. Both subject and body are required.",
			parameters: {
				type: 'OBJECT',
				properties: {
					subject: {
						type: 'STRING',
						description: 'Short subject line, ideally under 80 characters.',
					},
					body: {
						type: 'STRING',
						description:
							'Plain-text email body. Keep it concise — the user dictated this by voice.',
					},
				},
				required: ['subject', 'body'],
			},
		})
	}

	return [{ googleSearch: {} }, { functionDeclarations: declarations }]
}
