from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path
from urllib.parse import parse_qsl, urlencode, urlparse, urlunparse


def _bool_env(name: str, default: bool) -> bool:
	value = os.getenv(name)
	if value is None:
		return default
	return value.strip().lower() in {"1", "true", "yes", "on"}


def _float_env(name: str, default: float) -> float:
	value = os.getenv(name)
	if not value:
		return default
	try:
		return float(value)
	except ValueError:
		return default


def _int_env(name: str, default: int) -> int:
	value = os.getenv(name)
	if not value:
		return default
	try:
		return int(value)
	except ValueError:
		return default


def _default_state_dir() -> Path:
	return Path(os.getenv("XDG_STATE_HOME", Path.home() / ".local" / "state")) / "reachy-chat-stick"


@dataclass(frozen=True)
class AppConfig:
	server_url: str
	device_id: str
	device_token: str | None
	chat_id: str | None
	voice: str
	image_width: int
	image_height: int
	media_backend: str
	connection_mode: str
	reachy_host: str
	reachy_port: int
	output_audio_rate: int
	reconnect_delay_s: float
	vad_start_threshold: float
	vad_silence_threshold: float
	vad_start_s: float
	vad_end_silence_s: float
	vad_min_turn_s: float
	vad_max_turn_s: float
	vad_preroll_s: float
	use_doa: bool
	listen_while_speaking: bool
	enable_motion: bool
	log_transcripts: bool
	timer_file: Path

	@classmethod
	def from_env(cls) -> "AppConfig":
		state_dir = Path(os.getenv("CHAT_STICK_REACHY_STATE_DIR", _default_state_dir()))
		return cls(
			server_url=os.getenv("CHAT_STICK_SERVER_URL", "http://localhost:8787"),
			device_id=os.getenv("CHAT_STICK_DEVICE_ID", "reachy-mini"),
			device_token=os.getenv("CHAT_STICK_DEVICE_TOKEN") or None,
			chat_id=os.getenv("CHAT_STICK_CHAT_ID") or None,
			voice=os.getenv("CHAT_STICK_VOICE", "Aoede"),
			image_width=_int_env("CHAT_STICK_IMAGE_WIDTH", 240),
			image_height=_int_env("CHAT_STICK_IMAGE_HEIGHT", 240),
			media_backend=os.getenv("REACHY_MEDIA_BACKEND", "webrtc"),
			connection_mode=os.getenv("REACHY_CONNECTION_MODE", "localhost_only"),
			reachy_host=os.getenv("REACHY_HOST", "reachy-mini.local"),
			reachy_port=_int_env("REACHY_PORT", 8000),
			output_audio_rate=_int_env("CHAT_STICK_OUTPUT_AUDIO_RATE", 24000),
			reconnect_delay_s=_float_env("CHAT_STICK_RECONNECT_DELAY_S", 2.0),
			vad_start_threshold=_float_env("CHAT_STICK_VAD_START_THRESHOLD", 0.020),
			vad_silence_threshold=_float_env("CHAT_STICK_VAD_SILENCE_THRESHOLD", 0.010),
			vad_start_s=_float_env("CHAT_STICK_VAD_START_S", 0.25),
			vad_end_silence_s=_float_env("CHAT_STICK_VAD_END_SILENCE_S", 0.70),
			vad_min_turn_s=_float_env("CHAT_STICK_VAD_MIN_TURN_S", 0.45),
			vad_max_turn_s=_float_env("CHAT_STICK_VAD_MAX_TURN_S", 20.0),
			vad_preroll_s=_float_env("CHAT_STICK_VAD_PREROLL_S", 0.35),
			use_doa=_bool_env("CHAT_STICK_USE_DOA", False),
			listen_while_speaking=_bool_env("CHAT_STICK_LISTEN_WHILE_SPEAKING", False),
			enable_motion=_bool_env("CHAT_STICK_REACHY_MOTION", False),
			log_transcripts=_bool_env("CHAT_STICK_LOG_TRANSCRIPTS", True),
			timer_file=Path(os.getenv("CHAT_STICK_TIMER_FILE", state_dir / "timers.json")),
		)

	def websocket_url(self) -> str:
		parsed = urlparse(self.server_url)
		scheme = {"http": "ws", "https": "wss", "ws": "ws", "wss": "wss"}.get(
			parsed.scheme,
			"ws",
		)
		path = parsed.path or "/"
		if not path.endswith("/ws"):
			path = path.rstrip("/") + "/ws"

		query = dict(parse_qsl(parsed.query, keep_blank_values=True))
		query["device_id"] = self.device_id
		query["voice"] = self.voice
		query["image_w"] = str(self.image_width)
		query["image_h"] = str(self.image_height)
		if self.chat_id:
			query["chat_id"] = self.chat_id
		if self.device_token:
			query["device_token"] = self.device_token

		return urlunparse((scheme, parsed.netloc, path, "", urlencode(query), ""))

	def headers(self) -> dict[str, str]:
		if not self.device_token:
			return {}
		return {"X-Device-Token": self.device_token}
