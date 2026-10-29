# Impact Lab

A browser dashboard for the four-zone ESP32 DevKit V1 impact detector on GPIO32–35. It reads the existing firmware's newline-delimited JSON at 115200 baud. No firmware update is needed.

## Use

- **Try demo** starts an explicitly simulated session with rotating zone impacts. It never opens a serial port.
- **Import serial log** reviews existing JSON lines from a pasted log or local `.txt` / `.jsonl` file. This is a recorded session, not live data.
- **Connect ESP32** uses the browser's serial-port picker. Open the dashboard directly in desktop Chrome or Edge on HTTPS or localhost, and close Arduino Serial Monitor first. Choose the board's USB serial port. Connect is never automatic.
- **Disconnect** releases the serial reader and port. CSV export remains available.
- **Pause chart** freezes the chart only; events and readings continue to be collected.
- **Clear session** clears dashboard counts/history without resetting the device.
- **Export CSV** exports up to 1,000 retained events. The table displays the latest 100.

The website sends no serial commands and does not flash firmware. A serial-port open can still have hardware-specific effects; direct USB behavior has not been tested in this session. The unresolved input protection issue remains separate from this dashboard.

## What the screen means

The four tiles represent the assumed numbering: 1 top left, 2 top right, 3 bottom left, 4 bottom right. They are not a measured position map. Confident zone hits flash the corresponding tile. Ambiguous hits are labeled uncertain without flashing a definite location.

Sensor bars and the chart show raw 0–4095 ADC readings. Trigger deltas are relative to each baseline, matching the firmware. The chart displays the last 60 reported scans, not a high-speed waveform. Firmware status messages normally arrive once per second.

The detector correctly distinguishes version 1.2's `calibration_ok: false` / `detection_enabled: true` from failed automatic calibration. High idle noise does not falsely turn off its display of large-spike mode. Three active sensors leave the fourth sensor blank. Session hit counts include only messages captured by the dashboard, not the device's preexisting cumulative count.

Stale USB data is shown as waiting/no data after five seconds. Imported logs are always labeled recorded. Demo and live data are never mixed.

## Privacy and storage

Serial data, imported files, and events remain in browser memory. Reloading clears them. Export CSV to retain a session. No telemetry endpoint or backend data store is used. Fonts load from Google Fonts, with local fallback fonts when offline. Publication is private by default.

## Local use

The sibling `Impact Lab.html` is a single-file edition that opens directly for demo and log import. Browser security policies may require HTTPS or localhost for USB access.

To serve the source version locally from this directory:

```sh
python3 -m http.server 8765 --bind 127.0.0.1 --directory dist
```

Then open `http://127.0.0.1:8765` in Chrome. A local preview server could not be started by the assistant because this session forbids binding a local port.

## Verification

JavaScript syntax checks and nine automated data-model tests passed: version 1.2 readiness with high noise, legacy failure, three-sensor handling, fragmented serial messages, malformed/oversized line recovery, pasted HTML spaces, ambiguous events and CSV, bounded histories, and device reboots. Browser visual QA and physical USB behavior remain unverified in this environment.

Run checks with:

```sh
node --check dist/app.js
node --check dist/model.js
node --test tests/model.test.mjs
```

Reference: [Chrome Web Serial documentation](https://developer.chrome.com/docs/capabilities/serial).
