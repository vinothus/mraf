#!/usr/bin/env python3
"""Generate the wiring/block diagrams as standalone SVG files."""
from pathlib import Path

OUT = Path(__file__).resolve().parent.parent / "hardware" / "diagrams"

FONT = "DejaVu Sans, Segoe UI, Arial, sans-serif"
DARK = "#14202b"
MUTED = "#5b6b7a"
BLUE = "#e3f0ff"
GREEN = "#e6f7ec"
AMBER = "#fff4d6"
RED = "#fde8e8"
GREY = "#f1f4f7"
WHITE = "#ffffff"


def start(w, h, title, subtitle=""):
    return f'''<svg xmlns="http://www.w3.org/2000/svg" width="{w}" height="{h}" viewBox="0 0 {w} {h}">
<rect width="{w}" height="{h}" fill="{WHITE}"/>
<style>
text {{ font-family: {FONT}; fill: {DARK}; }}
.t {{ font-size: 26px; font-weight: 700; }}
.s {{ font-size: 14px; fill: {MUTED}; }}
.b {{ font-size: 15px; font-weight: 700; }}
.n {{ font-size: 12.5px; }}
.small {{ font-size: 11.5px; fill: {MUTED}; }}
.pin {{ font-size: 12px; font-family: DejaVu Sans Mono, Consolas, monospace; }}
.title {{ font-size: 26px; font-weight: 700; }}
</style>
<text x="40" y="46" class="title">{title}</text>
<text x="40" y="70" class="s">{subtitle}</text>
'''


def end():
    return "</svg>\n"


def box(x, y, w, h, title, lines=(), fill=BLUE, stroke="#4a7fb5", rx=10):
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="{rx}" fill="{fill}" stroke="{stroke}" stroke-width="2"/>\n'
    s += f'<text x="{x+12}" y="{y+24}" class="b">{title}</text>\n'
    yy = y + 45
    for line in lines:
        s += f'<text x="{x+12}" y="{yy}" class="n">{line}</text>\n'
        yy += 18
    return s


def box_center(x, y, w, h, title, lines=(), fill=BLUE, stroke="#4a7fb5"):
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="10" fill="{fill}" stroke="{stroke}" stroke-width="2"/>\n'
    s += f'<text x="{x+w/2}" y="{y+26}" text-anchor="middle" class="b">{title}</text>\n'
    yy = y + 48
    for line in lines:
        s += f'<text x="{x+w/2}" y="{yy}" text-anchor="middle" class="n">{line}</text>\n'
        yy += 18
    return s


def text(x, y, value, cls="n", anchor="start", weight=None):
    w = f' font-weight="{weight}"' if weight else ""
    return f'<text x="{x}" y="{y}" class="{cls}" text-anchor="{anchor}"{w}>{value}</text>\n'


