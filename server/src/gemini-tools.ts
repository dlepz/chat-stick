import { USER_INSTRUCTIONS_PATH } from './files'
import {
	DEFAULT_THINKING_LEVEL,
	THINKING_LEVEL_LIST_TEXT,
} from './prompt-builder'
import { VOICE_LIST_TEXT } from './voice-tool'

export function buildGeminiTools({ canEmail }: { canEmail: boolean }) {
	return [
		{ googleSearch: {} },
		{
			functionDeclarations: [
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
					name: 'set_speaker',
					description:
						'Switch audio output between the StickS3 built-in speaker and an attached M5Stack HAT SPK2 external speaker. The user must physically attach the HAT before selecting "external" for audio to be audible. Check get_device_status to see which mode is currently active.',
					parameters: {
						type: 'OBJECT',
						properties: {
							mode: {
								type: 'STRING',
								description:
									'"internal" for the built-in stick speaker, or "external" for the attached SPK2 HAT.',
							},
						},
						required: ['mode'],
					},
				},
				{
					name: 'set_external_speaker_gain',
					description:
						'Adjust the software gain applied to audio sent to the external SPK2 HAT. Only effective when the speaker mode is "external". Raise to increase loudness; lower if you hear clipping or distortion. Typical usable range is 8 to 40.',
					parameters: {
						type: 'OBJECT',
						properties: {
							gain: {
								type: 'INTEGER',
								description: 'Gain multiplier, integer 1..64. Default is 24.',
							},
						},
						required: ['gain'],
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
					name: 'set_thinking_level',
					description: `Set Gemini thinking depth for this conversation only. New conversations always reset to "${DEFAULT_THINKING_LEVEL}". Changing the level applies after a brief reconnect. Available levels: ${THINKING_LEVEL_LIST_TEXT}.`,
					parameters: {
						type: 'OBJECT',
						properties: {
							level: {
								type: 'STRING',
								description: `One of: ${THINKING_LEVEL_LIST_TEXT}. Use minimal for fastest replies, high for harder reasoning.`,
							},
						},
						required: ['level'],
					},
				},
				{
					name: 'show_text',
					description:
						'Display a short text message on the device screen. Max ~20 characters per line, 7 lines.',
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
						"Generate and display an image on the device screen as a 1-bit dithered bitmap. Provide a detailed visual description. Use when the user explicitly asks for a picture, drawing, visual, or an image of a vocabulary word — examples: 'a cute cartoon cat sitting', 'a simple mountain landscape', 'a bitten apple on a table for the German word Apfel'. For vocab words, depict the meaning as an object/action/scene; do not ask for text labels. Generation takes ~10 seconds; you can keep talking while it generates and the device shows a pulse animation. The image and any text from this turn are paged together. Past images are saved automatically; use list_recent_images / search_images / show_saved_image to recall them.",
					parameters: {
						type: 'OBJECT',
						properties: {
							prompt: {
								type: 'STRING',
								description:
									'Visual description for the image. Be specific and concrete — output is monochrome with high contrast. For vocab words, describe the thing or action, not written text.',
							},
						},
						required: ['prompt'],
					},
				},
				{
					name: 'list_recent_images',
					description:
						'List the most recently generated images for this device. Returns id, prompt, and creation time per image. Use when the user wants to revisit recent visuals or you need an id for show_saved_image.',
					parameters: {
						type: 'OBJECT',
						properties: {
							limit: {
								type: 'INTEGER',
								description: 'How many recent images to return. Default 10, max 50.',
							},
						},
					},
				},
				{
					name: 'search_images',
					description:
						"Search past image prompts for this device by keyword (case-insensitive substring). Returns matching ids, prompts, and snippets. Use when the user refers to an old image by topic — e.g. 'show me that mountain one again'.",
					parameters: {
						type: 'OBJECT',
						properties: {
							query: {
								type: 'STRING',
								description: 'Keyword or phrase to search for across past image prompts.',
							},
							limit: {
								type: 'INTEGER',
								description: 'Max number of matches to return. Default 10, max 50.',
							},
						},
						required: ['query'],
					},
				},
				{
					name: 'show_saved_image',
					description:
						'Re-display a previously generated image on the device, identified by id (from list_recent_images or search_images). Instant — no regeneration. Use when the user wants to see a past image again.',
					parameters: {
						type: 'OBJECT',
						properties: {
							id: {
								type: 'INTEGER',
								description:
									'The image id returned by list_recent_images or search_images.',
							},
						},
						required: ['id'],
					},
				},
				{
					name: 'set_timer',
					description:
						'Start a countdown timer on the device. The device beeps an alarm when the timer expires. Timers persist across chat sessions and reboots. Optionally give the timer a short name so the user can refer to it later ("the eggs timer"). Returns the resulting timer list. Always pass the duration in seconds — for "2 minutes" pass 120, "an hour and a half" pass 5400.',
					parameters: {
						type: 'OBJECT',
						properties: {
							duration_seconds: {
								type: 'INTEGER',
								description:
									'Timer length in whole seconds. Minimum 1, maximum 86400 (24 hours).',
							},
							name: {
								type: 'STRING',
								description:
									'Optional short label, e.g. "eggs", "laundry". Lowercase preferred. Must be unique among active timers; omit for an unnamed timer.',
							},
						},
						required: ['duration_seconds'],
					},
				},
				{
					name: 'list_timers',
					description:
						'List all timers currently running on the device. Each entry includes id (stable numeric handle for cancel_timer / extend_timer), name, duration_seconds, remaining_seconds, and created_at_epoch. Call this whenever the user asks about active timers ("what timers are running?", "how much is left?", "what\'s on the eggs timer?"), or whenever you need an id to act on a specific timer. The device is the source of truth — do not rely on past values.',
					parameters: {
						type: 'OBJECT',
						properties: {},
					},
				},
				{
					name: 'cancel_timer',
					description:
						'Cancel a timer. Pass `id` (preferred; obtained from list_timers) or `name` (case-insensitive). When only one timer is active you may omit both. Set all=true to cancel everything. If the user refers to a timer ambiguously (e.g. "the shorter one"), call list_timers first, pick the right entry yourself, then pass its id. Returns the resulting timer list.',
					parameters: {
						type: 'OBJECT',
						properties: {
							id: {
								type: 'INTEGER',
								description:
									'Stable numeric id of the timer to cancel, from list_timers. Preferred over name when the user refers to a timer indirectly.',
							},
							name: {
								type: 'STRING',
								description:
									'Name of the timer to cancel. Case-insensitive. Use when the user named the timer explicitly. Ignored if id is provided.',
							},
							all: {
								type: 'BOOLEAN',
								description: 'If true, cancel every active timer regardless of id/name.',
							},
						},
					},
				},
				{
					name: 'extend_timer',
					description:
						'Adjust a running timer by adding or subtracting seconds. Positive values extend, negative values shorten. Pass `id` (preferred; obtained from list_timers) or `name`. When only one timer is active you may omit both. Use this for "add five more minutes" (delta_seconds=300) or "take a minute off the eggs timer" (delta_seconds=-60, name="eggs"). For ambiguous references, call list_timers first and pass the chosen id. Returns the resulting timer list.',
					parameters: {
						type: 'OBJECT',
						properties: {
							delta_seconds: {
								type: 'INTEGER',
								description:
									'Seconds to add (positive) or subtract (negative). Cannot drop the remaining time below 1 second.',
							},
							id: {
								type: 'INTEGER',
								description:
									'Stable numeric id of the timer to adjust, from list_timers. Preferred over name when the user refers to a timer indirectly.',
							},
							name: {
								type: 'STRING',
								description: 'Name of the timer to adjust. Ignored if id is provided.',
							},
						},
						required: ['delta_seconds'],
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
						'Get current device status including firmware_version, battery level, volume, brightness, speaker mode, voice, WiFi network, and uptime.',
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
						name: 'search_learning_resources',
						description:
							'Search the user\'s German learning resources: worksheet lessons, roleplays, dialogues, mock-test practice, and graded readers. Use this when the user wants to find or choose a lesson to practice.',
						parameters: {
							type: 'OBJECT',
							properties: {
								query: {
									type: 'STRING',
									description:
										'Search query, e.g. cafe, doctor appointment, B1 Bildbeschreibung, party planning, available graded readers.',
								},
								limit: {
									type: 'INTEGER',
									description: 'Maximum results to return, default 5.',
								},
								source: {
									type: 'STRING',
									description:
										'Optional filter: worksheet, roleplay, or graded_reader. Use graded_reader when the user asks to list or browse available readers/books/stories.',
								},
							},
							required: ['query'],
						},
					},
					{
						name: 'load_learning_resource',
						description:
							'Load the full practice context for a selected learning resource so you can run the lesson, roleplay, dialogue practice, or reader discussion.',
						parameters: {
							type: 'OBJECT',
							properties: {
								resource_id: {
									type: 'STRING',
									description:
										'Resource id returned by search_learning_resources, e.g. worksheet:cafe or graded_reader:abc123.',
								},
								max_chars: {
									type: 'INTEGER',
									description: 'Maximum prompt context characters to load, default 8000.',
								},
							},
							required: ['resource_id'],
						},
					},
					{
						name: 'search_reader_passages',
						description:
							'Search inside the full passages/sentences of a loaded or selected graded reader. Use this when the user asks for the part where the reader says something, or asks to find a topic inside a long reader/book.',
						parameters: {
							type: 'OBJECT',
							properties: {
								resource_id: {
									type: 'STRING',
									description:
										'A graded reader resource id, e.g. graded_reader:abc123. If omitted, use the currently loaded graded reader when available.',
								},
								query: {
									type: 'STRING',
									description:
										'Terms to find inside the reader passages, e.g. arches, hotel, where it says xyz.',
								},
								limit: {
									type: 'INTEGER',
									description: 'Maximum matching passages to return, default 5.',
								},
							},
							required: ['query'],
						},
					},
					{
						name: 'load_reader_passage',
						description:
							'Load a focused context window around a specific graded reader passage/sentence returned by search_reader_passages so you can read or discuss that part.',
						parameters: {
							type: 'OBJECT',
							properties: {
								resource_id: {
									type: 'STRING',
									description: 'A graded reader resource id, e.g. graded_reader:abc123.',
								},
								passage_index: {
									type: 'INTEGER',
									description: 'Zero-based passage index returned by search_reader_passages.',
								},
								sentence_index: {
									type: 'INTEGER',
									description:
										'Optional zero-based sentence index returned by search_reader_passages; loads context around this sentence.',
								},
								max_chars: {
									type: 'INTEGER',
									description: 'Maximum context characters, default 8000.',
								},
							},
							required: ['resource_id', 'passage_index'],
						},
					},
					{
						name: 'get_current_learning_resource',
						description: 'Return the currently loaded lesson/resource context, if one is active.',
						parameters: { type: 'OBJECT', properties: {} },
					},
					{
						name: 'clear_learning_resource',
						description:
							'Clear the currently loaded lesson/resource context and return to general assistant mode.',
						parameters: { type: 'OBJECT', properties: {} },
					},
					{
						name: 'save_flashcard',
						description:
							'Save a flashcard derived from the recent exchange. Use this when the learner asks to save a phrase, save a correction, or save the most useful item from the conversation. Pick the highest-value item and generate concise front/back text. After calling, briefly confirm what you saved.',
						parameters: {
							type: 'OBJECT',
							properties: {
								front: {
									type: 'STRING',
									description:
										'Front of the card — target-language phrase, learner sentence, or English prompt.',
								},
								back: {
									type: 'STRING',
									description:
										'Back of the card — translation, correction, or target-language answer.',
								},
								tags: {
									type: 'ARRAY',
									items: { type: 'STRING' },
									description:
										'Short tags describing the card, e.g. ["roleplay", "im-cafe", "polite-request"].',
								},
								source: {
									type: 'STRING',
									description: 'Optional context label, e.g. "roleplay:im-cafe" or "correction".',
								},
								note: {
									type: 'STRING',
									description: 'Optional short note giving usage context.',
								},
							},
							required: ['front', 'back'],
						},
					},
					{
						name: 'end_practice_review',
						description:
							'Finish the current German practice session and get a concise transcript-based feedback review with mistakes and a grade. Use when the learner says they are done, asks to end/review/get graded, or when an enabled practice task reaches a natural ending.',
						parameters: {
							type: 'OBJECT',
							properties: {},
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
								description:
									'Phrase or keyword to find (case-insensitive substring match).',
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
				...(canEmail
					? [
							{
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
							},
						]
					: []),
			],
		},
	]
}

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
