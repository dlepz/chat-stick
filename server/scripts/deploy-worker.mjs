import { spawnSync } from 'node:child_process'
import { resolvedWorkerName } from './worker-name.mjs'

const workerName = resolvedWorkerName()
const args = ['wrangler', 'deploy', '--name', workerName, ...process.argv.slice(2)]

console.log(`Deploying Cloudflare Worker: ${workerName}`)

const result = spawnSync('npx', args, { stdio: 'inherit' })
process.exit(result.status ?? 1)
