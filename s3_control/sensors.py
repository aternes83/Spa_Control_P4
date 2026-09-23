"""
Water-temperature sensing for the control node — NTC thermistor on ADC1.

Ported from openplc-hot-tub v2.0. The legacy DS18B20 1-Wire path is deliberately
not carried over: its bit-bang timing disables interrupts for milliseconds per
read, which is exactly the kind of jitter a hard-real-time control loop and a
framed serial link do not need. Wire an NTC.

    3.3V ── R_fixed ──┬── ADC pin ── NTC ── GND

poll() oversamples the ADC, back-solves the probe resistance and converts it to
°F with a Beta-model calibration. read_f() returns the last good reading;
faulted() is True until the first good read and whenever the probe reads
open/short/implausible — the control core turns that into FAULT_TEMP_SENSOR and
refuses to heat.
"""

try:
    import utime as time_mod
except ImportError:
    import time as time_mod


def _ticks_diff(a, b):
    if hasattr(time_mod, "ticks_diff"):
        return time_mod.ticks_diff(a, b)
    return a - b


def _ticks_add(t, d):
    if hasattr(time_mod, "ticks_add"):
        return time_mod.ticks_add(t, d)
    return t + d


NTC_READ_INTERVAL_MS = 1000     # ADC reads are cheap and instant; 1 s is plenty
NTC_OVERSAMPLE       = 16       # averaged samples to knock down noise/EMI on long leads
NTC_VSUPPLY_V        = 3.3      # divider top rail (absorbed by calibration if slightly off)
NTC_R_OPEN_OHMS      = 500000.0 # above → probe open / disconnected → fault
NTC_R_SHORT_OHMS     = 200.0    # below → probe shorted → fault
NTC_FAIL_LIMIT       = 5        # consecutive bad reads before declaring a fault

# Smoothing, as a one-pole filter over the 1 s samples.
#
# The raw reading carries roughly 0.6 °F peak-to-peak of noise on long probe
# leads, and temp_hysteresis_f is 0.5 °F — the noise is wider than the whole
# thermostat band, so without this the heater contactor chatters on noise alone,
# once per sample, whenever the water sits near setpoint.
#
# Filtered rather than sampled slowly, and the difference matters. Reading every
# 20 s instead would cut how *often* the relay can flip without touching the
# noise that flips it, and it would stretch NTC_FAIL_LIMIT from 5 s to 100 s
# before a disconnected probe is called a fault. Filtering at 1 s keeps fault
# detection quick and cuts the noise by about sqrt(tau/interval) — near 4.5x
# here, which puts it well inside the hysteresis band. Water is thermally slow;
# 20 s of lag on a body of water that takes hours to heat is nothing.
NTC_FILTER_TAU_MS    = 20000    # time constant; 0 disables the filter
NTC_FILTER_ALPHA     = (float(NTC_READ_INTERVAL_MS) /
                        (NTC_FILTER_TAU_MS + NTC_READ_INTERVAL_MS)
                        if NTC_FILTER_TAU_MS > 0 else 1.0)
TEMP_PLAUSIBLE_MIN_F = 20.0     # below this = open/short/ice — implausible for spa water
TEMP_PLAUSIBLE_MAX_F = 200.0    # above this = fault; real over-temp is the high limit's job

# Generic 10 k NTC, Beta 3950 @ 25 °C. Overwritten by a calibrated profile pushed
# from the HMI and persisted to config.json.
NTC_DEFAULT_CAL = {"r_fixed": 10000.0, "r0": 10000.0, "t0_c": 25.0,
                   "beta": 3950.0, "offset_f": 0.0}

# Balboa M7 (30 k NTC) — the preset for the sensor already in this tub.
#
# r_fixed is the divider resistor fitted on the board, NOT the probe: it stays
# 10 k whichever thermistor is on the end of the leads. Only r0 and beta
# describe the probe. This carried 30 k for r_fixed when it was ported, which
# reads a 70 F room as 30 F — plausible enough to pass a plausibility check and
# be believed. openplc-hot-tub had it right: r_fixed 10000, r0 30000.
NTC_BALBOA_M7_CAL = {"r_fixed": 10000.0, "r0": 30000.0, "t0_c": 25.0,
                     "beta": 3892.0, "offset_f": 0.0}


