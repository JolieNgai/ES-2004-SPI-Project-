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

// Send high-level commands (identify/backup/restore/quit/resume/etc.)
async function sendCommand(action, topN) {
  try {
    const body = { action };
    if (topN != null) {
      body.topN = topN;
    }

    const res = await fetch("/api/command", {
      method: "POST",
      headers: { "Content-Type": "application/json" },
      body: JSON.stringify(body)
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
      body: JSON.stringify({ data: text })
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

async function restoreFromSpecificImage() {
  const filename = prompt(
    "Enter image path or filename inside FLASHIMG.\n\n" +
    "Examples:\n" +
    "  FLASHIMG/t0000001234_ef4018.fimg\n" +
    "  t0000001234_ef4018.fimg"
  );

  if (filename === null) {
    // cancelled
    return;
  }

  const trimmed = filename.trim();
  if (!trimmed) {
    showBanner("error", "No filename entered; restore cancelled.");
    return;
  }

  // First send menu option '4' so main.c enters the restore-by-name flow.
  await sendCommand("restore_choose");

  // Give Pico a brief moment to print the list and prompt, then send filename line.
  setTimeout(() => {
    sendRawToDevice(trimmed + "\n");
  }, 400);

  showBanner(
    "info",
    "Restore request sent. Watch the terminal for [RESTORE] messages."
  );
}


// Restore from specific image (.fimg) – menu option 4
async function restoreFromSpecificImage() {
  const filename = prompt(
    "Enter image path or filename inside FLASHIMG.\n\n" +
    "Examples:\n" +
    "  FLASHIMG/t0000001234_ef4018.fimg\n" +
    "  t0000001234_ef4018.fimg"
  );

  if (filename === null) {
    // cancelled
    return;
  }

  const trimmed = filename.trim();
  if (!trimmed) {
    showBanner("error", "No filename entered; restore cancelled.");
    return;
  }

  // First send menu option '4' so main.c enters the restore-by-name flow.
  await sendCommand("restore_choose");

  // Give Pico a brief moment to print the list and prompt, then send filename line.
  setTimeout(() => {
    sendRawToDevice(trimmed + "\n");
  }, 400);

  showBanner(
    "info",
    "Restore request sent. Watch the terminal for [RESTORE] messages."
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
      if (confirm("Restore from latest .fimg in /FLASHIMG? This will overwrite the flash.")) {
        sendCommand("restore_latest");   // → menu option 3
      }
    });
  }

  const btnRestoreChoose = document.getElementById("btnRestoreChoose");
  if (btnRestoreChoose) {
    btnRestoreChoose.addEventListener("click", restoreFromSpecificImage); // → menu option 4
  }

  const btnListImages = document.getElementById("btnListImages");
  if (btnListImages) {
    btnListImages.addEventListener("click", () => {
      sendCommand("list_images");        // → menu option 5
    });
  }


  const btnQuit = document.getElementById("btnQuit");


  const btnResume = document.getElementById("btnResume");
  if (btnResume) {
    btnResume.addEventListener("click", () => sendCommand("resume"));
  }

  const input = document.getElementById("terminalInput");
  if (input) {
    input.addEventListener("keydown", (e) => {
      if (e.key === "Enter") {
        e.preventDefault();
        const value = input.value;
        if (!value) return;
        sendRawToDevice(value + "\n");
        input.value = "";
      }
    });
  }

  refreshLogs();
  setInterval(refreshLogs, 1000);
}

document.addEventListener("DOMContentLoaded", setupHandlers);
