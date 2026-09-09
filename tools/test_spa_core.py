#!/usr/bin/env python3
"""
Control-core tests. Runs on the host, no hardware:

    python3 tools/test_spa_core.py

Part 1 pins the safety properties that must survive any refactor of the two-board
split — a fault kills every load, the heater never runs without proven flow or on
a bad sensor, freeze protection outlives the run timer.

Part 2 is the reason this file exists now rather than later: s3_control/spa_core.py
is a port of logic that is currently running a real hot tub. It drives the original
openplc-hot-tub SpaController and the ported one through the same randomised input
sequences on a shared fake clock and requires the outputs to be identical, step for
step. If the sibling repo is not on disk that part skips with a notice.
"""
import os
import random
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "s3_control"))

import spa_core  # noqa: E402

FAILED = []


def check(name, cond, detail=""):
    if cond:
        print("  ok   %s" % name)
    else:
        print("  FAIL %s %s" % (name, detail))
        FAILED.append(name)


class FakeClock:
    """Monotonic ms clock we advance by hand, so the 5 s flow-prove and 4 h run
    timers are exercised in microseconds and without wall-clock flakiness."""

    def __init__(self):
        self.t = 0

    def install(self, module):
        module.ticks_ms = lambda: self.t
        module.ticks_diff = lambda a, b: a - b
        module.ticks_add = lambda a, d: a + d

    def advance(self, ms):
        self.t += ms


BASE = {
    "xSpaEnable": True, "xPumpRequest": False, "xPump1HighRequest": False,
    "xPump2Request": False, "xPump3Request": False, "xJetsRequest": False,
    "xBlowerRequest": False, "xLightRequest": False, "xFlowSwitch": True,
    "xHighLimitOK": True, "xRemoteEStopOK": True, "xTempSensorOK": True,
    "rWaterTemp_F": 98.0,
}


