#!/usr/bin/env python3
"""Put a running detector (ESP32-C5) into USB download mode, then exit.

From C5VRX-5 (Twotoz and the C5VRX contributors). The RTS/DTR reset esptool
uses does not reset the C5's modem domain: on C5VRX images the PHY I/Q dump
and Wi-Fi ran on into the ROM loader, whose USB download loop then stopped
answering. The detector keeps C5VRX's route: this sends the console download
key '`', and the firmware stops the scan task's captures, releases the dump
bank, stops Wi-Fi, requests a download boot and restarts
(sdr_rx_reboot_to_download, components/sdr_rx/sdr_radio_c5.c), after printing
"SDR download_boot=requested", the acknowledgement waited for here. Flash
afterwards with esptool --before no-reset; tools/flash.py does both steps.

  python tools/enter_download.py COM33 && esptool --before no-reset ...

If the console is starved (heavy load, a hung image) the key is not read;
then use the BOOT button (hold BOOT, tap RESET).
"""
import sys
import time

import serial

ACK = b"download_boot=requested"


def enter_download(port, attempts=5, settle_s=1.5, log=print):
    for attempt in range(1, attempts + 1):
        s = serial.Serial()
        s.port, s.baudrate, s.timeout, s.write_timeout = port, 115200, 0.05, 0.5
        s.dtr = False
        s.rts = False
        try:
            s.open()
        except serial.SerialException as e:
            log(f"enter_download: open {port} failed ({e}); retrying")
            time.sleep(0.5)
            continue
        seen = b""
        try:
            s.reset_input_buffer()
            s.write(b"`")
            deadline = time.time() + 1.0
            while time.time() < deadline and ACK not in seen:
                seen = (seen + s.read(512))[-4096:]
        except serial.SerialTimeoutException:
            log(f"enter_download: attempt {attempt}: console not reading (write timeout)")
        except serial.SerialException:
            # The port vanishing mid-read means the restart already happened.
            seen += ACK
        finally:
            try:
                s.close()
            except serial.SerialException:
                pass
        if ACK in seen:
            log("enter_download: firmware acknowledged, waiting for download mode")
            time.sleep(settle_s)
            return True
        time.sleep(0.3)
    log("enter_download: no acknowledgement; if this board is already in the ROM "
        "loader that is fine, otherwise hold BOOT and tap RESET")
    return False


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(2)
    sys.exit(0 if enter_download(sys.argv[1]) else 1)
