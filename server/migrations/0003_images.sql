-- Migration 0003: device-scoped image history for show_image / search_images / show_saved_image
-- Run with: wrangler d1 migrations apply --local
--          wrangler d1 migrations apply --remote

CREATE TABLE IF NOT EXISTS images (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  device_id TEXT NOT NULL,
  chat_id TEXT,
  prompt TEXT NOT NULL,
  enhanced_prompt TEXT,
  dithered_key TEXT,
  original_key TEXT,
  packed_bits TEXT NOT NULL,
  width INTEGER NOT NULL,
  height INTEGER NOT NULL,
  created_at TEXT DEFAULT (datetime('now'))
);

CREATE INDEX IF NOT EXISTS idx_images_device_created ON images(device_id, created_at);
