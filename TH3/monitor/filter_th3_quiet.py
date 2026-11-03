"""Keep TH3 command/state feedback readable while repetitive timing rows arrive."""
import re

from platformio.device.monitor.filters.base import DeviceMonitorFilterBase


class Th3Quiet(DeviceMonitorFilterBase):
    NAME = "th3_quiet"
    _TIMING_ROW = re.compile(r"^\d+,(?:timer|millis),")

    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._pending = ""

    def rx(self, text):
        self._pending += text
        visible = []
        while "\n" in self._pending:
            line, self._pending = self._pending.split("\n", 1)
            if not self._TIMING_ROW.match(line):
                visible.append(line + "\n")
        # Preserve incomplete nonstandard diagnostic output without growing forever.
        if len(self._pending) > 16384:
            visible.append(self._pending)
            self._pending = ""
        return "".join(visible)
