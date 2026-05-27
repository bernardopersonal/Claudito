#!/usr/bin/env python3
"""Play all 6 Clawdmeter sounds sequentially via serial for video demo."""
import serial
import time
import sys

PORT = sys.argv[1] if len(sys.argv) > 1 else "/dev/cu.usbmodem3101"
BAUD = 115200

SOUNDS = [
    ("snd0", "SND_STOP",               2.97),
    ("snd1", "SND_STOP_FAILURE",        1.48),
    ("snd2", "SND_PERMISSION_REQUEST",  2.02),
    ("snd3", "SND_PERMISSION_PROMPT",   2.97),
    ("snd4", "SND_IDLE_PROMPT",         1.48),
    ("snd5", "SND_TASK_COMPLETED",      1.48),
]

PAUSE = 2.0  # seconds between sounds

def banner(text):
    w = len(text) + 4
    print()
    print("=" * w)
    print(f"  {text}")
    print("=" * w)

def main():
    print(f"Opening {PORT} @ {BAUD}...")
    ser = serial.Serial(PORT, BAUD, timeout=1)
    time.sleep(0.5)  # let serial settle

    banner("CLAWDMETER SOUND TEST")
    print(f"  {len(SOUNDS)} sounds, ~{sum(d for _,_,d in SOUNDS) + PAUSE*(len(SOUNDS)-1):.0f}s total")
    print()

    input("Press ENTER to start (hit record first!)...")
    print()

    for i, (cmd, name, duration) in enumerate(SOUNDS):
        banner(f"▶  {i+1}/{len(SOUNDS)}  {name}  ({cmd})")
        ser.write(f"{cmd}\n".encode())
        ser.flush()
        time.sleep(duration + PAUSE)

    banner("✅  ALL SOUNDS DONE")
    ser.close()

if __name__ == "__main__":
    main()
