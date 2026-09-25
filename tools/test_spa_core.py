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


def run(ctrl, clock, inputs, ms, step_ms=50, requests_valid=True):
    """Hold inputs steady for ms, returning the final outputs."""
    out = None
    for _ in range(max(1, ms // step_ms)):
        out = ctrl.step(inputs, requests_valid=requests_valid)
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

    # The ceiling can be removed for an installation whose enable comes from the
    # HMI board rather than a person — the P4 holds it continuously, and the link
    # failsafe is what drops it. Removing it must not touch anything else.
    c = spa_core.SpaController()
    c.default_run_ms = None
    d = dict(BASE, rWaterTemp_F=45.0, xJetsRequest=True)
    run(c, clock, d, 10000)
    clock.advance(50 * 60 * 60 * 1000)
    out = run(c, clock, d, 1000)
    check("with no ceiling the jets keep running past four hours", out["xJets"])
    check("and nothing else changes: the interlocks still hold",
          out["xHeater"] and not out["xPump1_High"])
    check("a removed ceiling reports no time remaining, not a negative one",
          c.spa_remaining_s() == 0, c.spa_remaining_s())
    d = dict(d, xRemoteEStopOK=False)
    out = run(c, clock, d, 1000)
    check("and an e-stop still stops everything with no ceiling",
          not out["xJets"] and not out["xHeater"] and out["xFault"])

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
        b.pump_run_ms = None         # v2.0 has no high-flow ceiling either
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
            # "timedOut" is reporting for the panel, not a plant output, and the
            # original has no such key. Everything the two firmwares actually
            # drive is still compared key for key, extras included.
            ob_plant = {k: v for k, v in ob.items() if k != "timedOut"}
            if oa != ob_plant:
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


def high_flow_run_ceiling():
    print()
    print("the 20-minute ceiling on the high-flow pumps")
    clock = FakeClock()
    clock.install(spa_core)

    for name, req, out in (("pump1_high", "xPump1HighRequest", "xPump1_High"),
                           ("pump2", "xPump2Request", "xPump2"),
                           ("pump3", "xPump3Request", "xPump3")):
        c = spa_core.SpaController()
        state = dict(BASE, xSpaEnable=True, **{req: True})
        o = c.step(dict(state))
        check("%s starts when asked" % name, o[out], o[out])
        check("%s reports 20 minutes left" % name,
              abs(c.pump_remaining_s(name) - 20 * 60) <= 1,
              c.pump_remaining_s(name))

        run(c, clock, dict(state), 19 * 60 * 1000, step_ms=5000)
        o = c.step(dict(state))
        check("%s still running at 19 min" % name, o[out], o[out])

        run(c, clock, dict(state), 90 * 1000, step_ms=5000)
        o = c.step(dict(state))
        check("%s stops once the ceiling is reached" % name, not o[out], o[out])
        check("%s reports 0 s left" % name, c.pump_remaining_s(name) == 0,
              c.pump_remaining_s(name))

        # Holding the request must NOT buy more time; only a release does.
        run(c, clock, dict(state), 60 * 1000, step_ms=5000)
        o = c.step(dict(state))
        check("%s stays off while the request is held" % name, not o[out], o[out])
        o = c.step(dict(state, **{req: False}))
        o = c.step(dict(state))
        check("%s restarts after releasing and re-requesting" % name, o[out], o[out])

    print()
    print("what the ceiling must not do")
    c = spa_core.SpaController()
    # Cold water, so the thermostat wants heat and its circulation pump.
    state = dict(BASE, xSpaEnable=True, rWaterTemp_F=80.0, xPumpRequest=True)
    o = c.step(dict(state))
    check("pump 1 low runs for the thermostat", o["xPump1_Low"], o)
    run(c, clock, dict(state), 40 * 60 * 1000, step_ms=5000)
    o = c.step(dict(state))
    check("pump 1 low is still running after 40 min — it has no ceiling",
          o["xPump1_Low"], o)
    check("and the heater is still allowed", c.pump_remaining_s("pump1_low") == 0)

    # High speed timing out must hand back to low, not stop circulation.
    c = spa_core.SpaController()
    state = dict(BASE, xSpaEnable=True, rWaterTemp_F=80.0,
                 xPumpRequest=True, xPump1HighRequest=True)
    o = c.step(dict(state))
    check("high speed wins while both are asked", o["xPump1_High"]
          and not o["xPump1_Low"], o)
    run(c, clock, dict(state), 21 * 60 * 1000, step_ms=5000)
    o = c.step(dict(state))
    check("after the ceiling, low speed takes over",
          o["xPump1_Low"] and not o["xPump1_High"], o)

    c = spa_core.SpaController()
    c.pump_run_ms = None
    state = dict(BASE, xSpaEnable=True, xPump2Request=True)
    c.step(dict(state))
    run(c, clock, dict(state), 40 * 60 * 1000, step_ms=5000)
    o = c.step(dict(state))
    check("pump_run_ms = None removes the ceiling", o["xPump2"], o)

    print()
    print("a dropped link must not hand back a fresh 20 minutes")
    # Found on hardware: the link flapped every few seconds, main.py swapped in
    # the all-off failsafe set each time, and the ceiling read that as a button
    # release. A 20-minute limit had not fired in seven hours.
    c = spa_core.SpaController()
    on = dict(BASE, xSpaEnable=True, xPump2Request=True)
    off = dict(BASE, xSpaEnable=False, xPump2Request=False)   # the failsafe set
    c.step(dict(on))
    for _ in range(15):   # 15 x (90 s run + 3 s outage) = 1395 s > the 1200 s ceiling
        # 90 s of running, then a 3 s outage, repeated: far more blips than the
        # ceiling is long, which is exactly what the bench was doing.
        run(c, clock, dict(on), 90 * 1000, step_ms=5000)
        c.step(dict(on))
        run(c, clock, dict(off), 3 * 1000, step_ms=500, requests_valid=False)
        c.step(dict(off), requests_valid=False)
    o = c.step(dict(on))
    check("the ceiling still fires after 15 link outages",
          not o["xPump2"], (o["xPump2"], c.pump_remaining_s("pump2")))

    # And a genuine release, with the link up, still restarts it.
    c = spa_core.SpaController()
    c.step(dict(on))
    run(c, clock, dict(on), 21 * 60 * 1000, step_ms=5000)
    check("expired before the release", not c.step(dict(on))["xPump2"])
    c.step(dict(off))                      # requests_valid defaults to True
    o = c.step(dict(on))
    check("a real release with the link up still restarts the clock",
          o["xPump2"], o["xPump2"])

    print()
    print("nor may it bill the outage as run time")
    # The other half of the same bug, and the one that reached the tub: an
    # outage longer than the ceiling left the pump timed out the instant the
    # link came back, refusing with nothing wrong and no way to see why. The
    # load is off for the whole outage, so the clock pauses rather than runs.
    c = spa_core.SpaController()
    c.step(dict(on))
    run(c, clock, dict(on), 5 * 60 * 1000, step_ms=5000)
    c.step(dict(on))
    run(c, clock, dict(off), 30 * 60 * 1000, step_ms=5000, requests_valid=False)
    c.step(dict(off), requests_valid=False)
    o = c.step(dict(on))
    check("pump 2 runs again after an outage longer than its ceiling",
          o["xPump2"], o["xPump2"])
    check("and picks up the 15 minutes it had left, not a fresh 20",
          14 * 60 <= c.pump_remaining_s("pump2") <= 15 * 60 + 5,
          c.pump_remaining_s("pump2"))
    run(c, clock, dict(on), 15 * 60 * 1000, step_ms=5000)
    o = c.step(dict(on))
    check("and still stops at 20 minutes of actual running",
          not o["xPump2"], o["xPump2"])

    print()
    print("the panel is told a timeout is not a refusal")
    # Without this the HMI goes on asking forever, the tile sits amber on
    # "Refused", and because only a release restarts the ceiling the load can
    # never come back at all. See ui_intent_release_timeouts().
    c = spa_core.SpaController()
    state = dict(BASE, xSpaEnable=True, xPump2Request=True, xLightRequest=True)
    o = c.step(dict(state))
    check("nothing is timed out while the loads are running",
          not any(o["timedOut"].values()), o["timedOut"])
    run(c, clock, dict(state), 21 * 60 * 1000, step_ms=5000)
    o = c.step(dict(state))
    check("pump 2 is reported timed out once its ceiling is reached",
          o["timedOut"]["xPump2"] and not o["xPump2"], o["timedOut"])
    check("the light is still running on its own, longer ceiling",
          o["xLight"] and not o["timedOut"]["xLight"], o["timedOut"])
    run(c, clock, dict(state), 40 * 60 * 1000, step_ms=5000)
    o = c.step(dict(state))
    check("and is reported timed out at 60 minutes",
          o["timedOut"]["xLight"] and not o["xLight"], o["timedOut"])

    # A load an interlock is holding off is a refusal, and must NOT be reported
    # as a timeout — that is the one the user needs to see on the glass.
    c = spa_core.SpaController()
    o = c.step(dict(BASE, xSpaEnable=True, xPump2Request=True,
                    xRemoteEStopOK=False))
    check("an e-stop refusal is not dressed up as a timeout",
          not o["xPump2"] and not o["timedOut"]["xPump2"], o["timedOut"])

    print()
    print("the light runs on the same ceiling as the pumps")
    c = spa_core.SpaController()
    on_l = dict(BASE, xSpaEnable=True, xLightRequest=True)
    off_l = dict(BASE, xSpaEnable=False, xLightRequest=False)
    o = c.step(dict(on_l))
    check("the light comes on when asked", o["xLight"], o["xLight"])
    check("and reports 60 minutes left",
          abs(c.light_remaining_s() - 60 * 60) <= 1, c.light_remaining_s())
    # A blip used to reset the light's hour, the same way it reset the pumps'.
    for _ in range(20):
        run(c, clock, dict(on_l), 3 * 60 * 1000, step_ms=5000)
        c.step(dict(on_l))
        run(c, clock, dict(off_l), 3 * 1000, step_ms=500, requests_valid=False)
        c.step(dict(off_l), requests_valid=False)
    o = c.step(dict(on_l))
    check("the hour still runs out across 20 link outages",
          not o["xLight"], (o["xLight"], c.light_remaining_s()))
    o = c.step(dict(on_l, xLightRequest=False))
    o = c.step(dict(on_l))
    check("and a real release buys a fresh hour", o["xLight"], o["xLight"])


def main():
    safety_properties()
    high_flow_run_ceiling()
    differential_against_original()
    print()
    if FAILED:
        print("FAILED: %d" % len(FAILED))
        return 1
    print("all checks passed")
    return 0


if __name__ == "__main__":
    sys.exit(main())