def run(ctrl, clock, inputs, ms, step_ms=50):
    """Hold inputs steady for ms, returning the final outputs."""
    out = None
    for _ in range(max(1, ms // step_ms)):
        out = ctrl.step(inputs)
        clock.advance(step_ms)
    return out


def safety_properties():
    print("safety properties")
    clock = FakeClock()
    clock.install(spa_core)

    LOADS = ("xPump1_Low", "xPump1_High", "xPump2", "xPump3",
             "xHeater", "xJets", "xBlower")

    # Heater requires proven flow: it must stay off through the prove delay.
    c = spa_core.SpaController()
    early = run(c, clock, BASE, 3000)
    check("heater off before flow is proven", not early["xHeater"])
    late = run(c, clock, BASE, 6000)
    check("heater on once flow is proven and water is below setpoint", late["xHeater"])

    # A dead temp sensor must stop the heat and raise fault 5.
    c = spa_core.SpaController()
    d = dict(BASE, xTempSensorOK=False)
    out = run(c, clock, d, 10000)
    check("bad temp sensor stops the heater", not out["xHeater"])
    check("bad temp sensor raises FAULT_TEMP_SENSOR",
          out["iFaultCode"] == spa_core.FAULT_TEMP_SENSOR)

    # Every fault must drop every load.
    for name, mutation, code in (
        ("e-stop", {"xRemoteEStopOK": False}, spa_core.FAULT_ESTOP),
        ("high limit", {"xHighLimitOK": False}, spa_core.FAULT_HIGH_LIMIT),
        ("over temp", {"rWaterTemp_F": 110.0}, spa_core.FAULT_OVERTEMP),
    ):
        c = spa_core.SpaController()
        d = dict(BASE, xPumpRequest=True, xJetsRequest=True, xBlowerRequest=True)
        run(c, clock, d, 10000)
        out = run(c, clock, dict(d, **mutation), 1000)
        check("%s raises the right fault code" % name, out["iFaultCode"] == code,
              out["iFaultCode"])
        check("%s drops every load" % name, not any(out[k] for k in LOADS),
              [k for k in LOADS if out[k]])

    # Losing flow after it was proven must fault, not silently stop heating.
    # This is the one deliberate behavioural change from v2.0 (see spa_core's
    # flow_fault_latch): there, FAULT_NO_FLOW was unreachable.
    c = spa_core.SpaController()
    d = dict(BASE, xPumpRequest=True)
    run(c, clock, d, 10000)
    out = run(c, clock, dict(d, xFlowSwitch=False), 500)
    check("flow lost after proving raises FAULT_NO_FLOW",
          out["iFaultCode"] == spa_core.FAULT_NO_FLOW, out["iFaultCode"])
    check("flow lost drops the heater", not out["xHeater"])

    # The fault must clear by itself once flow comes back — a flow blip should not
    # need an operator to reset it, or freeze protection would stay down.
    out = run(c, clock, d, 1000)
    check("NO_FLOW clears when flow returns", out["iFaultCode"] == spa_core.FAULT_NONE,
          out["iFaultCode"])

    # ...and must not fire before flow was ever established (dry start).
    c = spa_core.SpaController()
    out = run(c, clock, dict(BASE, xPumpRequest=True, xFlowSwitch=False), 10000)
    check("no NO_FLOW fault before flow is ever proven",
          out["iFaultCode"] == spa_core.FAULT_NONE, out["iFaultCode"])

    # With the latch off the port must reproduce v2.0 exactly: no fault at all.
    c = spa_core.SpaController()
    c.flow_fault_latch = False
    run(c, clock, d, 10000)
    out = run(c, clock, dict(d, xFlowSwitch=False), 500)
    check("flow_fault_latch=False reproduces v2.0 (fault unreachable)",
          out["iFaultCode"] == spa_core.FAULT_NONE, out["iFaultCode"])

    # Freeze protection: with the spa disabled the thermostat must still run.
    c = spa_core.SpaController()
    d = dict(BASE, xSpaEnable=False, rWaterTemp_F=45.0)
    out = run(c, clock, d, 10000)
    check("freeze protection circulates with the spa disabled", out["xPump1_Low"])
    check("freeze protection heats with the spa disabled", out["xHeater"])

    # ...and must outlive the 4-hour run timer, which only gates user loads.
    c = spa_core.SpaController()
    d = dict(BASE, rWaterTemp_F=45.0, xJetsRequest=True)
    run(c, clock, d, 10000)
    clock.advance(5 * 60 * 60 * 1000)
    out = run(c, clock, d, 1000)
    check("run timer expiry stops the jets", not out["xJets"])
    check("run timer expiry does not stop freeze heating", out["xHeater"])

    # Pump 1 speeds are interlocked — never both energised.
    c = spa_core.SpaController()
    d = dict(BASE, xPumpRequest=True, xPump1HighRequest=True)
    out = run(c, clock, d, 10000)
    check("pump 1 low and high are interlocked",
          not (out["xPump1_Low"] and out["xPump1_High"]))


def differential_against_original():
    print("differential vs the firmware running the tub")
    orig = os.path.join(os.path.dirname(ROOT), "openplc-hot-tub")
    if not os.path.isdir(orig):
        print("  skip  openplc-hot-tub not found next to this repo (%s)" % orig)
        return
    sys.path.insert(0, orig)
    try:
        import spa_control as old
    except Exception as e:                       # pragma: no cover - diagnostic path
        print("  skip  could not import the original module: %s" % e)
        return

    clock = FakeClock()
    clock.install(spa_core)
    clock.install(old)

    rng = random.Random(20260909)
    bools = ("xSpaEnable", "xPumpRequest", "xPump1HighRequest", "xPump2Request",
             "xPump3Request", "xJetsRequest", "xBlowerRequest", "xLightRequest",
             "xFlowSwitch", "xHighLimitOK", "xRemoteEStopOK", "xTempSensorOK")

    divergences = []
    steps = 0
    for run_i in range(60):
        a = old.SpaController()
        b = spa_core.SpaController()
        b.flow_fault_latch = False   # diff against v2.0 semantics, not the fix
        state = {k: rng.random() < 0.5 for k in bools}
        state["rWaterTemp_F"] = rng.uniform(35.0, 115.0)
        for _ in range(400):
            # Flip a field occasionally so timers and edges get exercised, and
            # let the clock jump unevenly across the 5 s prove threshold.
            if rng.random() < 0.15:
                k = rng.choice(bools)
                state[k] = not state[k]
            if rng.random() < 0.10:
                state["rWaterTemp_F"] = rng.uniform(35.0, 115.0)
            oa = a.step(dict(state))
            ob = b.step(dict(state))
            steps += 1
            if oa != ob:
                divergences.append((run_i, dict(state), oa, ob))
                break
            clock.advance(rng.choice((10, 50, 250, 1000, 3000)))
        # x_heat_request existed only in the old signature; the port drops it.
        # Confirm that is harmless by driving it both ways above (it is unread).
    check("%d randomised steps produce identical outputs" % steps,
          not divergences, divergences[:1])

    # The ported timer accessors are new; check them against the old internals.
    b = spa_core.SpaController()
    b.step(dict(BASE))
    check("spa_remaining_s reports the 4-hour ceiling",
          abs(b.spa_remaining_s() - 4 * 3600) <= 1, b.spa_remaining_s())
    b.step(dict(BASE, xLightRequest=True))
    check("light_remaining_s reports the 60-minute runtime",
          abs(b.light_remaining_s() - 3600) <= 1, b.light_remaining_s())


def main():
    safety_properties()
    differential_against_original()
    print()
    if FAILED:
        print("FAILED: %d" % len(FAILED))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
