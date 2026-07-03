#!/usr/bin/env python3
"""
SmartVase ESP32-CAM Serial Console Helper
Author: Giacomo Radin
Description: Interactive script to send commands (e.g. capture, status, set)
             to the ESP32-CAM board without terminal echoing or buffer conflicts.
"""

import sys
import threading
import time

try:
    import serial
except ImportError:
    print("ERROR: 'pyserial' library is not installed.")
    print("Please run: C:\\Users\\anton\\.platformio\\penv\\Scripts\\pip.exe install pyserial")
    sys.exit(1)

PORT = "COM7"   # SELECT OWN COM
BAUD = 115200

def reader_thread(ser):
    """Continuously reads incoming telemetry and reports from the serial port."""
    while True:
        try:
            line = ser.readline()
            if line:
                decoded = line.decode('utf-8', errors='replace').rstrip('\r\n')
                # Overwrite the prompt line cleanly to display asynchronous board telemetry
                sys.stdout.write(f"\r\033[K{decoded}\nCAM> ")
                sys.stdout.flush()
        except serial.SerialException:
            print("\n[ERROR] Serial connection lost. Has the ESP32-CAM board been disconnected?")
            break
        except Exception:
            break

def main():
    try:
        ser = serial.Serial()
        ser.port = PORT
        ser.baudrate = BAUD
        ser.timeout = 0.1
        ser.dtr = False
        ser.rts = False
        ser.open()
    except Exception as e:
        print(f"[ERROR] Failed to open {PORT}: {e}")
        print("Please ensure any other active Serial Monitors or Miniterm instances are CLOSED!")
        sys.exit(1)


    print(f"--- Successfully connected to {PORT}! ---")
    print("Tip: type 'capture' to perform a bench test capture or 'help' to view CLI menu.")
    print("Type 'exit' or 'quit' to terminate this console.\n")

    # Launch background reader thread
    t = threading.Thread(target=reader_thread, args=(ser,), daemon=True)
    t.start()

    time.sleep(0.5)
    sys.stdout.write("CAM> ")
    sys.stdout.flush()

    while True:
        try:
            cmd = sys.stdin.readline()
            if not cmd:
                break
            cmd = cmd.strip()
            if cmd.lower() in ("exit", "quit"):
                print("Closing console...")
                break
            if cmd:
                # Send command with LF terminator
                ser.write(f"{cmd}\n".encode('utf-8'))
                ser.flush()
                print(f"[SENT] Command '{cmd}' sent to ESP32-CAM")
            sys.stdout.write("CAM> ")
            sys.stdout.flush()

        except KeyboardInterrupt:
            print("\nExiting...")
            break
        except Exception as e:
            print(f"\n[ERROR] Failed to send command: {e}")
            break

    ser.close()

if __name__ == "__main__":
    main()
