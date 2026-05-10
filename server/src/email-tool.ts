import { type EmailEnv, sendEmail } from './email'

export async function handleEmailTool(
	env: EmailEnv,
	args: Record<string, unknown>
): Promise<{
	response: { result: string }
	logArgs: { subject: unknown; body_chars: number }
	logResult: string
	status: 'ok' | 'error'
	error?: string
}> {
	const subject = typeof args.subject === 'string' ? args.subject : ''
	const body = typeof args.body === 'string' ? args.body : ''
	const result = await sendEmail(env, subject, body)

	if ('ok' in result) {
		return {
			response: { result: `email sent to ${result.recipient}` },
			logArgs: { subject: args.subject, body_chars: body.length },
			logResult: 'sent',
			status: 'ok',
		}
	}

	return {
		response: { result: `email failed: ${result.error}` },
		logArgs: { subject: args.subject, body_chars: body.length },
		logResult: result.error,
		status: 'error',
		error: result.error,
	}
}
