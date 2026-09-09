"""
Spa control core — the safety-critical logic, ported unchanged from the proven
openplc-hot-tub firmware (v2.0) that has been running the tub.

Deliberately free of hardware imports: no machine, no Pin, no ADC, no display,
no link. One call to SpaController.step(inputs) is one control cycle, exactly as
before. That keeps it testable on a laptop (tools/test_spa_core.py) and means the
move to a two-board architecture cannot quietly change how the plant behaves.

Plant assumptions (unchanged):
- 240 Vac supply, 5 kW heater on a single contactor
- Pump 1 two-speed (LOW/HIGH interlocked); pumps 2 and 3 single-speed
- Temperatures in Fahrenheit
"""

try:
    import utime as time_mod
except ImportError:          # CPython, for host tests
    import time as time_mod


def ticks_ms():
    if hasattr(time_mod, "ticks_ms"):
        return time_mod.ticks_ms()
    return int(time_mod.monotonic() * 1000)


def ticks_diff(now, then):
    if hasattr(time_mod, "ticks_diff"):
        return time_mod.ticks_diff(now, then)
    return now - then


def ticks_add(t, delta):
    if hasattr(time_mod, "ticks_add"):
        return time_mod.ticks_add(t, delta)
    return t + delta


# Fault codes, mirrored by SPALINK_FAULT_* on the wire and by the HMI's fault text.
FAULT_NONE        = 0
FAULT_NO_FLOW     = 1
FAULT_HIGH_LIMIT  = 2
FAULT_OVERTEMP    = 3
FAULT_ESTOP       = 4
FAULT_TEMP_SENSOR = 5

FAULT_TEXT = {
    FAULT_NONE: "OK",
    FAULT_NO_FLOW: "NO FLOW",
    FAULT_HIGH_LIMIT: "HIGH LIMIT",
    FAULT_OVERTEMP: "OVER TEMP",
    FAULT_ESTOP: "E-STOP",
    FAULT_TEMP_SENSOR: "TEMP SENSOR",
}


class OnDelay:
    """TON: q goes true once signal has been continuously true for delay_ms."""

    def __init__(self, delay_ms):
        self.delay_ms = int(delay_ms)
        self._start_ms = None
        self.q = False

    def update(self, signal):
        now = ticks_ms()
        if signal:
            if self._start_ms is None:
                self._start_ms = now
            self.q = ticks_diff(now, self._start_ms) >= self.delay_ms
        else:
            self._start_ms = None
            self.q = False
        return self.q


class RollingAverage:
    """Fixed-length circular-buffer rolling average. O(1) update, no heap churn."""

    def __init__(self, n):
        self._buf = [0.0] * n
        self._n = n
        self._idx = 0
        self._count = 0
        self._total = 0.0

    def update(self, value):
        v = float(value)
        if self._count < self._n:
            self._count += 1
        else:
            self._total -= self._buf[self._idx]
        self._buf[self._idx] = v
        self._total += v
        self._idx = (self._idx + 1) % self._n
        return self._total / self._count


class ThermostatHeat:
    def __init__(self):
        self.heat_on = False

    def update(self, enable, temp_f, setpoint_f, hysteresis_f, flow_ok,
               high_limit_ok, flow_proven, min_on_time_elapsed):
        sp_low = setpoint_f - (hysteresis_f * 0.5)
        sp_high = setpoint_f + (hysteresis_f * 0.5)

        if ((not enable) or (not flow_ok) or (not high_limit_ok)
                or (not flow_proven) or (not min_on_time_elapsed)):
            self.heat_on = False
        elif temp_f < sp_low:
            self.heat_on = True
        elif temp_f > sp_high:
            self.heat_on = False

        return self.heat_on


