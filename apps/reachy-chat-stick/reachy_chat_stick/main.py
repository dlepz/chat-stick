from __future__ import annotations

import asyncio
import logging
import os
import sys
import threading
from typing import Any

from .config import AppConfig

try:
	from reachy_mini import ReachyMini, ReachyMiniApp
except ModuleNotFoundError:  # Allows syntax checks on machines without the SDK.
	ReachyMini = Any  # type: ignore

	class ReachyMiniApp:  # type: ignore
		def wrapped_run(self, *args: Any, **kwargs: Any) -> None:
			raise RuntimeError("reachy-mini is not installed in this Python environment")

		def stop(self) -> None:
			pass


class ChatStickReachyApp(ReachyMiniApp):
	custom_app_url: str | None = None

	def run(self, reachy_mini: ReachyMini, stop_event: threading.Event) -> None:
		_setup_logging()
		config = AppConfig.from_env()
		logger = logging.getLogger(__name__)
		logger.info("Reachy chat-stick app starting")
		logger.info("server=%s device_id=%s voice=%s", config.server_url, config.device_id, config.voice)
		try:
			from .runner import ReachyChatStickRunner
		except ModuleNotFoundError as exc:
			missing = exc.name or "a required package"
			raise RuntimeError(
				f"Missing Python dependency: {missing}. "
				"Install the app first with `python -m pip install -e .` "
				"from apps/reachy-chat-stick, using the same Python you use to run it."
			) from exc

		runner = ReachyChatStickRunner(reachy_mini, stop_event, config)
		asyncio.run(runner.run())


def _setup_logging() -> None:
	level_name = os.getenv("CHAT_STICK_LOG_LEVEL", "INFO").upper()
	level = getattr(logging, level_name, logging.INFO)
	logging.basicConfig(
		level=level,
		format="%(asctime)s %(levelname)s %(name)s: %(message)s",
		stream=sys.stdout,
	)


def main() -> None:
	_setup_logging()
	config = AppConfig.from_env()

	app = ChatStickReachyApp()
	app.media_backend = config.media_backend
	if config.connection_mode == "network":
		app.daemon_on_localhost = False
	elif config.connection_mode == "localhost_only":
		app.daemon_on_localhost = True
	try:
		app.wrapped_run(host=config.reachy_host, port=config.reachy_port)
	except KeyboardInterrupt:
		app.stop()


if __name__ == "__main__":
	main()
