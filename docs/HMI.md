# The HMI screen

800×480 landscape on a 480×800 panel driven rotated. The arrangement comes from
the v2.0 HMI that has been on this tub's wall; the palette, cards and dial come
from the SpaControl iOS app, so the wall and the phone read as one product.

**This has run on the hardware.** Everything below was checked on the glass, not
only in a browser.

```
p4_hmi/main/ui/
  ui_model.c    presentation logic. No LVGL, no ESP-IDF. Host-tested.
  ui.c          the screen: header, control rows, the alert
  ui_dial.c     the temperature dial and its step keys
  ui_theme.c    palette and shared styles, from the app's Theme.swift
  ui_icons.c    waves, bulb, leaf, flame, sun, transfer — as polylines
```

## Layout

```
┌──────────────────────────────────────────────────────────────────────┐
│ ☀ ════◉═══                        ᛒ    ᯤ    ⇄ LINK        14:32      │ 40
├──────────────────────────────────────────┬───────────────────────────┤
│  WATER TEMP                              │ ┌───────────────────────┐ │
│                                          │ │ ≋  Jet 1         Low  │ │
│            ╭────────────────╮            │ └───────────────────────┘ │
│           │       101        │           │ ┌──────────┬────────────┐ │
│           │   TARGET  102°   │           │ │ ≋ Jet 2  │  ≋ Jet 3   │ │
│            ╰────────────────╯            │ └──────────┴────────────┘ │
│                                          │ ┌───────────────────────┐ │
│  ┌──────────────────┬─────────────────┐  │ │ ☀  Light         40m  │ │
│  │        -         │        +        │  │ └───────────────────────┘ │
│  └──────────────────┴─────────────────┘  │ ┌──────────┬────────────┐ │
│                                          │ │ 🌿 Eco   │ 🔥 Max Jets│ │
└──────────────────────────────────────────┴─└──────────┴────────────┘─┘
```

Nothing scrolls. The temperature panel takes 60 % of the width; the controls take
the rest as four stacked rows, in the order the wall panel has them.

Every dimension below the header is **derived, not written down** — the rows and
the dial are whatever the header and the step keys leave, and `ui.c` asserts at
compile time that a control row still clears the touch floor. The firmware prints
what the arithmetic produced at boot, which is the quickest way to catch a metric
change that silently shrank something:

```
layout: rows 101 px, dial 280 px in a 294 box, step keys 78 px
```

### The header

Brightness on the left. On the right: Bluetooth, Wi-Fi, the link, the clock.

The link is two opposing arrows and the word LINK, both in one colour — green up,
amber never-connected, red with a count-up once it has gone quiet. **Holding it
for 1.5 s opens diagnostics** (link counters, framing errors, peer firmware, the
live request and status bytes). There is no gear: it is a service screen, and the
header should read at a glance. The firmware times the hold itself rather than
using LVGL's long press, which fires at 400 ms — well inside the time a wet
finger rests on something it is already pressing.

### There is no bottom strip

v2.0 had HEAT / JETS / LITE / FAULT along the foot. Each duplicated something a
control already said — heat is the ring and its halo, jets and light are the
tiles themselves — so they are gone, and their 40 px went to the dial.

What they carried that nothing else did is the *sentence*, and that is now an
**alert across the foot of the screen, present only when there is something to
say**: a fault, named and explained; "waiting for the control board"; or how long
the link has been dead. It costs no layout space the rest of the time, and when
it appears it covers the bottom of both columns — during a fault that is correct,
since nothing under it will be honoured anyway. A dead link is never announced
only there: the header says so independently.

## The dial

280 px, 32.6 mm across, and the whole of it drags. Where v2.0 had two columns of
seven-segment digits and a pair of +/− buttons, this has one ring: the water is
the filled sweep, the target is the knob you drag, and both numbers still read
out in the middle.

**The step keys** under it move the target a degree a press and repeat on hold.
Dragging a ring is a fine gesture and not everyone wants it — wet hands, cold
hands, or simply preferring a button. They take the same path as a drag, one
frame on the wire when the finger leaves however many degrees it travelled, so
nothing above `ui_dial.c` can tell which the user used.

**A dead centre.** LVGL's arc takes presses anywhere in its bounding box, which
is what makes the ring easy to grab and would also let a splash landing on the
readout spin the setpoint. The middle 112 px ignores touch. It fails open: if the
input device will not report where a press landed the press is accepted, so the
worst case is the old behaviour rather than a dial that will not turn.

