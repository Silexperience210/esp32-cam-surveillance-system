# Troubleshooting

Real issues encountered and solved while building this project.

## Camera stays in "waiting for download" (no WiFi AP appears)

**Symptom**: serial log shows `boot:0x3 (DOWNLOAD_BOOT)` / `waiting for download`.
**Cause**: the GPIO 0 → GND flashing jumper is still in place.
**Fix**: remove the jumper, press RESET. Flashing jumper is only needed during upload.

## Camera/monitor unreachable on the LAN (works on other devices)

**Symptom**: `ping` and browser fail from one PC, but the device answers from others.
**Cause**: a VPN client (e.g. Tailscale) has installed a route for your LAN subnet (`192.168.1.0/24`), hijacking local traffic into the tunnel.
**Fix**: disable the subnet route in the VPN client (Tailscale → *Use Tailscale subnets* → uncheck your LAN), or disconnect the VPN.

## Telegram commands are ignored (but bot answers at boot)

**Cause**: chat ID mismatch — e.g. wrong value, or a parsing bug. The camera only accepts commands from the configured `chat_id`.
**Fix**: verify the chat ID with **@userinfobot** and re-enter it in `http://<ip>/setup`.

## Camera probe fails from the monitor ("no camera at this IP")

**Symptom**: manual add fails while the camera works in a browser.
**Cause**: probe timeout too short — the camera is busy streaming to another client.
**Fix**: close other streams and retry; the firmware ships with a 2 s timeout for manual add and 600 ms for network scans.

## Viewer screen is white after flashing

**Cause**: wrong PSRAM setting.
**Fix**: recompile with `PSRAM=opi` and the correct flash size (8M for N8R8, 16M for N16R8).

## Touch works but axes are inverted

**Fix**: adjust `cfg.offset_rotation` (0–7) in the LGFX touch config block of the monitor firmware.

## Brownouts / random restarts while streaming

**Cause**: power supply too weak. Streaming + WiFi peaks > 500 mA.
**Fix**: use a dedicated 5 V / 2 A supply and a short, good-quality cable.
