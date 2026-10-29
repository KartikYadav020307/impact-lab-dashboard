# Impact Lab

A real-time browser dashboard and firmware for a four-zone ESP32 piezoelectric impact detection system.

## Project Structure

- `firmware/piezo_zones/`: Arduino / ESP32 firmware (`piezo_zones.ino`) reading 4-zone piezoelectric sensor inputs and streaming newline-delimited JSON over USB serial (115200 baud).
- `source/`: Modular web application source code (`dist/` containing `index.html`, `app.js`, `model.js`, `styles.css`) and automated unit tests (`tests/model.test.mjs`).
- `Impact Lab.html`: Standalone single-file bundle that can be opened directly in modern browsers for demo mode and serial log import.

## Features

- **Live USB Serial Connection**: Connect directly to your ESP32 board via Web Serial API in desktop Chrome or Edge.
- **Simulated Demo Mode**: Try out the dashboard with simulated four-zone impact events without hardware.
- **Serial Log Import**: Import captured `.jsonl` / `.txt` serial log sessions for post-run analysis.
- **Real-Time Visualizations**:
  - Four-zone impact flash indicators with confidence/ambiguity classification.
  - Live ADC raw readings and baseline-relative delta bars.
  - Rolling 60-scan signal chart with pause/resume functionality.
  - Impact event log with CSV export support (up to 1,000 retained events).

## Running Locally

To serve the web dashboard locally from the `source` directory:

```bash
cd source
python -m http.server 8765 --bind 127.0.0.1 --directory dist
```

Then navigate to `http://127.0.0.1:8765` in Google Chrome or Microsoft Edge.

## Testing

Run unit tests for data parsing and state transitions:

```bash
cd source
node --test tests/model.test.mjs
```
