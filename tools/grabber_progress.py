"""CLI-only heartbeat. The reporting thread never touches files or the database."""
from contextvars import ContextVar
import sys
import threading
import time


_active = ContextVar("grabber_progress", default=None)


class Progress:
    def __init__(self, command, interval=10):
        self.command = command
        self.interval = interval
        self.stop = threading.Event()
        self.lock = threading.Lock()
        self.started = time.monotonic()
        self.label = "initializing"
        self.low = self.high = 0
        self.source_range = (0, 0)
        self.stage_started = self.started
        self.current = self.total = None
        self.unit = ""
        self.done = False

    def __enter__(self):
        self.token = _active.set(self)
        self.thread = threading.Thread(target=self._loop, daemon=True)
        self.thread.start()
        return self

    def __exit__(self, exc_type, exc, traceback):
        self.stop.set()
        self.thread.join()
        _active.reset(self.token)
        if exc_type is None:
            self.done = True
            self.emit()

    def stage(self, label, low, high):
        with self.lock:
            self.label, self.low, self.high = label, low, high
            self.current = self.total = None
            self.unit = ""
            self.stage_started = time.monotonic()

    def update(self, current, total, unit):
        with self.lock:
            self.current, self.total, self.unit = current, total, unit

    def emit(self):
        with self.lock:
            now = time.monotonic()
            fraction = min(1, self.current / self.total) if self.total else None
            percent = self.low + (self.high - self.low) * (fraction or 0)
            stage_eta = "unknown"
            detail = "unknown"
            if fraction is not None:
                detail = f"{fraction:.1%} ({self.current:,}/{self.total:,} {self.unit})"
                if fraction == 1:
                    stage_eta = "0s"
                elif 0 < fraction < 1:
                    stage_eta = f"~{(now - self.stage_started) * (1 - fraction) / fraction:.0f}s"
            elif self.current is not None:
                detail = f"{self.current:,} {self.unit}; total unknown"
            if self.done:
                message = f"[{self.command} 100%] finished | ETA 0s"
            else:
                message = (f"[{self.command} ~{percent:.1f}%] {self.label} | "
                           f"stage {detail} | stage ETA {stage_eta} | overall ETA unknown")
            try:
                print(f"{message} | elapsed {now - self.started:.0f}s", file=sys.stderr, flush=True)
            except OSError:
                # A closed stderr pipe must not affect catalog processing.
                pass

    def _loop(self):
        while not self.stop.wait(self.interval):
            self.emit()


def active():
    return _active.get()


def stage(label, low, high):
    if progress := active():
        progress.stage(label, low, high)


def portion(label, index, count, low, high):
    width = (high - low) / max(count, 1)
    stage(label, low + index * width, low + (index + 1) * width)


def substage(label, start, end):
    """Divide the current source's allotted range, without nesting state."""
    if progress := active():
        low, high = progress.source_range
        progress.stage(label, low + (high - low) * start, low + (high - low) * end)


def source(label, index, count, low, high):
    portion(label, index, count, low, high)
    if progress := active():
        progress.source_range = progress.low, progress.high


def track(items, total=None, unit="rows"):
    progress = active()
    if progress is None:
        yield from items
        return
    progress.update(0, total, unit)
    index = 0
    for index, item in enumerate(items, 1):
        yield item
        if index == 1 or index % 256 == 0:
            progress.update(index, total, unit)
    progress.update(index, total, unit)
