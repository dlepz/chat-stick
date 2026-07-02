import { spawnSync } from 'node:child_process'
import { fileURLToPath } from 'node:url'

const BASE_WORKER_NAME = 'm5-live'
const MAIN_BRANCHES = new Set(['main', 'master'])

function currentBranch() {
	const result = spawnSync('git', ['branch', '--show-current'], {
		stdio: 'pipe',
		encoding: 'utf8',
	})
	if (result.status !== 0) {
		return ''
	}
	return result.stdout.trim()
}

function branchWorkerName(branch) {
	if (!branch || MAIN_BRANCHES.has(branch)) {
		return BASE_WORKER_NAME
	}

	const slug = branch
		.toLowerCase()
		.replace(/[^a-z0-9-]+/g, '-')
		.replace(/^-+|-+$/g, '')
		.slice(0, 48)

	return slug ? `${BASE_WORKER_NAME}-${slug}` : BASE_WORKER_NAME
}

export function resolvedWorkerName() {
	return process.env.WORKER_NAME || branchWorkerName(currentBranch())
}

if (process.argv[1] === fileURLToPath(import.meta.url)) {
	console.log(resolvedWorkerName())
}
