import sys, time
import serial

PORT = "COM4"
BAUD = 115200

# Toggle DTR/RTS to try to trigger a reset, then read boot log
with serial.Serial(PORT, BAUD, timeout=0.3) as s:
    s.setDTR(False); s.setRTS(True); time.sleep(0.1)
    s.setRTS(False); time.sleep(0.1)
    s.reset_input_buffer()
    t0 = time.time()
    while time.time() - t0 < 4.0:
        line = s.readline()
        if line:
            sys.stdout.write(line.decode(errors="replace"))
