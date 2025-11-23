# SPI Flash Forensic Tool

## 1. Project Overview

This project implements an **SPI Flash Performance Evaluation & Forensic Analysis** tool using a **Raspberry Pi Pico** and a **web-based control panel**.

The Pico firmware:
- Talks to an external **SPI flash chip** and an **SD card**.
- Benchmarks **erase / program / read** operations.
- Saves and restores complete flash images (`.fimg`) on the SD card.
- Loads a CSV timing database from SD and performs **forensic identification** by comparing measured timings and JEDEC ID to known chips.

The PC-side web server:
- Shows the Pico’s serial output in a browser.
- Lets the user trigger benchmark, backup, restore, and listing operations from a simple GUI.


---

## 2. File & Folder Summary

### Root

- **`main.c`**  
  Pico firmware source.
  - Configures SPI0 (external flash) and SPI1 (SD card).
  - Provides low-level flash operations (JEDEC ID, read, page program, sector erase, CRC32).
  - Implements FIMG backup/restore:
    - Backs up current flash contents to `/FLASHIMG/tXXXXXXXXXX_<JEDEC>.fimg` on SD.
    - Restores from latest or user-chosen `.fimg`, including CRC integrity checks.
  - Loads `Embedded_datasheet.csv` from SD into RAM and benchmarks the attached flash.
  - Computes score differences vs database entries and prints **Top-N matches** and the **most likely chip**.
  - Exposes a text-based **main menu** over USB serial:

    ```text
    1 = Run benchmark + CSV + identification
    2 = Backup SPI flash to SD (/FLASHIMG/*.fimg)
    3 = Restore SPI flash from SD (latest .fimg)
    4 = Restore SPI flash from SD (choose specific file)
    5 = List available flash images (.fimg)
    q = Quit (idle loop), m = Return to main menu
    ```

- **`CMakeLists.txt`**  
  CMake build script for the Pico SDK. Defines the executable target, adds `main.c`, and links to the SD-card / FatFs libraries provided by the SDK.

- **`README.md`**  
  This documentation file.

### Web server (PC side)

Located in the same project folder:

- **`server.py`**  
  Flask + MQTT web server.
  - Connects to an MQTT broker (`pico/log` for logs, `pico/cmd` for commands).
  - Buffers recent log lines from the Pico and exposes them via:
    - `GET /api/logs` – returns log buffer + “database loading” flag.
  - Accepts high-level commands from the front-end:
    - `POST /api/command` with `action` such as:
      - `identify` → send `1<topN>\n` (benchmark + CSV match).
      - `backup` → send `2` (backup to SD).
      - `restore` / `restore_latest` → send `3` (restore latest `.fimg`).
      - `quit` → send `q` (idle).
      - `resume` → send `r` (return to main menu).
    - `POST /api/send` – raw passthrough string to serial.
  - Serves the main web page (`index.html`) and static files from `static/`.

- **`BridgeToPico.py`**  
  Serial - MQTT bridge.
  - Opens the Pico’s USB serial port (e.g., `COM8`, 115200 baud).
  - Reads lines from serial and publishes them to MQTT topic `pico/log`.
  - Subscribes to `pico/cmd` and writes any received payload directly to the Pico.
  - Automatically re-opens the serial port if the Pico is unplugged/replugged.

- **`index.html`**  
  HTML template for the “SPI Flash Forensic Tool” web interface.
  - Left panel: operation cards with buttons for:
    - Run benchmark workflow (option 1)
    - Backup to SD (option 2)
    - Restore latest `.fimg` (option 3)
    - Restore from specific image (option 4)
    - List FLASHIMG images (option 5)
    - Quit / Return to main menu
  - Right panel: database status pill, scrollable terminal window, and “Send raw line” text box.

- **`static/app.js`**  
  Front-end JavaScript.
  - Polls `/api/logs` every second and updates:
    - The terminal output area.
    - The “Database: Idle / Loading” status pill.
  - Sends **high-level commands** to `/api/command` when buttons are clicked:
    - Shows a popup for option 1 to ask for number of top matches, then calls `identify`.
    - Calls `backup`, `restore`, `quit`, `resume` as appropriate.
  - Sends raw text from the “Send raw line” input to `/api/send` so the user can still interact with the firmware menu directly.
  - Special handling:
    - Typing `1` in the input triggers the same option-1 popup as the button.
    - Clicking “Restore from specific image” sends menu `4` and sets a flag so the next text entered is treated as the filename for restore.

- **`static/style.css`**  
  CSS styles for the web UI:
  - Dark theme, layout of panels, buttons, terminal, and status banner.
  - Responsive layout suitable for desktop browsers.

- **`.venv/`**  
   Python virtual environment containing Flask, paho-mqtt, pyserial, etc.

> **SD card files expected by the firmware**
>
> - `Embedded_datasheet.csv` – CSV database of reference chips and timings.  
> - `/FLASHIMG/` – folder where the `.fimg` backup images are stored.

---

## 3. How to Compile, Run, and Test

### 3.1 Hardware Setup

- Raspberry Pi Pico (e.g. on Maker Pi Pico board).
- External SPI flash wired to **SPI0**:

  | Pico Pin | Function | External Flash |
  |----------|----------|----------------|
  | GP2      | SCK      | CLK            |
  | GP3      | MOSI     | DI             |
  | GP4      | MISO     | DO             |
  | GP5      | CS       | CS             |

- SD card slot on Maker Pi Pico using **SPI1**:

  | Pico Pin | Function | SD Card |
  |----------|----------|---------|
  | GP10     | SCK      | SCK     |
  | GP11     | MOSI     | MOSI    |
  | GP12     | MISO     | MISO    |
  | GP13     | CS       | CS      |

- SD card contents:
  - `Embedded_datasheet.csv`
  - `FLASHIMG/` folder (created automatically if missing).

### 3.2 Build and Flash the Pico Firmware

1. Ensure the Pico SDK is installed and `PICO_SDK_PATH` is set.
2. From the project root:

   ```bash
   mkdir -p build
   cd build
   cmake ..          # configure for Pico
   cmake --build .   # or: ninja

### 3.3 Run the web server
1. In the terminal
  ```powershell
    .\.venv\Scripts\activate
    python BridgeToPico.py

2. In another terminal
  ```powershell
    python server.py

