#!/usr/bin/env python3
import getpass
import hashlib
import json
import os
import secrets
from pathlib import Path

PASSWORD_PATH = Path('/opt/noxus-master/gate_password.json')
ITERATIONS = 350_000


def main():
    first = getpass.getpass('New gate password: ')
    second = getpass.getpass('Confirm gate password: ')
    if first != second:
        raise SystemExit('Passwords do not match.')
    if len(first) < 16:
        raise SystemExit('Use at least 16 characters.')

    salt = secrets.token_bytes(32)
    verifier = hashlib.pbkdf2_hmac('sha256', first.encode('utf-8'), salt,
                                   ITERATIONS, dklen=32)
    temporary = PASSWORD_PATH.with_suffix('.json.tmp')
    temporary.write_text(json.dumps({
        'version': 1,
        'algorithm': 'pbkdf2-hmac-sha256',
        'iterations': ITERATIONS,
        'salt': salt.hex(),
        'verifier': verifier.hex(),
    }, indent=2) + '\n')
    os.chmod(temporary, 0o600)
    temporary.replace(PASSWORD_PATH)
    print('Gate password updated. Restart noxus-master.service to load it.')


if __name__ == '__main__':
    main()
