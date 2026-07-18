# 📦 ESP32-CAM + ESP32-CAM-MB Case

Two-piece enclosure for an **ESP32-CAM (AI-Thinker) stacked on its ESP32-CAM-MB USB programming board** — thin **1.6 mm walls** with a clean **vertical ribbed texture** outside.

![exploded preview](../images/preview_case_esp32cam.png)

## 📁 Files

| File | Description |
|---|---|
| `esp32cam_mb_case.scad` | Fully **parametric** OpenSCAD source |
| `esp32cam_case_body.stl` | Main body — 59.3 × 33 × 18.7 mm |
| `esp32cam_case_lid.stl` | Snap-friction lid — 4.2 mm |
| `esp32cam_case_print_all.stl` | Both parts side by side (single plate) |

## 🖨️ Printing

- **No supports** — body upright (bottom on the bed), lid plate on the bed
- PLA or PETG · **0.2 mm** layers · 3–4 perimeters (1.6 mm walls = 4 × 0.4 lines)
- Infill doesn't matter (walls are solid)
- ~1h30 total print time

## 📐 Dimensions

- Outside: **59.3 × 33 × 20.2 mm** (ribs included)
- Inside: 54.5 × 28.2 × 17 mm (fits the MB board 52.5 × 26 mm + stacked camera)

## ✨ Features

- Ø11 mm **lens window** (front face)
- **Flash LED window** in the lid (`top_flash`, AI-Thinker LED shines upwards — set `front_flash=true` if yours faces forward)
- **Micro-USB** slot (rear face) + pin-hole for the MB **reset button**
- Side **microSD slot** — insert/remove the card without opening the case
- **WiFi antenna window** in the lid (better signal)
- 6 side **ventilation slots**
- 2× **M3 holes** in the bottom (wall/desk mounting)
- **Friction-fit lid** — no screws, no hardware

## ⚠️ Measure before printing

Default dimensions match the standard AI-Thinker + MB stack. Check yours and adjust at the top of `esp32cam_mb_case.scad`:

| Parameter | Default | Check |
|---|---|---|
| `in_l / in_w / in_h` | 54.5 / 28.2 / 17 | Your MB board size + total stack height |
| `cam_z` | 14.5 | Lens center height (from inside bottom) |
| `cam_y` | 0 | Lens lateral offset |
| `flash_x / flash_y` | 20 / -8.5 | Flash LED position |
| `usb_z` | 5 | Micro-USB connector height |
| `sd_x / sd_z` | 5 / 12.3 | microSD slot position (left wall) |
| `lid_clr` | 0.25 | Lid clearance — **+0.05 if too tight, -0.05 if too loose** |

## 🔧 Re-export STLs

```bash
openscad -o esp32cam_case_body.stl -D part=1 esp32cam_mb_case.scad   # body
openscad -o esp32cam_case_lid.stl  -D part=2 esp32cam_mb_case.scad   # lid
openscad -o esp32cam_case_print_all.stl -D part=3 esp32cam_mb_case.scad  # both
```

## 🎨 Texture tuning

- `rib_step = 2.1` → rib spacing (larger = airier, smaller = finer)
- `rib_r = 0.85` → rib depth
- `rib = false` → smooth case