### The knob, and why it went missing

LVGL sizes and places the arc knob from the **`LV_PART_INDICATOR`'s arc width**,
not the main part:

```c
int32_t indic_width = lv_obj_get_style_arc_width(obj, LV_PART_INDICATOR);
r -= indic_width / 2;
knob_area->x1 = center->x + knob_x - left_knob - indic_width / 2;
```

The target arc's indicator is deliberately invisible, and it had been left at
width 0 — so the knob came out radius 7 instead of 19, sitting 12 px off the
stroke. It rendered perfectly in the browser mock-up, which draws the dial with
SVG, and was only visible as wrong on the glass. **An invisible indicator still
has to carry its real width.**

The knob also has to fit, and it only does when `knob_r ≤ arc_w/2`. So the arc
sits *inside* a box larger by the overhang: `UI_DIAL_SIZE` is derived from
`UI_DIAL_BOX`, the knob and the stroke, so the relationship cannot drift back.

## Touch, for wet hands

A wet fingertip spreads, so the contact patch is larger and less certain than the
pointer the user thinks they are aiming, and they cannot see what is under it.
Phone guidance lands near 7 mm; that is not enough here.

**Anything that moves the plant gets at least 9 mm in its smallest dimension.**
At 218 ppi one pixel is 0.117 mm, so 9 mm is 77 px — hence `UI_TOUCH_MIN 78`.

| control | px | mm | |
|---|---|---|---|
| control tile | 101 | 11.8 | |
| step key | 78 | 9.1 | full half-width of the card |
| dial drag band | 84 | 9.8 | the annulus outside the dead centre |
| brightness slider | 46 | 5.4 | 10 px track, the rest invisible slop |

Jet 1 is one full-width button cycling Off → Low → High rather than three
segments. Off to High takes two presses; on a control used a few times a session
that is worth a target twice the size. Every target also carries invisible slop
(`lv_obj_set_ext_click_area`), so the gap between two tiles belongs to whichever
is nearer rather than to neither.

## Colour carries state

**A control's fill comes from the controller; its selection comes from the user.**

`spa_state_t` is what the S3 says is true. `ui_intent_t` is what the user has
asked for. They are separate structs and nothing copies one into the other, which
is why the screen cannot show a load as running that is not. A press moves a bit
in the intent and nothing else; the tile lights up when — and only when — the
S3's next `STATUS` reports the load energised.

| state | means | drawn as |
|---|---|---|
| `OFF` | not asked for, not running | card ground, muted |
| `ON` | the S3 reports it energised | filled, white text, bright ring |
| `PENDING` | asked for, under 1.5 s ago | outlined only |
| `REFUSED` | asked for, still not running | filled amber, white text |

`REFUSED` is the state that earns the model. The S3 refuses on interlocks and on
faults, and a screen that showed the press as success would leave somebody
believing the jets are running when they are not.

`ON` without a request is normal: the S3 runs Jet 1 itself to circulate water for
the heater, so the tile reads **Low** against an **Off** selection.

### Why the fill is not the accent at full strength

White on the accents measures 1.4–2.0:1 — under the 4.5:1 floor, and on the
yellow Light tile close to unreadable:

| fill | white | dark |
|---|---|---|
| water `#40D4E2` | 1.79 | 8.51 |
| light `#FFD43B` | 1.43 | 10.69 |
| good `#3ED67F` | 1.89 | 8.08 |
| heat `#FF9F45` | 2.04 | 7.47 |

So the fill is the accent **mixed 45 % over the screen ground** (`UI_FILL_MIX`),
where white clears 4.5:1 on every one, and the full-strength colour goes to a
3 px border, where nothing has to be legible against it. A vivid ring, a deep
fill of the same hue, and white that actually reads.

### Modes take over the colour

While Eco or Max Jets owns the pumps, the dial's sweep *and its halo* and all
three jet tiles wear that mode's colour — green or orange. Between the ring and
three of the six tiles that is most of the colour on the glass, so the mode is
readable across a room.

Priority is Max Jets → Eco → heating → water: a mode is something the user chose
and is still running, and it outranks a thermostat that comes and goes on its
own. Heating is not lost — the halo still appears only when the heater is on, it
just wears the mode's colour, because an orange glow under a green ring reads as
two states at once.

