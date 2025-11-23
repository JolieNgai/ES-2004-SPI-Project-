# BridgeToPico.py / bridge_pico_mqtt.py

import threading
import time
import serial
from serial import SerialException
import paho.mqtt.client as mqtt

# ==================== CONFIG ====================

MQTT_HOST = "test.mosquitto.org"
MQTT_PORT = 1883
CMD_TOPIC = "pico/cmd"
LOG_TOPIC = "pico/log"

SERIAL_PORT = "COM8"         
BAUDRATE = 115200


# ================= SERIAL HANDLING =================

def open_serial():
    """Try to open the serial port in a loop until it succeeds."""
    while True:
        try:
            s = serial.Serial(SERIAL_PORT, BAUDRATE, timeout=0.1)
            print(f"[Serial] Opened {SERIAL_PORT} @ {BAUDRATE}")
            return s
        except SerialException as e:
            print(f"[Serial open error] {e}. Retrying in 2s...")
            time.sleep(2)


# global serial object (will be reopened if it breaks)
ser = open_serial()


# ================= MQTT CALLBACKS =================

def on_connect(client, userdata, flags, reason_code, properties):
    # Callback API version 2 signature
    print(f"MQTT connected with result {reason_code}")
    client.subscribe(CMD_TOPIC)


def on_message(client, userdata, msg):
    global ser
    cmd = msg.payload.decode("utf-8", errors="ignore")
    print(f"[MQTT] CMD from topic {msg.topic}: {cmd!r}")
    try:
        ser.write(cmd.encode("utf-8"))
        ser.flush()
    except SerialException as e:
        print(f"[Serial write error] {e}")


def serial_reader(client):
    """Read Pico stdout and publish to MQTT."""
    global ser

    while True:
        try:
            line = ser.readline()
        except SerialException as e:
            print(f"[Serial read error] {e}. Re-opening port...")
            # try to close and reopen
            try:
                ser.close()
            except Exception:
                pass
            time.sleep(1)
            ser = open_serial()
            continue  # restart loop with new port

        if not line:
            time.sleep(0.01)
            continue

        try:
            text = line.decode("utf-8", errors="ignore")
        except Exception:
            text = repr(line)

        print(text, end="")
        client.publish(LOG_TOPIC, text)


# ================= MAIN =================
# Use Callback API version 2 to remove the deprecation warning
client = mqtt.Client(mqtt.CallbackAPIVersion.VERSION2)
client.on_connect = on_connect
client.on_message = on_message

client.connect(MQTT_HOST, MQTT_PORT, 60)

t = threading.Thread(target=serial_reader, args=(client,), daemon=True)
t.start()

try:
    client.loop_forever()
except KeyboardInterrupt:
    print("\n[Main] KeyboardInterrupt – exiting...")
    try:
        ser.close()
    except Exception:
        pass
