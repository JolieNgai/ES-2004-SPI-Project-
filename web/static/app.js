// Handle showing status/info/error messages in the blue banner at the top
function showBanner(type, message) {
  const el = document.getElementById("statusBanner");
  if (!el) return;
  
  // Hide banner if message is empty
  if (!message) {
    el.className = "banner banner-hidden";
    el.textContent = "";
    return;
  }
  
  // Select banner style (info vs error)
  if (type === "error") {
    el.className = "banner banner-error";
  } else {
    el.className = "banner banner-info";
  }
  el.textContent = message;
}

// let the user type the filename in the raw input
let restorePending = false;

// Send high-level commands (identify/backup/restore/quit/resume/etc.)
async function sendCommand(action, topN, filename) {
  try {
    const body = { action };
    if (topN != null) {
      body.topN = topN;
    }
    if (filename != null) {
      body.filename = filename;
    }

    const res = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body),
    });

    if (!res.ok) {
      showBanner(
        "error",
        `Server error (${res.status}) while sending '${action}'.`
      );
      return;
    }

    const data = await res.json();
    if (!data.ok) {
      showBanner(
        "error",
        `Device rejected '${action}': ${data.error || "unknown error"}.`
      );
    } else {
      showBanner("info", `Command '${action}' sent successfully.`);
    }
  } catch (err) {
    console.error(err);
    showBanner("error", "Could not reach server. Is server.py running?");
  }
}

// Send raw text to the device (for the input box)
async function sendRawToDevice(text) {
  try {
    const res = await fetch("/api/send", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify({ data: text }),
    });

    if (!res.ok) {
      showBanner(
        "error",
        `Server error (${res.status}) while sending raw data.`
      );
      return;
    }

    const data = await res.json();
    if (!data.ok) {
      showBanner(
        "error",
        `Raw send failed: ${data.error || "unknown error"}.`
      );
    } else {
      showBanner("info", "Line sent to device.");
    }
  } catch (err) {
    console.error(err);
    showBanner("error", "Could not reach server for raw send.");
  }
}

// When user clicks "Run benchmark workflow" (menu option 1)
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


// Restore from specific image (.fimg) – menu option 4
function restoreFromSpecificImage() {
  restorePending = true;
  showBanner(
    "info",
    "Option 4 armed. Now type the filename (e.g. FLASHIMG/xxx.fimg) " +
      "in the 'Send raw line' box below and press Enter."
  );
}

// Poll logs and update terminal
async function refreshLogs() {
  try {
    const res = await fetch("/api/logs");
    const data = await res.json();

    const term = document.getElementById("terminal");
    if (!term) return;

    const isAtBottom =
      term.scrollTop + term.clientHeight >= term.scrollHeight - 5;

    term.textContent = data.lines.join("");
    // Check whether user is currently scrolled to bottom
    if (isAtBottom) {
      term.scrollTop = term.scrollHeight;
    }

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

// Wire up all buttons and the raw-input text box once DOM is ready
function setupHandlers() {
  const btnRun = document.getElementById("btnRun");
  if (btnRun) {
    btnRun.addEventListener("click", runBenchmarkWorkflow);
  }

  const btnBackup = document.getElementById("btnBackup");
  if (btnBackup) {
    btnBackup.addEventListener("click", () => {
      if (confirm("Backup SPI flash to SD? This may take a while.")) {
        sendCommand("backup");
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
        sendCommand("restore_latest"); // maps to menu option 3
      }
    });
  }

// Menu option 4 – will send "4\n" and instruct the user to type filename
  const btnRestoreChoose = document.getElementById("btnRestoreChoose");
  if (btnRestoreChoose) {
    btnRestoreChoose.addEventListener("click", restoreFromSpecificImage); 
  }

  const btnListImages = document.getElementById("btnListImages");
  if (btnListImages) {
    btnListImages.addEventListener("click", () => {
      // Menu option 5
      sendCommand("list_images");
    });
  }

  const btnQuit = document.getElementById("btnQuit");
  if (btnQuit) {
    btnQuit.addEventListener("click", () => {
      // Idel loop
      sendCommand("quit");
    });
  }

  const btnResume = document.getElementById("btnResume");
  if (btnResume) {
    btnResume.addEventListener("click", () => sendCommand("resume"));
  }

const input = document.getElementById("terminalInput");
if (input) {
  input.addEventListener("keydown", (e) => {
    if (e.key === "Enter") {
      e.preventDefault();
      let value = input.value.trim();
      if (!value) return;

      // If user types "1" here, treat it as "Run benchmark workflow"
      if (value === "1") {
        input.value = "";
        // This will show the popup and then call /api/command identify,
        runBenchmarkWorkflow();
        return;
      }

      // All other inputs are sent raw to the device
      sendRawToDevice(value + "\n");
      input.value = "";
    }
  });
}



  refreshLogs();
  setInterval(refreshLogs, 1000);
}
// Initialise handlers when the page has finished loading
document.addEventListener("DOMContentLoaded", setupHandlers);
