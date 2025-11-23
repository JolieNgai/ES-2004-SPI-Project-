import os
import threading

from flask import Flask, request, jsonify, render_template
import paho.mqtt.client as mqtt

# ---------- MQTT config ----------
MQTT_HOST = "test.mosquitto.org"   # or "localhost" if you run your own broker
MQTT_PORT = 1883
LOG_TOPIC = "pico/log"
CMD_TOPIC = "pico/cmd"

# ---------- Flask app ----------
BASE_DIR = os.path.dirname(os.path.abspath(__file__))

app = Flask(
    __name__,
    static_folder=os.path.join(BASE_DIR, "static"),
    template_folder=BASE_DIR,
)

LOG_BUFFER = []
LOG_MAX = 500
db_loading = False

mqtt_client = mqtt.Client()


def on_connect(client, userdata, flags, rc):
    print("Web MQTT connected:", rc)
    client.subscribe(LOG_TOPIC)


def on_message(client, userdata, msg):
    global db_loading
    line = msg.payload.decode(errors="ignore")

    LOG_BUFFER.append(line)
    if len(LOG_BUFFER) > LOG_MAX:
        del LOG_BUFFER[0:len(LOG_BUFFER) - LOG_MAX]

    if "--- Loading database from SD card ---" in line:
        db_loading = True
    if "Total entries loaded into local memory" in line or "Integration complete." in line:
        db_loading = False


mqtt_client.on_connect = on_connect
mqtt_client.on_message = on_message
mqtt_client.connect(MQTT_HOST, MQTT_PORT, 60)

threading.Thread(target=mqtt_client.loop_forever, daemon=True).start()


# ---------- HTTP endpoints ----------

@app.get("/")
def index():
    return render_template("index.html")


@app.get("/api/logs")
def api_logs():
    return jsonify({
        "lines": LOG_BUFFER,
        "db_loading": db_loading,
    })


@app.post("/api/command")
def api_command():
    """
    High-level commands mapped to Pico main.c menu:

      1 = Run benchmark + CSV + identification
      2 = Backup SPI flash to SD  (/FLASHIMG/*.fimg)
      3 = Restore SPI flash from SD (latest .fimg)
      4 = Restore SPI flash from SD (choose specific file)
      5 = List available flash images (.fimg)
      q = Quit (idle loop), m = return to menu in idle mode
    """
    data = request.get_json(force=True)
    action = data.get("action")
    topN = data.get("topN")

    if action == "identify":
        # Option 1: Run benchmark + CSV + identification
        # We send '1' to select menu option, then the user-specified
        # topN as a line (e.g., "3\n") to answer the prompt in main.c.
        #
        # To make this robust, we send "1<topN>\\n" so the firmware
        # sees '1' as the menu choice and "<topN>\\n" as the next line.
        if topN is not None:
            try:
                topN = int(topN)
            except (TypeError, ValueError):
                topN = 3
            if topN < 1:
                topN = 1
            if topN > 10:
                topN = 10
            payload = f"1{topN}\n"
        else:
            payload = "1"        
        
    elif action == "backup":
        payload = "2"

    elif action == "restore" or action == "restore_latest":
        # Menu option 3: restore from latest .fimg in /FLASHIMG
        payload = "3"

    elif action == "restore_choose":
        # Menu option 4: restore from a specific .fimg filename
        payload = "4"

    elif action == "list_images":
        # Menu option 5: list available .fimg images on SD
        payload = "5"

    elif action == "quit":
        # q = Quit (idle loop)
        payload = "q"

    elif action == "resume":
        # 'm' in idle loop returns to main menu
        payload = "m"

    else:
        return jsonify({"ok": False, "error": "unknown action"}), 400


    mqtt_client.publish(CMD_TOPIC, payload)
    return jsonify({"ok": True})


@app.post("/api/send")
def api_send():
    data = request.get_json(force=True)
    payload = data.get("data", "")
    if not isinstance(payload, str) or not payload:
        return jsonify({"ok": False, "error": "empty payload"}), 400

    mqtt_client.publish(CMD_TOPIC, payload)
    return jsonify({"ok": True})


if __name__ == "__main__":
    app.run(host="0.0.0.0", port=5000, debug=True)
