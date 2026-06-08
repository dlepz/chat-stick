from __future__ import annotations

import json
import time
from dataclasses import asdict, dataclass
from pathlib import Path


MAX_TIMERS = 8
MAX_DURATION_SECONDS = 24 * 60 * 60
MAX_NAME_LEN = 32


@dataclass
class TimerRecord:
	id: int
	name: str
	deadline_epoch: float
	duration_seconds: int
	created_at_epoch: float

	def remaining_seconds(self, now: float | None = None) -> int:
		now = time.time() if now is None else now
		return max(0, int(round(self.deadline_epoch - now)))


class TimerManager:
	def __init__(self, path: Path):
		self.path = path
		self.timers: list[TimerRecord] = []
		self.next_id = 1
		self.load()

	def load(self) -> None:
		try:
			raw = json.loads(self.path.read_text())
		except (FileNotFoundError, json.JSONDecodeError, OSError):
			return

		now = time.time()
		self.next_id = int(raw.get("next_id", 1))
		self.timers = []
		for row in raw.get("timers", []):
			try:
				timer = TimerRecord(
					id=int(row["id"]),
					name=str(row.get("name", "")),
					deadline_epoch=float(row["deadline_epoch"]),
					duration_seconds=int(row["duration_seconds"]),
					created_at_epoch=float(row["created_at_epoch"]),
				)
			except (KeyError, TypeError, ValueError):
				continue
			if timer.deadline_epoch > now:
				self.timers.append(timer)
		if self.timers:
			self.next_id = max(self.next_id, max(t.id for t in self.timers) + 1)

	def save(self) -> None:
		self.path.parent.mkdir(parents=True, exist_ok=True)
		payload = {
			"next_id": self.next_id,
			"timers": [asdict(timer) for timer in self.timers],
		}
		self.path.write_text(json.dumps(payload, indent=2))

	def add(self, duration_seconds: int, name: str = "") -> str:
		if duration_seconds < 1 or duration_seconds > MAX_DURATION_SECONDS:
			return "duration out of range"
		if len(self.timers) >= MAX_TIMERS:
			return "timer limit reached"

		clean_name = name.strip()[:MAX_NAME_LEN]
		if clean_name and self._find_by_name(clean_name) is not None:
			return "a timer with that name is already running"

		now = time.time()
		timer = TimerRecord(
			id=self.next_id,
			name=clean_name,
			deadline_epoch=now + duration_seconds,
			duration_seconds=duration_seconds,
			created_at_epoch=now,
		)
		self.next_id += 1
		self.timers.append(timer)
		self.save()
		return self.describe_all()

	def cancel(self, timer_id: int | None = None, name: str = "", all_timers: bool = False) -> str:
		if all_timers:
			count = len(self.timers)
			self.timers = []
			self.save()
			return f"cancelled {count} timer(s); {self.describe_all()}"

		index = self._resolve(timer_id, name)
		if isinstance(index, str):
			return index
		cancelled = self.timers.pop(index)
		self.save()
		label = f" {cancelled.name}" if cancelled.name else ""
		return f"cancelled timer{label}; {self.describe_all()}"

	def extend(self, delta_seconds: int, timer_id: int | None = None, name: str = "") -> str:
		index = self._resolve(timer_id, name)
		if isinstance(index, str):
			return index

		timer = self.timers[index]
		timer.deadline_epoch = max(time.time() + 1, timer.deadline_epoch + delta_seconds)
		new_duration = timer.duration_seconds + delta_seconds
		if 1 <= new_duration <= MAX_DURATION_SECONDS:
			timer.duration_seconds = new_duration
		self.save()
		return self.describe_all()

	def harvest_expired(self) -> list[TimerRecord]:
		now = time.time()
		expired = [timer for timer in self.timers if timer.deadline_epoch <= now]
		if expired:
			self.timers = [timer for timer in self.timers if timer.deadline_epoch > now]
			self.save()
		return expired

	def describe_all(self) -> str:
		now = time.time()
		return json.dumps(
			{
				"count": len(self.timers),
				"timers": [
					{
						"id": timer.id,
						"name": timer.name,
						"duration_seconds": timer.duration_seconds,
						"remaining_seconds": timer.remaining_seconds(now),
						"created_at_epoch": int(timer.created_at_epoch),
					}
					for timer in self.timers
				],
			},
			separators=(",", ":"),
		)

	def _resolve(self, timer_id: int | None, name: str) -> int | str:
		if not self.timers:
			return "no timers running"
		if timer_id:
			for index, timer in enumerate(self.timers):
				if timer.id == timer_id:
					return index
			return "no timer with that id"
		if name.strip():
			index = self._find_by_name(name)
			if index is None:
				return "no timer with that name"
			return index
		if len(self.timers) > 1:
			return "multiple timers active; call list_timers and pass the target id"
		return 0

	def _find_by_name(self, name: str) -> int | None:
		needle = name.strip().lower()
		for index, timer in enumerate(self.timers):
			if timer.name.strip().lower() == needle:
				return index
		return None