class SpaController:
    """One call to step() = one control cycle (similar to a PLC scan)."""

    def __init__(self):
        # Tunables (Fahrenheit)
        self.temp_setpoint_f = 100.0
        self.temp_hysteresis_f = 0.5
        self.max_safe_temp_f = 105.0
        self.flow_prove_ms = 5_000
        self.pump_preheat_ms = 5_000
        self.default_run_ms = 4 * 60 * 60 * 1000   # 4-hour ceiling; resets on any HMI press
        self.light_run_ms = 60 * 60 * 1000         # 60-minute light runtime

        # Status/fault outputs
        self.x_fault = False
        self.i_fault_code = FAULT_NONE

        # Flow-loss fault latch. In the v2.0 firmware FAULT_NO_FLOW is unreachable:
        # its guard is `flow_proven and not xFlowSwitch`, but flow_proven is
        # recomputed from (any_pump and xFlowSwitch) earlier in the same scan, so it
        # is already False on the scan the flow switch opens. Losing flow therefore
        # stopped the heater silently and never told anyone why. Latching "flow was
        # proven for this pump run" makes the fault reachable. Set False for
        # bit-exact v2.0 behaviour (tools/test_spa_core.py uses that to diff the
        # port against the firmware on the tub).
        self.flow_fault_latch = True
        self._flow_was_proven = False

        # Internal FB equivalents
        self._flow_prove = OnDelay(self.flow_prove_ms)
        self._pump_min_run = OnDelay(self.pump_preheat_ms)
        self._thermostat = ThermostatHeat()
        self._run_timer_start_ms = None
        self._light_timer_start_ms = None

    # ── Timer introspection, for MSG_TIMERS ──────────────────────────────────
    def spa_remaining_s(self):
        if self._run_timer_start_ms is None:
            return 0
        left = self.default_run_ms - ticks_diff(ticks_ms(), self._run_timer_start_ms)
        return max(0, left // 1000)

    def light_remaining_s(self):
        if self._light_timer_start_ms is None:
            return 0
        left = self.light_run_ms - ticks_diff(ticks_ms(), self._light_timer_start_ms)
        return max(0, left // 1000)

    def step(self, inputs):
        x_spa_enable_cmd = bool(inputs.get("xSpaEnable", False))
        x_pump_request = bool(inputs.get("xPumpRequest", False))
        x_pump1_high_request = bool(inputs.get("xPump1HighRequest", False))
        x_pump2_request = bool(inputs.get("xPump2Request", False))
        x_pump3_request = bool(inputs.get("xPump3Request", False))
        x_jets_request = bool(inputs.get("xJetsRequest", False))
        x_blower_request = bool(inputs.get("xBlowerRequest", False))
        x_light_request = bool(inputs.get("xLightRequest", False))
        x_flow_switch = bool(inputs.get("xFlowSwitch", False))
        x_high_limit_ok = bool(inputs.get("xHighLimitOK", True))
        x_remote_estop_ok = bool(inputs.get("xRemoteEStopOK", True))
        x_temp_sensor_ok = bool(inputs.get("xTempSensorOK", True))
        r_water_temp_f = float(inputs.get("rWaterTemp_F", 70.0))

        now = ticks_ms()
        if x_spa_enable_cmd:
            if self._run_timer_start_ms is None:
                self._run_timer_start_ms = now
            run_expired = ticks_diff(now, self._run_timer_start_ms) >= self.default_run_ms
        else:
            self._run_timer_start_ms = None
            run_expired = False

        x_spa_enable = x_spa_enable_cmd and (not run_expired)

        if x_light_request:
            if self._light_timer_start_ms is None:
                self._light_timer_start_ms = now
            light_expired = ticks_diff(now, self._light_timer_start_ms) >= self.light_run_ms
        else:
            self._light_timer_start_ms = None
            light_expired = False

        # ── Two-tier permissive ───────────────────────────────────────────────
        # Freeze-protection permissive: safety interlocks only, NO run timer, so
        # the thermostat and its circulation pump stay alive indefinitely and the
        # setpoint is always maintained (critical for winter freeze protection).
        x_freeze_permissive = (
            x_high_limit_ok
            and x_remote_estop_ok
            and (r_water_temp_f <= self.max_safe_temp_f)
        )
        # Full permissive: additionally requires spa-enable + run timer not expired.
        x_permissive = x_spa_enable and x_freeze_permissive

        x_heat_active = r_water_temp_f < self.temp_setpoint_f

        x_pump1_high = x_permissive and x_pump1_high_request
        x_pump1_low = (
            (x_freeze_permissive and x_heat_active)   # thermostat circulation
            or (x_permissive and x_pump_request)      # manual LOW request
        ) and (not x_pump1_high)

        x_pump2 = x_permissive and x_pump2_request
        x_pump3 = x_permissive and x_pump3_request
        x_any_pump = x_pump1_low or x_pump1_high or x_pump2 or x_pump3

        flow_proven = self._flow_prove.update(x_any_pump and x_flow_switch)
        min_run_elapsed = self._pump_min_run.update(x_any_pump)

        # Remember that flow was established for as long as a pump is called for;
        # cleared when the pumps stop, so the next run has to prove flow again.
        if not x_any_pump:
            self._flow_was_proven = False
        elif flow_proven:
            self._flow_was_proven = True
        flow_had_been_proven = flow_proven or (self.flow_fault_latch and self._flow_was_proven)

        x_heater = self._thermostat.update(
            # Never heat on a bad temperature reading — fail safe.
            enable=(x_freeze_permissive and x_heat_active and x_temp_sensor_ok),
            temp_f=r_water_temp_f,
            setpoint_f=self.temp_setpoint_f,
            hysteresis_f=self.temp_hysteresis_f,
            flow_ok=x_flow_switch,
            high_limit_ok=x_high_limit_ok,
            flow_proven=flow_proven,
            min_on_time_elapsed=min_run_elapsed,
        )

        x_jets = x_permissive and x_jets_request and x_any_pump
        x_blower = x_permissive and x_blower_request
        x_light = x_light_request and (not light_expired)

        # Fault priority
        self.x_fault = False
        self.i_fault_code = FAULT_NONE

        if x_spa_enable and (not x_remote_estop_ok):
            self.x_fault = True
            self.i_fault_code = FAULT_ESTOP
        elif x_spa_enable and (not x_high_limit_ok):
            self.x_fault = True
            self.i_fault_code = FAULT_HIGH_LIMIT
        elif x_spa_enable and (r_water_temp_f > self.max_safe_temp_f):
            self.x_fault = True
            self.i_fault_code = FAULT_OVERTEMP
        elif not x_temp_sensor_ok:
            self.x_fault = True
            self.i_fault_code = FAULT_TEMP_SENSOR
        elif x_any_pump and flow_had_been_proven and (not x_flow_switch):
            self.x_fault = True
            self.i_fault_code = FAULT_NO_FLOW

        if self.x_fault:
            x_pump1_low = False
            x_pump1_high = False
            x_pump2 = False
            x_pump3 = False
            x_heater = False
            x_jets = False
            x_blower = False

        return {
            "xPump1_Low": x_pump1_low,
            "xPump1_High": x_pump1_high,
            "xPump2": x_pump2,
            "xPump3": x_pump3,
            "xHeater": x_heater,
            "xJets": x_jets,
            "xBlower": x_blower,
            "xLight": x_light,
            "xFault": self.x_fault,
            "iFaultCode": self.i_fault_code,
        }
