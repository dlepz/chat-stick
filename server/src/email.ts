// Optional outbound email via Cloudflare Email Routing.
// Disabled unless the [[send_email]] binding and EMAIL_SENDER/EMAIL_RECIPIENT
// secrets are configured. See README for setup.

import { EmailMessage } from 'cloudflare:email'
import { createMimeMessage } from 'mimetext'

// Minimal local type for the binding so we don't depend on a global SendEmail.
interface SendEmailBinding {
	send(message: EmailMessage): Promise<void>
}

export interface EmailEnv {
	EMAIL?: SendEmailBinding
	EMAIL_SENDER?: string
	EMAIL_RECIPIENT?: string
}

export const MAX_EMAIL_BODY_BYTES = 50_000

export function emailEnabled(env: EmailEnv): boolean {
	return Boolean(env.EMAIL && env.EMAIL_SENDER && env.EMAIL_RECIPIENT)
}

export async function sendEmail(
	env: EmailEnv,
	subject: string,
	body: string
): Promise<{ ok: true; recipient: string } | { error: string }> {
	if (!emailEnabled(env)) {
		return { error: 'email is not configured on this server' }
	}
	const sender = env.EMAIL_SENDER!
	const recipient = env.EMAIL_RECIPIENT!
	const trimmedSubject = (subject || '').trim() || '(no subject)'
	const trimmedBody = body || ''
	if (trimmedBody.length > MAX_EMAIL_BODY_BYTES) {
		return {
			error: `body too large: ${trimmedBody.length} bytes (max ${MAX_EMAIL_BODY_BYTES})`,
		}
	}

	const msg = createMimeMessage()
	msg.setSender({ name: 'chat-stick', addr: sender })
	msg.setRecipient(recipient)
	msg.setSubject(trimmedSubject)
	msg.addMessage({ contentType: 'text/plain', data: trimmedBody })

	try {
		await env.EMAIL!.send(new EmailMessage(sender, recipient, msg.asRaw()))
		return { ok: true, recipient }
	} catch (err) {
		return { error: err instanceof Error ? err.message : String(err) }
	}
}
