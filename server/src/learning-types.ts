export type LearningResourceSource = 'worksheet' | 'roleplay' | 'graded_reader'

export interface LearningResourceSummary {
	resourceId: string
	source: LearningResourceSource
	sourceId: string
	title: string
	subtitle?: string
	description?: string
	level?: string
	language?: string
	tags: string[]
	matchedText?: string
	score: number
	image?: string
	updatedAt?: string
}

export interface LessonContextBundle {
	resourceId: string
	source: LearningResourceSource
	sourceId: string
	title: string
	level?: string
	language?: string
	description?: string
	tutorMode: 'roleplay' | 'lesson' | 'reader_discussion' | 'exam_practice'
	instructions: string
	vocabulary: Array<{ term: string; translation?: string; note?: string }>
	phrases: Array<{ label?: string; text: string }>
	dialogues: Array<{
		title?: string
		exchanges: Array<{ speaker?: string; text: string }>
	}>
	passages: Array<{
		title?: string
		sentences: Array<{ german: string; english?: string; speaker?: string }>
	}>
	exercises: Array<{
		type?: string
		question: string
		options?: string[]
		correctAnswer?: string
		hint?: string
	}>
	promptContext: string
	metadata: {
		totalChars: number
		truncated: boolean
		sourceUpdatedAt?: string
		[key: string]: unknown
	}
}

export interface SearchLearningResourcesResponse {
	query: string
	deviceId: string
	results: LearningResourceSummary[]
}

export interface LoadLearningResourceContextResponse {
	context: LessonContextBundle
}

export interface ReaderPassageSearchResult {
	resourceId: string
	storyId: string
	passageIndex: number
	sentenceIndex?: number
	title?: string
	quote: string
	snippet: string
	score: number
}

export interface SearchReaderPassagesResponse {
	resourceId: string
	query: string
	results: ReaderPassageSearchResult[]
}
