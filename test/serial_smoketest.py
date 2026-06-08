import sys, time
import serial

PORT = "COM4"
BAUD = 115200

cmds = ["ping", "working", "done", "all_done", "error", "idle"]

with serial.Serial(PORT, BAUD, timeout=0.3) as s:
    time.sleep(0.5)
    s.reset_input_buffer()
    # drain any boot log
    t0 = time.time()
    while time.time() - t0 < 1.0:
        line = s.readline()
        if line:
            sys.stdout.write("BOOT| " + line.decode(errors="replace"))
    for c in cmds:
        s.write((c + "\n").encode())
        s.flush()
        time.sleep(0.4)
        # read responses
        t0 = time.time()
        while time.time() - t0 < 0.6:
            line = s.readline()
            if line:
                sys.stdout.write("DEV | " + line.decode(errors="replace"))
        print(f"--- sent: {c} ---")
