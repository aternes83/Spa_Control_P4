#!/usr/bin/env python3
"""NTC sensor: calibration curve, fault handling, and the smoothing filter.

No hardware — NTCSensor takes an injectable adc, so a fake one that returns
whatever microvolts we want drives the whole path.
"""
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "s3_control"))

import sensors  # noqa: E402

fail = 0


def check(name, ok, got=None):
    global fail
    if ok:
        print("  ok   %s" % name)
    else:
        fail += 1
        print("  FAIL %s%s" % (name, "" if got is None else "  (got %r)" % (got,)))


class FakeADC:
    """Returns a fixed voltage, plus an optional per-call wobble."""

    def __init__(self, volts, noise=0.0):
        self.volts = volts
        self.noise = noise
        self._n = 0

    def read_uv(self):
        self._n += 1
        # Deterministic square wobble: half the samples high, half low, so the
        # oversampler inside _read_resistance() cannot average it away.
        sign = 1 if (self._n // sensors.NTC_OVERSAMPLE) % 2 == 0 else -1
        return int((self.volts + sign * self.noise) * 1_000_000)


def volts_for(ohms, cal):
    """Inverse of the divider, so a test can ask for a resistance directly."""
    return cal["vsupply"] * ohms / (cal["r_fixed"] + ohms)


CAL = dict(sensors.NTC_BALBOA_M7_CAL)
CAL.setdefault("vsupply", sensors.NTC_VSUPPLY_V)


def probe_at(ohms, noise_ohms=0.0):
    v = volts_for(ohms, CAL)
    v_hi = volts_for(ohms + noise_ohms, CAL)
    return sensors.NTCSensor(6, CAL, adc=FakeADC(v, abs(v_hi - v)))


def pump(p, seconds):
    """Drive poll() at the configured interval for a number of seconds."""
    for i in range(seconds):
        p.poll(i * sensors.NTC_READ_INTERVAL_MS)


print()
print("the calibration curve")
p = probe_at(CAL["r0"])
p.poll(0)
check("r0 reads back as t0 (25 C = 77 F)", abs(p.read_f() - 77.0) < 0.05, p.read_f())

# 34.4k on the bench read 71.4 F against a room thermometer at about 70.
p = probe_at(34411.0)
p.poll(0)
check("the bench reading reproduces: 34.4k -> about 71 F",
      70.5 < p.read_f() < 72.5, p.read_f())

check("r_fixed is the 10k divider, not the 30k probe",
      sensors.NTC_BALBOA_M7_CAL["r_fixed"] == 10000.0,
      sensors.NTC_BALBOA_M7_CAL["r_fixed"])

print()
print("faults")
p = probe_at(CAL["r0"])
check("starts faulted before any read", p.read_f() is None)

p = probe_at(1_000_000.0)          # open probe
pump(p, 10)
check("an open probe never produces a reading", p.read_f() is None, p.read_f())

p = probe_at(50.0)                 # shorted probe
pump(p, 10)
check("a shorted probe never produces a reading", p.read_f() is None, p.read_f())

print()
print("the smoothing filter")
check("alpha matches tau and the sample interval",
      abs(sensors.NTC_FILTER_ALPHA - 1000.0 / 21000.0) < 1e-9,
      sensors.NTC_FILTER_ALPHA)

# A noisy probe, wobbling by about +/-0.6 F worth of resistance.
noisy = probe_at(34411.0, noise_ohms=260.0)
noisy.poll(0)
seeded = noisy.read_f()
check("the first good read seeds the filter rather than ramping from zero",
      abs(seeded - noisy.read_raw_f()) < 1e-9, (seeded, noisy.read_raw_f()))

raws, filts = [], []
for i in range(1, 400):
    noisy.poll(i * sensors.NTC_READ_INTERVAL_MS)
    raws.append(noisy.read_raw_f())
    filts.append(noisy.read_f())
raw_spread = max(raws) - min(raws)
filt_spread = max(filts[100:]) - min(filts[100:])   # after settling
check("the raw signal really is noisier than the hysteresis band",
      raw_spread > 0.5, raw_spread)
check("the filter brings it well inside the 0.5 F band",
      filt_spread < 0.25, filt_spread)
check("and cuts the noise by at least 3x", raw_spread / max(filt_spread, 1e-9) > 3.0,
      raw_spread / max(filt_spread, 1e-9))

# A step change must still arrive, just slowly.
step = probe_at(34411.0)
step.poll(0)
start = step.read_f()
step.adc.volts = volts_for(30000.0, CAL)            # jump toward 77 F
for i in range(1, 121):
    step.poll(i * sensors.NTC_READ_INTERVAL_MS)
check("a real step still gets there (within 2 min)",
      abs(step.read_f() - step.read_raw_f()) < 0.2,
      (step.read_f(), step.read_raw_f()))
check("and it moved in the right direction", step.read_f() > start + 3.0,
      (start, step.read_f()))

# Coming back from a fault must re-seed, not slew across.
rec = probe_at(34411.0)
rec.poll(0)
rec.adc.volts = volts_for(1_000_000.0, CAL)         # unplug
for i in range(1, 11):
    rec.poll(i * sensors.NTC_READ_INTERVAL_MS)
check("a disconnected probe faults", rec._fail >= sensors.NTC_FAIL_LIMIT, rec._fail)
rec.adc.volts = volts_for(60000.0, CAL)             # plug a much colder one back in
rec.poll(11 * sensors.NTC_READ_INTERVAL_MS)
check("the first read after a fault re-seeds instead of slewing",
      abs(rec.read_f() - rec.read_raw_f()) < 1e-9,
      (rec.read_f(), rec.read_raw_f()))

print()
if fail:
    print("%d sensor test(s) FAILED" % fail)
    sys.exit(1)
print("all sensor tests passed")
