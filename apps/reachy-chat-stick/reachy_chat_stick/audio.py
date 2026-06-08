from __future__ import annotations

import math
from collections.abc import Iterable

import numpy as np


def samples_to_mono_float32(samples: np.ndarray) -> np.ndarray:
	arr = np.asarray(samples, dtype=np.float32)
	if arr.ndim == 0:
		mono = arr.reshape(1)
	elif arr.ndim == 1:
		mono = arr
	else:
		mono = arr.mean(axis=1)
	return np.nan_to_num(mono, nan=0.0, posinf=1.0, neginf=-1.0)


def rms(samples: np.ndarray) -> float:
	mono = samples_to_mono_float32(samples)
	if mono.size == 0:
		return 0.0
	return float(np.sqrt(np.mean(np.square(mono, dtype=np.float32))))


def float_samples_to_pcm16(samples: np.ndarray) -> bytes:
	mono = samples_to_mono_float32(samples)
	clipped = np.clip(mono, -1.0, 1.0)
	ints = (clipped * 32767.0).astype("<i2", copy=False)
	return ints.tobytes()


def pcm16_to_float_samples(data: bytes) -> np.ndarray:
	if not data:
		return np.zeros((0,), dtype=np.float32)
	ints = np.frombuffer(data, dtype="<i2")
	return (ints.astype(np.float32) / 32768.0).clip(-1.0, 1.0)


def resample_linear(samples: np.ndarray, source_rate: int, target_rate: int) -> np.ndarray:
	if source_rate == target_rate or len(samples) == 0:
		return samples.astype(np.float32, copy=False)

	duration = len(samples) / float(source_rate)
	target_len = max(1, int(round(duration * target_rate)))
	source_positions = np.linspace(0.0, len(samples) - 1, num=len(samples), dtype=np.float32)
	target_positions = np.linspace(0.0, len(samples) - 1, num=target_len, dtype=np.float32)
	return np.interp(target_positions, source_positions, samples).astype(np.float32)


def mono_to_channels(samples: np.ndarray, channels: int) -> np.ndarray:
	mono = samples.astype(np.float32, copy=False).reshape(-1, 1)
	if channels <= 1:
		return mono
	return np.repeat(mono, channels, axis=1)


def pcm16_to_reachy_samples(
	data: bytes,
	source_rate: int,
	target_rate: int,
	target_channels: int,
) -> np.ndarray:
	mono = pcm16_to_float_samples(data)
	resampled = resample_linear(mono, source_rate, target_rate)
	return mono_to_channels(resampled, target_channels)


NOTE_OFFSETS = {
	"C": -9,
	"C#": -8,
	"DB": -8,
	"D": -7,
	"D#": -6,
	"EB": -6,
	"E": -5,
	"F": -4,
	"F#": -3,
	"GB": -3,
	"G": -2,
	"G#": -1,
	"AB": -1,
	"A": 0,
	"A#": 1,
	"BB": 1,
	"B": 2,
}


def note_frequency(note: str) -> float | None:
	token = note.strip().upper()
	if token in {"R", "REST", "-"}:
		return None

	name = token[:-1]
	octave_text = token[-1:]
	if not octave_text.isdigit() or name not in NOTE_OFFSETS:
		return None

	octave = int(octave_text)
	semitones_from_a4 = NOTE_OFFSETS[name] + (octave - 4) * 12
	return 440.0 * (2.0 ** (semitones_from_a4 / 12.0))


def make_tone(
	frequency_hz: float,
	duration_ms: int,
	sample_rate: int,
	channels: int,
	amplitude: float = 0.20,
) -> np.ndarray:
	sample_count = max(1, int(sample_rate * duration_ms / 1000))
	t = np.arange(sample_count, dtype=np.float32) / float(sample_rate)
	wave = np.sin(2.0 * math.pi * frequency_hz * t) * amplitude

	fade_len = min(sample_count // 8, int(sample_rate * 0.01))
	if fade_len > 1:
		fade = np.linspace(0.0, 1.0, fade_len, dtype=np.float32)
		wave[:fade_len] *= fade
		wave[-fade_len:] *= fade[::-1]

	return mono_to_channels(wave.astype(np.float32), channels)


def silence(duration_ms: int, sample_rate: int, channels: int) -> np.ndarray:
	sample_count = max(1, int(sample_rate * duration_ms / 1000))
	return np.zeros((sample_count, max(1, channels)), dtype=np.float32)


def melody_to_samples(
	notes: str,
	sample_rate: int,
	channels: int,
	default_duration_ms: int = 180,
) -> np.ndarray:
	parts: list[np.ndarray] = []
	for raw_token in notes.split():
		if ":" in raw_token:
			note, duration_text = raw_token.split(":", 1)
			try:
				duration_ms = max(20, min(2000, int(duration_text)))
			except ValueError:
				duration_ms = default_duration_ms
		else:
			note = raw_token
			duration_ms = default_duration_ms

		frequency = note_frequency(note)
		if frequency is None:
			parts.append(silence(duration_ms, sample_rate, channels))
		else:
			parts.append(make_tone(frequency, duration_ms, sample_rate, channels))

	if not parts:
		return make_tone(880.0, 120, sample_rate, channels)
	return np.concatenate(parts, axis=0)


def named_sound(name: str, sample_rate: int, channels: int) -> np.ndarray:
	key = name.strip().lower()
	melodies: dict[str, Iterable[str]] = {
		"beep": ("A5:120",),
		"success": ("C5:100", "E5:100", "G5:160"),
		"error": ("C4:160", "R:60", "C4:220"),
		"alert": ("A5:140", "R:80", "A5:140", "R:80", "A5:220"),
		"fanfare": ("C5:100", "E5:100", "G5:100", "C6:260"),
	}
	return melody_to_samples(" ".join(melodies.get(key, melodies["beep"])), sample_rate, channels)
