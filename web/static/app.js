// Handle showing status/info/error messages in the blue banner at the top
function showBanner(type, message) {
  const el = document.getElementById("statusBanner");
  if (!el) return;

  if (!message) {
    el.className = "banner banner-hidden";
    el.textContent = "";
    return;
  }

  if (type === "error") {
    el.className = "banner banner-error";
  } else {
    el.className = "banner banner-info";
  }
  el.textContent = message;
}

// Track whether we're waiting for a filename for menu option 4
let restorePending = false;


// High-level commands (mapped in server.py -> MQTT -> Pico menu)
// identify, backup, restore (latest), quit, resume

async function sendCommand(action, topN) {
  try {
    const body = { action };
    if (topN != null) {
      body.topN = topN;
    }

    const res = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });

    if (!res.ok) {
      showBanner("error", `Server error (${res.status}) while sending '${action}'.`);
      return;
    }

    const data = await res.json();
    if (!data.ok) {
      showBanner("error", `Device rejected '${action}': ${data.error || "unknown error"}.`);
    } else {
      showBanner("info", `Command '${action}' sent successfully.`);
    }
  } catch (err) {
    console.error(err);
    showBanner("error", "Could not reach server. Is server.py running?");
  }
}

// Send raw text directly to the Pico via MQTT
async function sendRawToDevice(text) {
  try {
    const res = await fetch("/api/send", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ data: text }),
    });

    if (!res.ok) {
      showBanner("error", `Server error (${res.status}) while sending raw data.`);
      return;
    }

    const data = await res.json();
    if (!data.ok) {
      showBanner("error", `Raw send failed: ${data.error || "unknown error"}.`);
    } else {
      showBanner("info", "Line sent to device.");
    }
  } catch (err) {
    console.error(err);
    showBanner("error", "Could not reach server for raw send.");
  }
}

// Menu option 1: Run benchmark + CSV + identification
async function runBenchmarkWorkflow() {
  let topN = prompt(
    "[CSV MATCH] How many top matches to display? (1-10, Enter for 3):",
    "3"
  );

  if (topN === null) {
    // user cancelled
    return;
  }

  topN = parseInt(topN, 10);
  if (isNaN(topN)) topN = 3;
  if (topN < 1) topN = 1;
  if (topN > 10) topN = 10;

  await sendCommand("identify", topN);
}


// Menu option 4: Restore from specific .fimg image
function restoreFromSpecificImage() {
  // Enter option 4 in the Pico menu (no '\n'!)
  sendRawToDevice("4");
  restorePending = true;

  showBanner(
    "info",
    "Option 4 selected. When you see the [RESTORE] list, type the image path " +
      "(e.g. FLASHIMG/xxx.fimg) into 'Send raw line' and press Enter."
  );
}


// Poll logs from /api/logs and update the terminal + DB status pill
async function refreshLogs() {
  try {
    const res = await fetch("/api/logs");
    const data = await res.json();

    const term = document.getElementById("terminal");
    if (!term) return;

    term.textContent = data.lines.join("");

    const db = document.getElementById("dbStatus");
    if (db) {
      if (data.db_loading) {
        db.textContent = "Loading database from SD card…";
        db.className = "status-pill status-on";
      } else {
        db.textContent = "Idle";
        db.className = "status-pill status-off";
      }
    }
  } catch (err) {
    console.error(err);
    showBanner(
      "error",
      "Failed to refresh logs. Check server.py and MQTT bridge."
    );
  }
}


// Wire up buttons and raw input box
function setupHandlers() {
  // Left-side operation buttons
  const btnRun = document.getElementById("btnRun");
  if (btnRun) {
    btnRun.addEventListener("click", runBenchmarkWorkflow);
  }

  const btnBackup = document.getElementById("btnBackup");
  if (btnBackup) {
    btnBackup.addEventListener("click", () => {
      if (confirm("Backup SPI flash to SD? This may take a while.")) {
        sendCommand("backup"); // menu option 2
      }
    });
  }

  const btnRestoreLatest = document.getElementById("btnRestoreLatest");
  if (btnRestoreLatest) {
    btnRestoreLatest.addEventListener("click", () => {
      if (
        confirm(
          "Restore from latest .fimg in /FLASHIMG? This will overwrite the flash."
        )
      ) {
        sendCommand("restore"); // menu option 3 (latest .fimg)
      }
    });
  }

  const btnRestoreChoose = document.getElementById("btnRestoreChoose");
  if (btnRestoreChoose) {
    btnRestoreChoose.addEventListener("click", restoreFromSpecificImage);
  }

  const btnListImages = document.getElementById("btnListImages");
  if (btnListImages) {
    btnListImages.addEventListener("click", () => {
      // No dedicated server action; just send raw menu option 5 + newline.
      sendRawToDevice("5\n");
    });
  }

  const btnQuit = document.getElementById("btnQuit");
  if (btnQuit) {
    btnQuit.addEventListener("click", () => {
      sendCommand("quit");
    });
  }

  const btnResume = document.getElementById("btnResume");
  if (btnResume) {
    btnResume.addEventListener("click", () => sendCommand("resume"));
  }

  // Raw input text box at the bottom of the terminal
  const input = document.getElementById("terminalInput");
  if (input) {
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        let value = input.value.trim();
        if (!value) return;

        // If we are waiting for filename for option 4, treat this as filename
        if (restorePending) {
          restorePending = false;
          sendRawToDevice(value + "\n");
          input.value = "";
          return;
        }

        // If user types "1", mirror menu option 1 but go through the popup
        if (value === "1") {
          input.value = "";
          runBenchmarkWorkflow();
          return;
        }

        // If user types "4" manually, behave like clicking the button
        if (value === "4") {
          input.value = "";
          restoreFromSpecificImage();
          return;
        }

        sendRawToDevice(value + "\n");
        input.value = "";
      }
    });
  }

  // Start log polling
  refreshLogs();
  setInterval(refreshLogs, 1000);

  // On startup, ask Pico to show its main menu (like pressing "Return")
  setTimeout(() => {
    sendCommand("resume");
  }, 500);
}

// Initialise handlers when DOM is ready
document.addEventListener("DOMContentLoaded", setupHandlers);
