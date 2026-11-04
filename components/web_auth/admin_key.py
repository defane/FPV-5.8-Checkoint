#!/usr/bin/env python3
"""Derive the C3/C6 admin key (components/web_auth) from the password in sdkconfig.json.

K = PBKDF2-HMAC-SHA256(password, salt, iterations), 32 bytes, printed in hex; nothing
for an empty password (admin auth disabled). The password is read from the build's
sdkconfig.json, so it never appears on a command line.

usage: admin_key.py <sdkconfig.json> <CONFIG name without CONFIG_> <salt> <iterations>
"""
import hashlib
import json
import sys


def main() -> int:
    config_json, name, salt, iterations = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4])
    with open(config_json, encoding="utf-8") as f:
        password = json.load(f)[name]
    if password:
        key = hashlib.pbkdf2_hmac("sha256", password.encode("utf-8"), salt.encode("utf-8"), iterations, 32)
        print(key.hex())
    return 0


if __name__ == "__main__":
    sys.exit(main())
