-- Migration 0002: device-scoped files for AI-managed note/file CRUD
-- Run with: wrangler d1 migrations apply --local
--          wrangler d1 migrations apply --remote

CREATE TABLE IF NOT EXISTS files (
  device_id TEXT NOT NULL,
  path TEXT NOT NULL,
  content TEXT NOT NULL DEFAULT '',
  created_at TEXT DEFAULT (datetime('now')),
  updated_at TEXT DEFAULT (datetime('now')),
  PRIMARY KEY (device_id, path)
);

CREATE INDEX IF NOT EXISTS idx_files_device_updated ON files(device_id, updated_at);