class NTCSensor:
    def __init__(self, pin, cal=None, adc=None):
        self._last_f = None              # filtered, and what read_f() returns
        self._raw_f = None               # unfiltered, for diagnostics only
        self._fail = NTC_FAIL_LIMIT      # start faulted until the first good read
        self._next_ms = 0
        self._r = None                   # last computed resistance (Ω)
        self.adc = adc                   # injectable, so host tests need no hardware
        c = dict(NTC_DEFAULT_CAL)
        if cal:
            c.update(cal)
        self.set_cal(c)
        if self.adc is None:
            try:
                from machine import ADC, Pin
                self.adc = ADC(Pin(int(pin)), atten=ADC.ATTN_11DB)  # ~0–3.1 V usable
            except Exception as e:
                print("temp: NTC init failed: %s" % e)

    def set_cal(self, cal):
        """Apply calibration coefficients. Partial dicts merge onto current values,
        so an offset-only update works."""
        self.r_fixed = float(cal.get("r_fixed", getattr(self, "r_fixed", 10000.0)))
        self.r0 = float(cal.get("r0", getattr(self, "r0", 10000.0)))
        t0_c = float(cal.get("t0_c", getattr(self, "_t0_c", 25.0)))
        self._t0_c = t0_c
        self.t0_k = t0_c + 273.15
        self.beta = float(cal.get("beta", getattr(self, "beta", 3950.0)))
        self.offset_f = float(cal.get("offset_f", getattr(self, "offset_f", 0.0)))
        self.vsupply = float(cal.get("vsupply", getattr(self, "vsupply", NTC_VSUPPLY_V)))

    def get_cal(self):
        return {"r_fixed": self.r_fixed, "r0": self.r0, "t0_c": self._t0_c,
                "beta": self.beta, "offset_f": self.offset_f, "vsupply": self.vsupply}

    def _read_resistance(self):
        acc = 0
        for _ in range(NTC_OVERSAMPLE):
            acc += self.adc.read_uv()               # calibrated µV (uses the eFuse curve)
        v = (acc / NTC_OVERSAMPLE) / 1_000_000.0    # → volts
        if v <= 0.001:                              # rail-low → shorted probe
            return NTC_R_SHORT_OHMS * 0.5
        if v >= self.vsupply - 0.001:               # rail-high → open probe
            return NTC_R_OPEN_OHMS * 2.0
        return self.r_fixed * v / (self.vsupply - v)

    def _r_to_f(self, r):
        import math
        inv_t = 1.0 / self.t0_k + (1.0 / self.beta) * math.log(r / self.r0)
        t_c = 1.0 / inv_t - 273.15
        return t_c * 9.0 / 5.0 + 32.0 + self.offset_f

    def _note_fail(self):
        if self._fail <= NTC_FAIL_LIMIT:
            self._fail += 1

    def poll(self, now_ms):
        if self.adc is None:
            return
        if _ticks_diff(now_ms, self._next_ms) < 0:
            return
        self._next_ms = _ticks_add(now_ms, NTC_READ_INTERVAL_MS)
        try:
            r = self._read_resistance()
        except Exception:
            r = None
        self._r = r
        if r is None or r <= NTC_R_SHORT_OHMS or r >= NTC_R_OPEN_OHMS:
            self._note_fail()
            return
        try:
            f = self._r_to_f(r)
        except Exception:
            self._note_fail()
            return
        if TEMP_PLAUSIBLE_MIN_F <= f <= TEMP_PLAUSIBLE_MAX_F:
            self._raw_f = f
            # Re-seed instead of slewing on the first good read after a fault,
            # so a probe that was unplugged for a while does not drag the
            # reading across from wherever it was left.
            if self._last_f is None or self._fail >= NTC_FAIL_LIMIT:
                self._last_f = f
            else:
                self._last_f += NTC_FILTER_ALPHA * (f - self._last_f)
            self._fail = 0
        else:
            self._note_fail()

    def read_f(self):
        """Filtered water temperature. This is what the control loop uses."""
        return self._last_f

    def read_raw_f(self):
        """The latest unfiltered sample. Diagnostics only — comparing it against
        read_f() shows how much noise the filter is actually removing."""
        return self._raw_f

    def read_ohms(self):
        return self._r

    def faulted(self):
        return self._last_f is None or self._fail >= NTC_FAIL_LIMIT