def line(x1, y1, x2, y2, color="#33475b", width=2, dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    return f'<line x1="{x1}" y1="{y1}" x2="{x2}" y2="{y2}" stroke="{color}" stroke-width="{width}"{d}/>\n'


def poly(points, color="#33475b", width=2, fill="none", dash=None):
    d = f' stroke-dasharray="{dash}"' if dash else ""
    pts = " ".join(f"{a},{b}" for a, b in points)
    return f'<polyline points="{pts}" fill="{fill}" stroke="{color}" stroke-width="{width}"{d}/>\n'


def arrow(x1, y1, x2, y2, color="#33475b", width=2, dash=None):
    import math
    angle = math.atan2(y2 - y1, x2 - x1)
    size = 10
    a1 = angle + math.radians(155)
    a2 = angle - math.radians(155)
    p1 = (x2 + size * math.cos(a1), y2 + size * math.sin(a1))
    p2 = (x2 + size * math.cos(a2), y2 + size * math.sin(a2))
    s = line(x1, y1, x2, y2, color, width, dash)
    s += f'<polygon points="{x2},{y2} {p1[0]:.1f},{p1[1]:.1f} {p2[0]:.1f},{p2[1]:.1f}" fill="{color}"/>\n'
    return s


def dot(x, y, color="#33475b", r=4):
    return f'<circle cx="{x}" cy="{y}" r="{r}" fill="{color}"/>\n'


def note(x, y, lines, w=520, fill=AMBER, stroke="#c99a2e"):
    h = 20 + 18 * len(lines)
    s = f'<rect x="{x}" y="{y}" width="{w}" height="{h}" rx="8" fill="{fill}" stroke="{stroke}" stroke-width="1.5"/>\n'
    yy = y + 22
    for ln in lines:
        s += text(x + 12, yy, ln, "n")
        yy += 18
    return s


def system_block():
    s = start(1200, 780, "MRAF system block diagram",
              "Off-the-shelf modules only. No custom PCB. Mains-derived relay rail; battery-backed controller rail.")
    s += box(40, 110, 250, 150, "3-phase mains", [
        "415 V, 50 Hz, 3-phase + N + PE",
        "5 HP borewell pump",
        "Licensed electrician required",
    ], AMBER, "#c99a2e")
    s += box(340, 110, 230, 150, "Protection", [
        "3-pole isolator 32 A",
        "4-pole RCCB 40 A / 30 mA",
        "3-pole MCB 16 A (C or D)",
        "Phase-failure relay",
    ], RED, "#c05656")
    s += box(620, 110, 230, 150, "DOL starter", [
        "9 A AC-3 contactor",
        "Thermal overload 5.5-8 A",
        "Set ~7 A (FLC 6.9 A)",
        "RC snubber / MOV on coil",
    ], GREEN, "#4f9b67")
    s += box(900, 110, 260, 150, "Pump + field safety", [
        "5 HP 3-phase pump",
        "E-stop (control chain)",
        "Tank-full float (NC)",
        "Sump-dry float (NC)",
    ], AMBER, "#c99a2e")
    s += arrow(290, 185, 338, 185)
    s += arrow(570, 185, 618, 185)
    s += arrow(850, 185, 898, 185)

    s += box(40, 330, 250, 175, "Mains-derived 5 V rail", [
        "230 V AC -> 5 V 3 A SMPS",
        "Feeds 16-ch relay coils",
        "JD-VCC jumper removed",
        "Drops out on power failure",
    ], RED, "#c05656")
    s += box(340, 330, 250, 175, "ESP32 controller", [
        "ESP32 DevKit V1 38-pin",
        "Wi-Fi + HTTP server",
        "NTP + DS3231 RTC",
        "Schedules + pump state machine",
    ], BLUE, "#4a7fb5")
    s += box(640, 330, 250, 175, "Relay module", [
        "16-channel, 5 V, active-LOW",
        "12 scheduled load outputs",
        "1 dedicated pump output",
        "Logic on 3.3 V, coils on 5 V",
    ], GREEN, "#4f9b67")
    s += box(940, 330, 220, 175, "Loads", [
        "Lights, aerators, fans",
        "Drip valves / pump starter",
        "All loads drop on outage",
        "Manual override + boost",
    ], GREY, "#7d8b99")

    s += box(40, 555, 300, 175, "Battery-backed 5 V rail", [
        "12.8 V LiFePO4 + LiFePO4 charger",
        "LVD protects battery",
        "5 V buck -> ESP32 / sensors",
        "Pump never runs on battery",
    ], GREEN, "#4f9b67")
    s += box(400, 555, 270, 175, "Sensors", [
        "Mains presence (divider)",
        "Tank-full / sump-dry floats",
        "Battery voltage divider",
        "ACS712-30 A + 2:1 divider",
    ], BLUE, "#4a7fb5")
    s += box(730, 555, 430, 175, "Operator interface", [
        "Built-in dashboard: http://mraf.local",
        "AP fallback: MRAF-xxxxxx / farm1234",
        "Basic auth: admin / farm1234 (change it)",
        "REST API + native simulator",
    ], GREY, "#7d8b99")

    s += arrow(165, 505, 165, 553)
    s += arrow(465, 505, 465, 553)
    s += arrow(415, 417, 638, 417)
    s += arrow(765, 417, 938, 417)
    s += arrow(585, 417, 637, 417)
    s += arrow(300, 417, 338, 417)
    s += dot(415, 417)
    s += dot(585, 417)

    s += note(40, 750, ["Relay coils are on the mains rail by design: a power cut always de-energises every load."], 1080)
    s += end()
    return s


def control_wiring():
    s = start(1300, 940, "MRAF control panel wiring",
              "Pin map, relay module modification, sensors and power rails. Terminal numbers are suggestions.")
    s += box(40, 100, 360, 700, "ESP32 DevKit V1 (38-pin)", [], BLUE, "#4a7fb5")
    pins_left = [
        ("GPIO13", "Relay IN1  Shed Light"),
        ("GPIO14", "Relay IN2  Yard Light"),
        ("GPIO16", "Relay IN3  Drip A"),
        ("GPIO17", "Relay IN4  Drip B"),
        ("GPIO18", "Relay IN5  Aerator 1"),
        ("GPIO19", "Relay IN6  Aerator 2"),
        ("GPIO21", "Relay IN7  Cattle Fan"),
        ("GPIO22", "Relay IN8  Milking Light"),
        ("GPIO23", "Relay IN9  Gate Light"),
        ("GPIO25", "Relay IN10 Spare 1"),
        ("GPIO26", "Relay IN11 Spare 2"),
        ("GPIO27", "Relay IN12 Spare 3"),
        ("GPIO33", "Relay IN13 Pump"),
    ]
    pins_right = [
        ("GPIO32", "Mains sense (22k/15k)"),
        ("GPIO34", "Tank full float"),
        ("GPIO35", "Sump dry float"),
        ("GPIO36", "Battery 100k/22k"),
        ("GPIO39", "ACS712 + 2:1 divider"),
        ("GPIO4", "DS3231 SDA"),
        ("GPIO5", "DS3231 SCL"),
        ("GPIO2", "Status LED"),
        ("3V3", "Relay VCC + pull-ups"),
        ("GND", "Common logic GND"),
        ("5V", "Not used for relay coils"),
        ("EN", "Reset button"),
    ]
    y = 150
    for gpio, label in pins_left:
        s += text(55, y, gpio, "pin", weight="700")
        s += line(130, y - 4, 200, y - 4, "#4a7fb5", 1)
        s += text(208, y, label, "n")
        y += 36
    y = 150
    for gpio, label in pins_right:
        s += text(385, y, gpio, "pin", anchor="end", weight="700")
        s += line(285, y - 4, 355, y - 4, "#4a7fb5", 1)
        s += text(292, y, label, "n", anchor="end")
        y += 36

    s += box(470, 100, 420, 300, "16-channel 5 V relay module", [
        "Remove JD-VCC jumper.",
        "VCC  -> ESP32 3V3 (logic only)",
        "GND  -> ESP32 GND",
        "JD-VCC -> 5 V SMPS +",
        "Coil GND -> 5 V SMPS -",
        "IN1..IN13 -> GPIO list at left",
        "Contacts: COM/NO/NC per channel",
        "Active-LOW inputs: LOW = relay ON",
        "Use NO contacts for loads; keep all",
        "live terminals covered / in enclosure.",
    ], GREEN, "#4f9b67")
    s += note(470, 420, [
        "Why split the rails? The ESP32 stays alive on the battery,",
        "but every relay coil is on the mains-derived 5 V SMPS. During",
        "a power cut all relays drop out even if the ESP32 is still on.",
    ], 420)

    s += box(940, 100, 320, 220, "DS3231 RTC", [
        "VCC -> 3V3",
        "GND -> GND",
        "SDA -> GPIO4",
        "SCL -> GPIO5",
        "Battery: CR2032",
        "I2C address 0x68",
    ], BLUE, "#4a7fb5")
    s += box(940, 350, 320, 220, "ACS712-30 A", [
        "VCC -> 5 V (battery rail)",
        "GND -> GND",
        "OUT -> 10k/10k divider -> GPIO39",
        "66 mV/A, /2 => 33 mV/A",
        "Zero calibrated at boot (pump OFF)",
        "Series in pump line L1 only",
    ], AMBER, "#c99a2e")
    s += box(940, 600, 320, 200, "Float switches (sense)", [
        "Tank full: COM -> GPIO34, NO -> GND",
        "Sump dry: COM -> GPIO35, NO -> GND",
        "10k pull-up to 3V3 on each GPIO",
        "Use SEPARATE NC floats at mains",
        "voltage in the contactor safety chain.",
        "Never share a float common between",
        "230 V and 3.3 V circuits.",
    ], GREY, "#7d8b99")

    s += box(40, 830, 540, 90, "Battery-backed controller rail", [
        "LiFePO4 charger -> 12.8 V pack; pack -> fuse -> LVD -> 5 V buck -> ESP32 (or USB).",
        "Common GND with relay logic side only. Do not parallel the two 5 V sources.",
    ], GREEN, "#4f9b67")
    s += box(620, 830, 640, 90, "Mains-derived relay rail", [
        "230 V AC -> 5 V 3 A SMPS -> JD-VCC and all relay coil returns.",
        "Add 1 A fuse on the SMPS input and a MOV across its output.",
    ], RED, "#c05656")
    s += end()
    return s


def pump_starter():
    s = start(1300, 940, "5 HP 3-phase DOL starter and safety chain",
              "Concept diagram. Verify with a licensed electrician and local rules before energising.")
    s += text(40, 110, "POWER CIRCUIT", "t", weight="700")
    phases = ["L1", "L2", "L3"]
    xs = [80, 120, 160]
    for p, x in zip(phases, xs):
        s += text(x, 140, p, "pin", anchor="middle", weight="700")
        s += line(x, 150, x, 780, "#c05656", 3)
    s += line(200, 780, 200, 150, "#33475b", 3)
    s += text(200, 140, "N", "pin", anchor="middle", weight="700")
    s += line(240, 780, 240, 150, "#4f9b67", 3)
    s += text(240, 140, "PE", "pin", anchor="middle", weight="700")

    def power_block(y, h, label, detail, fill=GREY, stroke="#7d8b99"):
        nonlocal s
        s += f'<rect x="55" y="{y}" width="210" height="{h}" rx="8" fill="{fill}" stroke="{stroke}" stroke-width="2"/>\n'
        s += text(160, y + 22, label, "b", anchor="middle")
        s += text(160, y + 42, detail, "small", anchor="middle")

    power_block(170, 80, "3-POLE ISOLATOR", "32 A, lockable", AMBER, "#c99a2e")
    power_block(290, 80, "RCCB", "4-pole, 40 A, 30 mA", RED, "#c05656")
    power_block(410, 80, "3-POLE MCB", "16 A, C or D curve", RED, "#c05656")
    power_block(530, 100, "CONTACTOR", "9 A AC-3, 415 V coil", GREEN, "#4f9b67")
    power_block(670, 100, "OVERLOAD", "5.5-8 A, set ~7 A", GREEN, "#4f9b67")
    power_block(810, 70, "PUMP", "5 HP, 3-phase, 415 V", BLUE, "#4a7fb5")
    s += arrow(160, 250, 160, 288)
    s += arrow(160, 370, 160, 408)
    s += arrow(160, 490, 160, 528)
    s += arrow(160, 630, 160, 668)
    s += arrow(160, 770, 160, 808)
    s += text(270, 215, "All 3 phases + N switched where required", "small")
    s += text(270, 335, "30 mA may nuisance-trip on pump start; use 100 mA if needed", "small")
    s += text(270, 455, "Motor-rated breaker preferred", "small")
    s += text(270, 585, "Coil A1/A2: 415 V or 230 V according to contactor", "small")
    s += text(270, 725, "Set to motor FLC (6.9 A)", "small")

    s += text(480, 110, "CONTROL CIRCUIT (230 V, 6 A MCB)", "t", weight="700")
    s += line(510, 150, 510, 860, "#c05656", 2)
    s += line(1210, 150, 1210, 860, "#33475b", 2)
    s += text(510, 140, "L (after 6 A MCB)", "small", anchor="middle")
    s += text(1210, 140, "N", "small", anchor="middle")

    chain = [
        ("E-STOP", "NC, latching mushroom"),
        ("OVERLOAD NC", "95-96 opens on trip"),
        ("PHASE FAILURE", "NC, drops on phase loss"),
        ("TANK FULL", "safety float NC"),
        ("SUMP DRY", "safety float NC"),
    ]
    y = 180
    for label, detail in chain:
        s += f'<rect x="545" y="{y}" width="110" height="70" rx="8" fill="{RED}" stroke="#c05656" stroke-width="2"/>\n'
        s += text(600, y + 27, label, "b", anchor="middle")
        s += text(600, y + 50, detail, "small", anchor="middle")
        s += line(510, y + 35, 543, y + 35, "#c05656", 2)
        s += line(655, y + 35, 690, y + 35, "#c05656", 2)
        y += 92
    s += line(690, 215, 690, 598, "#c05656", 2)
    s += text(700, 220, "Safety chain: all NC contacts in series. Any open = coil de-energised.", "small")

    s += box(730, 610, 200, 120, "AUTO / OFF / HAND", [
        "OFF: both contacts open",
        "AUTO: ESP32 pump relay",
        "HAND: local start/stop",
    ], AMBER, "#c99a2e")
    s += line(690, 650, 728, 650, "#c05656", 2)
    s += line(930, 650, 970, 650, "#c05656", 2)

    s += box(970, 610, 230, 120, "AUTO contact", [
        "ESP32 GPIO33 -> relay IN13",
        "COM/NO dry contact",
        "Closes when firmware wants pump",
    ], GREEN, "#4f9b67")
    s += box(970, 750, 230, 110, "HAND circuit", [
        "STOP NC -> START NO",
        "Aux NO 13-14 latch",
        "Local maintenance only",
    ], GREY, "#7d8b99")
    s += line(1200, 670, 1210, 670, "#c05656", 2)
    s += line(1200, 805, 1210, 805, "#c05656", 2)
    s += line(1085, 730, 1085, 748, "#c05656", 2)
    s += arrow(1085, 748, 1085, 750)

    s += box(545, 750, 380, 110, "Contactor coil A1 / A2", [
        "A1 from selected control path",
        "A2 -> N",
        "RC snubber + MOV across A1/A2",
        "1 A fuse protects control circuit",
    ], GREEN, "#4f9b67")
    s += line(690, 725, 690, 748, "#c05656", 2)
    s += line(690, 748, 545, 748, "#c05656", 2)
    s += line(690, 748, 690, 780, "#c05656", 2)
    s += arrow(690, 780, 690, 750)

    s += note(40, 875, [
        "The ESP32 never switches 415 V directly. It only closes a 5 V relay contact in the contactor coil circuit."
    ], 1220)
    s += end()
    return s


def panel_layout():
    s = start(1200, 820, "Suggested IP65 panel layout",
              "500 x 400 x 200 mm enclosure; DIN rail and wire duct. Not to scale.")
    s += f'<rect x="50" y="100" width="1100" height="650" rx="12" fill="#fbfcfd" stroke="#33475b" stroke-width="3"/>\n'
    s += text(70, 130, "Enclosure front (door removed)", "b")
    s += f'<rect x="80" y="150" width="1040" height="80" rx="6" fill="{GREY}" stroke="#7d8b99" stroke-width="2"/>\n'
    s += text(90, 180, "DIN rail 1 (power)", "b")
    power = ["ISOLATOR", "RCCB", "MCB", "CONTACTOR", "OVERLOAD", "PHASE FAIL", "CTRL MCB", "SMPS 5V"]
    x = 95
    for item in power:
        w = 118 if item not in ("CONTACTOR", "OVERLOAD") else 132
        s += f'<rect x="{x}" y="190" width="{w-8}" height="30" rx="4" fill="{AMBER}" stroke="#c99a2e"/>\n'
        s += text(x + (w-8)/2, 210, item, "small", anchor="middle")
        x += w

    s += f'<rect x="80" y="270" width="1040" height="80" rx="6" fill="{GREY}" stroke="#7d8b99" stroke-width="2"/>\n'
    s += text(90, 300, "DIN rail 2 (control)", "b")
    control = ["TERMINALS", "RELAY 16CH", "FUSE", "BUCK 5V", "LVD", "TERMINALS"]
    x = 95
    widths = [230, 300, 90, 120, 120, 160]
    for item, w in zip(control, widths):
        s += f'<rect x="{x}" y="310" width="{w-8}" height="30" rx="4" fill="{GREEN}" stroke="#4f9b67"/>\n'
        s += text(x + (w-8)/2, 330, item, "small", anchor="middle")
        x += w

    s += box(100, 400, 300, 150, "ESP32 on standoffs", [
        "Keep antenna edge clear",
        "USB port accessible from side",
        "Label every GPIO wire",
        "Strain relief on all cables",
    ], BLUE, "#4a7fb5")
    s += box(430, 400, 260, 150, "RTC + ACS712", [
        "DS3231 with CR2032",
        "ACS712 on L1 only",
        "Divider on perf board",
        "Keep AC and signal wires apart",
    ], GREEN, "#4f9b67")
    s += box(720, 400, 380, 150, "Cable glands / entries", [
        "Bottom entry: 3-phase in, pump out",
        "Separate gland for sensors",
        "Separate gland for antenna/ethernet?",
        "Use ferrules and numbered cores",
    ], GREY, "#7d8b99")
    s += box(100, 580, 1000, 140, "12.8 V 6 Ah LiFePO4 battery", [
        "Place on the bottom of the enclosure or in a separate ventilated box.",
        "Fuse the battery positive (5 A). Keep LiFePO4 charger and LVD accessible.",
        "Vent the battery compartment and keep the LiFePO4 pack dry.",
    ], AMBER, "#c99a2e")
    s += note(40, 770, ["Keep 230/415 V wiring in its own duct. Signal wiring must not share a duct with mains."], 1120)
    s += end()
    return s


def main():
    OUT.mkdir(parents=True, exist_ok=True)
    (OUT / "system-block.svg").write_text(system_block(), encoding="utf-8")
    (OUT / "control-wiring.svg").write_text(control_wiring(), encoding="utf-8")
    (OUT / "pump-starter.svg").write_text(pump_starter(), encoding="utf-8")
    (OUT / "panel-layout.svg").write_text(panel_layout(), encoding="utf-8")
    for p in sorted(OUT.glob("*.svg")):
        print(p.name, p.stat().st_size)


if __name__ == "__main__":
    main()
