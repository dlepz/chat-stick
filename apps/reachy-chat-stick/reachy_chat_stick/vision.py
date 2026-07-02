from __future__ import annotations

import logging
import time
from dataclasses import dataclass
from typing import Any

import numpy as np

logger = logging.getLogger(__name__)


@dataclass(frozen=True)
class FaceTarget:
	center_x: float
	center_y: float
	width: float
	height: float
	frame_width: int
	frame_height: int
	seen_at: float
	head_pose: np.ndarray | None = None

	@property
	def x_offset(self) -> float:
		return _normalized_offset(self.center_x, self.frame_width)

	@property
	def y_offset(self) -> float:
		return _normalized_offset(self.center_y, self.frame_height)

	def with_head_pose(self, head_pose: np.ndarray | None) -> "FaceTarget":
		return FaceTarget(
			center_x=self.center_x,
			center_y=self.center_y,
			width=self.width,
			height=self.height,
			frame_width=self.frame_width,
			frame_height=self.frame_height,
			seen_at=self.seen_at,
			head_pose=head_pose,
		)


class FaceDetector:
	def __init__(self, cv2: Any, cascade: Any, scale_factor: float, min_neighbors: int):
		self.cv2 = cv2
		self.cascade = cascade
		self.scale_factor = scale_factor
		self.min_neighbors = min_neighbors

	@classmethod
	def create(
		cls,
		scale_factor: float = 1.1,
		min_neighbors: int = 4,
	) -> "FaceDetector | None":
		try:
			import cv2  # type: ignore[import-not-found]
		except ModuleNotFoundError:
			logger.warning(
				"face tracking disabled: install opencv-python-headless or run with "
				"CHAT_STICK_REACHY_FACE_TRACKING=false"
			)
			return None

		cascade_path = (
			getattr(cv2.data, "haarcascades", "")
			+ "haarcascade_frontalface_default.xml"
		)
		cascade = cv2.CascadeClassifier(cascade_path)
		if cascade.empty():
			logger.warning("face tracking disabled: could not load OpenCV face cascade")
			return None
		return cls(cv2, cascade, scale_factor, min_neighbors)

	def detect(self, frame: np.ndarray) -> FaceTarget | None:
		if frame is None or frame.size == 0 or frame.ndim < 2:
			return None

		frame_height, frame_width = frame.shape[:2]
		if frame_width <= 0 or frame_height <= 0:
			return None

		try:
			gray = self.cv2.cvtColor(frame, self.cv2.COLOR_BGR2GRAY)
			gray = self.cv2.equalizeHist(gray)
			min_size = max(32, min(frame_width, frame_height) // 14)
			faces = self.cascade.detectMultiScale(
				gray,
				scaleFactor=self.scale_factor,
				minNeighbors=self.min_neighbors,
				minSize=(min_size, min_size),
			)
		except Exception:
			logger.debug("face detection failed", exc_info=True)
			return None

		if len(faces) == 0:
			return None

		x, y, w, h = max(faces, key=lambda rect: rect[2] * rect[3])
		return FaceTarget(
			center_x=float(x + w / 2),
			center_y=float(y + h / 2),
			width=float(w),
			height=float(h),
			frame_width=int(frame_width),
			frame_height=int(frame_height),
			seen_at=time.monotonic(),
		)


def _normalized_offset(value: float, size: int) -> float:
	if size <= 0:
		return 0.0
	return max(-1.0, min(1.0, (value - (size / 2.0)) / (size / 2.0)))