## Eco and Max Jets

Both are **applied on this board**, as a rewrite of the request bits, so the
control node never learns a UI concept:

* **Eco** — Jet 1 low, jets 2 and 3 off, setpoint locked to 80 °F. The previous
  setpoint is remembered and sent back when Eco is switched off.
* **Max Jets** — everything on high, for 20 minutes, then off by itself.

Entering either cancels the other. The numbers are inherited from v2.0. The
`modes` byte still goes on the wire — it is already in the protocol and lets the
S3's log say why the pumps are doing what they are doing — but nothing depends on
the S3 reading it.

## There is no off

The panel is the tub's only control surface and is always on, so `spa_enable` is
asserted at `ui_intent_init()` and never withdrawn. There is no power button and
no standby state.

This has one consequence on the control node, and it is deliberate.
`spa_core.step()` only resets its run timer when the enable command *drops*, so a
continuously-held enable would hit the four-hour ceiling and close the permissive
by itself — every control dead, with no way to recover from the screen. So
`s3_control/main.py` sets `ctrl.default_run_ms = None`.

What the ceiling protected against is still covered, and better. It guarded a tub
left enabled by someone who walked away; here enable comes from a board, not a
person, and the moment that board stops talking the link failsafe drops it inside
1.5 s — crash, unplug, or reflash. The thermostat is unaffected: it runs under
the freeze permissive, which never consulted this timer.

**Put the ceiling back if a wired panel switch is ever fitted in parallel**,
because then a person can hold enable and the failsafe cannot. The default in
`spa_core.py` is unchanged at four hours, and `tools/test_spa_core.py` covers both.

## Not wired yet

**Wi-Fi is now real.** With `CONFIG_SPA_HMI_NET` the P4 associates through the
C6 over the module's internal SDIO bus and the header's Wi-Fi indicator follows
an actual address — see `docs/MQTT.md`. It lights only once there is a lease, not
on association: an association without an address carries nothing, and showing it
as up would be the same kind of lie as a tile lighting for a dead relay.

**Bluetooth is now real too.** The indicator follows an actual phone connection
to the provisioning service (`docs/BLE.md`). It shows a *connected phone*, not
merely that the radio exists — an indicator lit for a stack nobody is talking to
would say nothing worth the pixels.

**The clock** now keeps real time over SNTP — but only once `CONFIG_SPA_HMI_TZ`
is set. Left empty it stays `--:--`, deliberately: the alternative is UTC on a
wall panel, and a wrong time is worse than no time. The carrier's RTC on the
shared I²C bus is still undriven; SNTP made it unnecessary for now.

**Bold** is the other gap. LVGL's stock Montserrat is a single weight, so
`UI_FONT_*_B` currently resolve to the Medium faces. A bold face has to be
generated — one `lv_font_conv` run per size, and the command is in `ui_theme.h`
beside the `UI_FONT_BOLD` switch. The layout is already final; only the weight
changes when the files appear.

## The bench peer

`CONFIG_SPA_HMI_BENCH_PEER`, **off by default**, stands a fake control board up
inside this firmware so the screen can be judged before an S3 exists. It answers
the panel's own requests through the same `spa_state_apply()` path, so the UI
cannot tell the difference — which is exactly why it must never ship. A build
with it on warns twice on the console at boot.

It is deliberately not a second control core: it honours requests, runs a crude
thermostat and counts the light's timer, and has no faults or interlocks. That
logic belongs to `spa_core.py` and is diff-tested against the firmware running
the tub. To drive the UI through fault and refusal states, use the browser bench.

## Testing

`tools/test_ui_model.c` covers `ui_model.c` on a host: request derivation, both
modes and their setpoint hand-off, the Max Jet timeout, all four control states
including the millisecond-clock wrap, and the dial's optimistic target.

There is also a browser mock-up of the whole panel driven by a simulated S3 —
`spa_core.py`'s permissive, thermostat and fault priority in JavaScript — which is
how the layout and every state were reviewed before flashing.

**Its limit, learned the hard way:** it draws the dial with SVG and the panel
draws it with LVGL's arc widget, and the two do not fail alike. The missing knob
rendered perfectly there. Treat it as a layout and behaviour reference, not a
rendering one — anything about how a widget actually draws has to be seen on the
board.
