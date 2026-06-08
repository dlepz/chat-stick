from __future__ import annotations

import asyncio
import json
import logging
import threading
import time
from collections import deque
from contextlib import suppress
from typing import Any

import numpy as np
import websockets

from . import __version__
from .audio import (
	float_samples_to_pcm16,
	melody_to_samples,
	named_sound,
	pcm16_to_reachy_samples,
	rms,
)
from .config import AppConfig
from .timers import TimerManager

logger = logging.getLogger(__name__)


class ReachyChatStickRunner:
	def __init__(self, reachy_mini: Any, stop_event: threading.Event, config: AppConfig):
		self.reachy = reachy_mini
		self.media = reachy_mini.media
		self.stop_event = stop_event
		self.config = config
		self.timers = TimerManager(config.timer_file)
		self.playback_queue: asyncio.Queue[bytes | None] = asyncio.Queue()
		self.chat_id: str | None = config.chat_id
		self.ready = False
		self.recording = False
		self.playing_until = 0.0
		self.input_rate = 16000
		self.output_rate = 16000
		self.output_channels = 1
		self.noise_floor = 0.003
		self.last_input_level = 0.0
		self.use_doa = config.use_doa
		self._motion_ready = False
		self._last_state = "idle"

	async def run(self) -> None:
		await self._start_media()
		try:
			while not self.stop_event.is_set():
				try:
					await self._run_connection()
				except asyncio.CancelledError:
					raise
				except Exception:
					logger.exception("chat-stick connection failed")
				self.ready = False
				if not self.stop_event.is_set():
					await asyncio.sleep(self.config.reconnect_delay_s)
		finally:
			await self._stop_media()

	async def _start_media(self) -> None:
		await asyncio.to_thread(self.media.start_recording)
		await asyncio.to_thread(self.media.start_playing)
		self.input_rate = int(self._call_media_int("get_input_audio_samplerate", 16000))
		self.output_rate = int(self._call_media_int("get_output_audio_samplerate", 16000))
		self.output_channels = max(1, int(self._call_media_int("get_output_channels", 1)))
		logger.info(
			"Reachy media ready: input=%sHz output=%sHz channels=%s",
			self.input_rate,
			self.output_rate,
			self.output_channels,
		)

	async def _stop_media(self) -> None:
		for method in ("stop_recording", "stop_playing"):
			with suppress(Exception):
				await asyncio.to_thread(getattr(self.media, method))

	def _call_media_int(self, method: str, fallback: int) -> int:
		fn = getattr(self.media, method, None)
		if not callable(fn):
			return fallback
		try:
			return int(fn())
		except Exception:
			return fallback

	async def _run_connection(self) -> None:
		url = self.config.websocket_url()
		headers = self.config.headers()
		logger.info("connecting to %s", url)
		ws = await self._connect_ws(url, headers)
		try:
			logger.info("connected to chat-stick")
			tasks = [
				asyncio.create_task(self._receive_loop(ws), name="receive"),
				asyncio.create_task(self._capture_loop(ws), name="capture"),
				asyncio.create_task(self._playback_loop(), name="playback"),
				asyncio.create_task(self._timer_loop(), name="timers"),
			]
			if self.config.enable_motion:
				tasks.append(asyncio.create_task(self._motion_loop(), name="motion"))

			done, pending = await asyncio.wait(tasks, return_when=asyncio.FIRST_EXCEPTION)
			for task in pending:
				task.cancel()
			await asyncio.gather(*pending, return_exceptions=True)
			for task in done:
				task.result()
		finally:
			with suppress(Exception):
				await ws.close()

	async def _connect_ws(self, url: str, headers: dict[str, str]):
		try:
			return await websockets.connect(
				url,
				additional_headers=headers,
				max_size=None,
				ping_interval=20,
				ping_timeout=20,
			)
		except TypeError:
			return await websockets.connect(
				url,
				extra_headers=headers,
				max_size=None,
				ping_interval=20,
				ping_timeout=20,
			)

	async def _receive_loop(self, ws: Any) -> None:
		async for message in ws:
			if isinstance(message, bytes):
				await self.playback_queue.put(message)
				continue
			await self._handle_json_message(ws, message)

	async def _handle_json_message(self, ws: Any, raw: str) -> None:
		try:
			msg = json.loads(raw)
		except json.JSONDecodeError:
			logger.warning("ignoring non-json message: %r", raw[:120])
			return

		msg_type = msg.get("type")
		if msg_type == "session":
			self.chat_id = msg.get("chatId", self.chat_id)
			logger.info("session chat_id=%s reset=%s", self.chat_id, bool(msg.get("reset")))
		elif msg_type == "server_ready":
			logger.info("device channel ready")
		elif msg_type == "ready":
			self.ready = True
			logger.info("Gemini session ready")
		elif msg_type == "turn_complete":
			logger.info("turn complete")
		elif msg_type == "drop_audio":
			logger.info("dropping queued assistant audio")
			await self._clear_playback()
		elif msg_type == "ignore_audio":
			logger.info("server ignored audio: %s", msg.get("reason", "ignored"))
		elif msg_type == "transcript":
			if self.config.log_transcripts:
				logger.info("%s: %s", msg.get("source", "?"), msg.get("text", ""))
		elif msg_type == "tool_call":
			await self._handle_tool_call(ws, msg)
		elif msg_type == "voice_changed":
			logger.info("voice changed to %s", msg.get("voice"))
		elif msg_type == "show_image_pending":
			logger.info("image generation pending")
		elif msg_type == "show_image":
			logger.info(
				"image received for non-display robot: %sx%s",
				msg.get("width"),
				msg.get("height"),
			)
		elif msg_type == "show_image_failed":
			logger.info("image generation failed")
		elif msg_type == "error":
			logger.error("server error [%s] %s", msg.get("category"), msg.get("message"))

	async def _capture_loop(self, ws: Any) -> None:
		pre_roll: deque[tuple[bytes, float]] = deque()
		pre_roll_seconds = 0.0
		speech_seconds = 0.0
		silence_seconds = 0.0
		turn_seconds = 0.0

		while not self.stop_event.is_set():
			samples = await asyncio.to_thread(self.media.get_audio_sample)
			duration_s = self._sample_duration(samples)
			pcm = float_samples_to_pcm16(samples)
			speech = await self._is_speech(samples)

			if self._is_assistant_speaking() and not self.config.listen_while_speaking:
				speech = False

			if self.recording:
				await ws.send(pcm)
				turn_seconds += duration_s
				if speech:
					silence_seconds = 0.0
				else:
					silence_seconds += duration_s

				should_stop = (
					turn_seconds >= self.config.vad_min_turn_s
					and silence_seconds >= self.config.vad_end_silence_s
				) or turn_seconds >= self.config.vad_max_turn_s
				if should_stop:
					await ws.send(json.dumps({"type": "stop"}))
					self.recording = False
					self._last_state = "thinking"
					logger.info("user turn stopped (%.2fs)", turn_seconds)
					speech_seconds = 0.0
					silence_seconds = 0.0
					turn_seconds = 0.0
				continue

			pre_roll.append((pcm, duration_s))
			pre_roll_seconds += duration_s
			while pre_roll_seconds > self.config.vad_preroll_s and pre_roll:
				_, dropped_duration = pre_roll.popleft()
				pre_roll_seconds -= dropped_duration

			if speech:
				speech_seconds += duration_s
			else:
				speech_seconds = max(0.0, speech_seconds - duration_s)

			if speech_seconds >= self.config.vad_start_s:
				await ws.send(json.dumps({"type": "start"}))
				for chunk, _ in pre_roll:
					await ws.send(chunk)
				pre_roll.clear()
				pre_roll_seconds = 0.0
				self.recording = True
				self._last_state = "listening"
				turn_seconds = speech_seconds
				silence_seconds = 0.0
				logger.info(
					"user turn started (level=%.4f noise=%.4f)",
					self.last_input_level,
					self.noise_floor,
				)

	async def _is_speech(self, samples: np.ndarray) -> bool:
		level = rms(samples)
		self.last_input_level = level
		if not self.recording and not self._is_assistant_speaking() and level < 0.08:
			self.noise_floor = self.noise_floor * 0.98 + level * 0.02

		start_threshold = max(self.config.vad_start_threshold, self.noise_floor * 3.0)
		silence_threshold = max(self.config.vad_silence_threshold, self.noise_floor * 1.8)
		threshold = silence_threshold if self.recording else start_threshold
		if level >= threshold:
			return True

		if not self.use_doa:
			return False
		get_doa = getattr(self.media, "get_DoA", None)
		if not callable(get_doa):
			return False
		try:
			_, is_speech_detected = await asyncio.to_thread(get_doa)
			return bool(is_speech_detected)
		except Exception:
			self.use_doa = False
			logger.debug("Reachy DoA unavailable; continuing with RMS VAD")
			return False

	def _sample_duration(self, samples: np.ndarray) -> float:
		try:
			return len(samples) / float(self.input_rate)
		except TypeError:
			return 0.02

	async def _playback_loop(self) -> None:
		while not self.stop_event.is_set():
			data = await self.playback_queue.get()
			if data is None:
				continue
			samples = pcm16_to_reachy_samples(
				data,
				source_rate=self.config.output_audio_rate,
				target_rate=self.output_rate,
				target_channels=self.output_channels,
			)
			if len(samples) == 0:
				continue
			duration_s = len(samples) / float(self.output_rate)
			self.playing_until = max(self.playing_until, time.monotonic()) + duration_s + 0.05
			self._last_state = "speaking"
			await asyncio.to_thread(self.media.push_audio_sample, samples)
			await asyncio.sleep(duration_s)

	def _is_assistant_speaking(self) -> bool:
		return time.monotonic() < self.playing_until or not self.playback_queue.empty()

	async def _clear_playback(self) -> None:
		while not self.playback_queue.empty():
			with suppress(asyncio.QueueEmpty):
				self.playback_queue.get_nowait()
		self.playing_until = 0.0

		with suppress(Exception):
			await asyncio.to_thread(self.media.stop_playing)
			await asyncio.to_thread(self.media.start_playing)

	async def _timer_loop(self) -> None:
		while not self.stop_event.is_set():
			expired = self.timers.harvest_expired()
			for timer in expired:
				logger.info("timer expired: %s", timer.name or timer.id)
				await self._play_samples(named_sound("alert", self.output_rate, self.output_channels))
			await asyncio.sleep(0.25)

	async def _handle_tool_call(self, ws: Any, msg: dict[str, Any]) -> None:
		name = str(msg.get("name", ""))
		call_id = str(msg.get("id", ""))
		args = msg.get("args") if isinstance(msg.get("args"), dict) else {}
		result = await self._tool_result(name, args)
		response = {"type": "tool_response", "name": name, "id": call_id, "result": result}
		await ws.send(json.dumps(response))
		logger.info("tool %s -> %s", name, result)

	async def _tool_result(self, name: str, args: dict[str, Any]) -> str:
		if name == "get_device_status":
			return json.dumps(
				{
					"device": "reachy-mini",
					"app": "reachy-chat-stick",
					"app_version": __version__,
					"chat_id": self.chat_id,
					"input_audio_rate": self.input_rate,
					"output_audio_rate": self.output_rate,
					"output_channels": self.output_channels,
					"recording": self.recording,
					"assistant_speaking": self._is_assistant_speaking(),
					"timers": json.loads(self.timers.describe_all()),
				},
				separators=(",", ":"),
			)

		if name == "set_volume":
			return self._set_volume(args.get("level"))
		if name == "set_brightness":
			return "display brightness is unavailable on Reachy Mini"
		if name == "set_speaker":
			return "speaker routing is fixed to the Reachy Mini speaker"
		if name == "set_external_speaker_gain":
			return "external speaker gain is unavailable on Reachy Mini"
		if name == "show_text":
			text = str(args.get("text", ""))
			logger.info("show_text requested: %s", text)
			return "text logged; Reachy Mini has no chat-stick display"
		if name == "play_sound":
			await self._play_samples(named_sound(str(args.get("sound", "beep")), self.output_rate, self.output_channels))
			return f"Played sound: {args.get('sound', 'beep')}"
		if name == "play_melody":
			notes = str(args.get("notes", "A5:120"))
			await self._play_samples(melody_to_samples(notes, self.output_rate, self.output_channels))
			return "Melody played"
		if name == "power_off":
			self.stop_event.set()
			return "Stopping Reachy chat-stick app"
		if name == "set_timer":
			duration = self._optional_int(args.get("duration_seconds"))
			if duration is None:
				return "Missing duration_seconds"
			return self.timers.add(duration, str(args.get("name", "")))
		if name == "list_timers":
			return self.timers.describe_all()
		if name == "cancel_timer":
			timer_id = self._optional_int(args.get("id"))
			return self.timers.cancel(
				timer_id,
				str(args.get("name", "")),
				bool(args.get("all", False)),
			)
		if name == "extend_timer":
			timer_id = self._optional_int(args.get("id"))
			delta = self._optional_int(args.get("delta_seconds"))
			if delta is None:
				return "Missing delta_seconds"
			return self.timers.extend(
				delta,
				timer_id,
				str(args.get("name", "")),
			)
		return f"{name} is not implemented on Reachy Mini"

	def _optional_int(self, value: Any) -> int | None:
		if value is None or value == "":
			return None
		try:
			return int(value)
		except (TypeError, ValueError):
			return None

	def _set_volume(self, raw_level: Any) -> str:
		try:
			level = max(0, min(255, int(raw_level)))
		except (TypeError, ValueError):
			level = 128
		sdk_percent = round(level * 100 / 255)

		for method_name in ("set_volume", "set_output_volume"):
			method = getattr(self.media, method_name, None)
			if callable(method):
				try:
					method(sdk_percent)
					return f"Volume set to {level} ({sdk_percent}% on Reachy)"
				except Exception:
					logger.debug("media.%s failed", method_name, exc_info=True)
		return "volume control unavailable; Reachy Mini app will keep current speaker volume"

	async def _play_samples(self, samples: np.ndarray) -> None:
		if len(samples) == 0:
			return
		duration_s = len(samples) / float(self.output_rate)
		self.playing_until = max(self.playing_until, time.monotonic()) + duration_s
		await asyncio.to_thread(self.media.push_audio_sample, samples)

	async def _motion_loop(self) -> None:
		create_head_pose = None
		with suppress(Exception):
			from reachy_mini.utils import create_head_pose as imported_create_head_pose

			create_head_pose = imported_create_head_pose
		if create_head_pose is None or not hasattr(self.reachy, "set_target"):
			return

		self._motion_ready = True
		while not self.stop_event.is_set():
			try:
				if self.recording:
					head = create_head_pose(z=8, pitch=-3, degrees=True, mm=True)
					antennas = np.deg2rad([18.0, -18.0])
				else:
					head = create_head_pose()
					antennas = np.deg2rad([0.0, 0.0])
				self.reachy.set_target(head=head, antennas=antennas)
			except Exception:
				logger.debug("motion update failed", exc_info=True)
			await asyncio.sleep(0.10)
