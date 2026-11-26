"""Thread-safe stderr progress; stage weights are estimates, not byte-exact global work."""
from __future__ import annotations
import sys
import threading
import time


class Progress:
    def __init__(self, enabled=True, interval=2.0):
        self.enabled = enabled
        self.interval = interval
        self.lock = threading.Lock()
        self.stop = threading.Event()
        self.started = time.monotonic()
        self.label = "initializing"
        self.low, self.high, self.fraction = 0.0, 0.0, None
        self.stage_started = self.started
        self.thread = None
        self.percent = 0.0

    def __enter__(self):
        if self.enabled:
            self.thread = threading.Thread(target=self._loop, daemon=True)
            self.thread.start()
        return self

    def stage(self, label, low, high):
        with self.lock:
            self.label, self.low, self.high = label, low, high
            self.fraction = None
            self.stage_started = time.monotonic()
        self.emit()

    def update(self, current, total):
        with self.lock:
            self.fraction = min(1.0, current / total) if total and total > 0 else None

    def emit(self):
        if not self.enabled:
            return
        with self.lock:
            now = time.monotonic()
            f = self.fraction
            percent = max(self.percent, self.low + (self.high - self.low) * (f or 0))
            self.percent = percent
            overall_eta = ((now-self.started) * (100-percent) / percent
                           if percent > 0 and now-self.started >= 1 else None)
            eta = ((now - self.stage_started) * (1-f) / f
                   if f and f < 1 and now - self.stage_started >= 1 else None)
            print(f"[inventory ~{percent:5.1f}%] {self.label} | "
                  f"stage {f*100:.1f}%" if f is not None else
                  f"[inventory ~{percent:5.1f}%] {self.label} | stage indeterminate",
                  end=f" | elapsed {now-self.started:.0f}s | stage ETA " +
                      (f"~{eta:.0f}s" if eta is not None else "unknown") +
                      " | overall ETA " + (f"~{overall_eta:.0f}s (weighted)" if overall_eta is not None else "unknown") + "\n",
                  file=sys.stderr, flush=True)

    def _loop(self):
        while not self.stop.wait(self.interval):
            self.emit()

    def __exit__(self, *exc):
        self.stop.set()
        if self.thread:
            self.thread.join()
