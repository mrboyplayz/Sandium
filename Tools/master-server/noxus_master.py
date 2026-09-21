#!/usr/bin/env python3
import http.server
import socket
import socketserver
import struct
import threading
import time
import binascii
import json
import hashlib
import hmac
import secrets
import urllib.parse
from collections import defaultdict, deque
from pathlib import Path

MASTER_HOST = '185.227.111.150'
MASTER_PORT = 28015
GAME_HOST = '185.227.111.150'
GAME_PORT = 27584
SERVER_NAME = 'Sandium Server'
PHONES_PATH = Path('/opt/noxus-master/phones.json')
GATE_PASSWORD_PATH = Path('/opt/noxus-master/gate_password.json')
GATE_SESSION_SECONDS = 12 * 60 * 60
GATE_CHALLENGE_SECONDS = 60
GATE_MAX_FAILURES_PER_MINUTE = 5
phones_by_key = {}

gate_password = {}
gate_challenges = {}
gate_authorized = {}
gate_failures = defaultdict(deque)

def now():
    return time.strftime('%Y-%m-%d %H:%M:%S')

def load_gate_password():
    global gate_password
    try:
        data = json.loads(GATE_PASSWORD_PATH.read_text())
        verifier = bytes.fromhex(data['verifier'])
        salt = bytes.fromhex(data['salt'])
        iterations = int(data['iterations'])
        if len(verifier) != 32 or len(salt) < 16 or iterations < 100000:
            raise ValueError('unsafe gate password parameters')
        gate_password = {
            'verifier': verifier,
            'salt': salt,
            'iterations': iterations,
        }
    except Exception as exc:
        gate_password = {}
        print(f'{now()} GATE disabled: {exc}', flush=True)

def gate_is_authorized(ip):
    expiry = gate_authorized.get(ip, 0)
    if expiry <= time.time():
        gate_authorized.pop(ip, None)
        return False
    return True

def gate_rate_limited(ip):
    cutoff = time.time() - 60
    failures = gate_failures[ip]
    while failures and failures[0] < cutoff:
        failures.popleft()
    return len(failures) >= GATE_MAX_FAILURES_PER_MINUTE

def gate_new_challenge(ip):
    if not gate_password or gate_rate_limited(ip):
        return None
    challenge_id = secrets.token_hex(16)
    nonce = secrets.token_bytes(32)
    gate_challenges[challenge_id] = (ip, nonce, time.time() + GATE_CHALLENGE_SECONDS)
    return {
        'id': challenge_id,
        'nonce': nonce.hex(),
        'salt': gate_password['salt'].hex(),
        'iterations': gate_password['iterations'],
    }

def gate_verify(ip, challenge_id, proof_hex):
    challenge = gate_challenges.pop(challenge_id, None)
    if not challenge or gate_rate_limited(ip):
        return False
    challenge_ip, nonce, expiry = challenge
    if challenge_ip != ip or expiry < time.time():
        gate_failures[ip].append(time.time())
        return False
    expected = hmac.new(gate_password['verifier'], nonce, hashlib.sha256).hexdigest()
    if not hmac.compare_digest(expected, str(proof_hex).lower()):
        gate_failures[ip].append(time.time())
        return False
    gate_failures.pop(ip, None)
    gate_authorized[ip] = time.time() + GATE_SESSION_SECONDS
    return True

def load_phones():
    global phones_by_key
    try:
        phones_by_key = json.loads(PHONES_PATH.read_text())
    except Exception:
        phones_by_key = {}

def save_phones():
    PHONES_PATH.write_text(json.dumps(phones_by_key, indent=2, sort_keys=True))

def phone_display(index):
    prefix = 256 + (index - 1) // 10000
    suffix = (index - 1) % 10000 + 1
    return f'{prefix:03d}-{suffix:04d}'

def assign_phone(key):
    if key in phones_by_key:
        return phones_by_key[key]
    number = phone_display(len(phones_by_key) + 1)
    phones_by_key[key] = number
    save_phones()
    return number

def encode_host(ip):
    return bytes(reversed([int(x) for x in ip.split('.')]))

def packet(ip, port):
    return b'7DFP' + bytes([0x40]) + encode_host(ip) + struct.pack('<H', port)

GAME_REPLY = packet(GAME_HOST, GAME_PORT)
load_phones()
load_gate_password()

# --- CS auth (client state machine sub_14006BED0) ---
# 'H' (0x48) Steam auth: client sends 7DFP 0x48 + 32B id + u32 len + len B echo.
#   reply: 7DFP 0x48 + u32 token + u32 accept(1) + u32 phone (all LE, native read)
# 'I' (0x49) CS auth: client sends 7DFP 0x49 + u32 token + u32 accept-flag.
#   reply: 7DFP 0x49 + u32 1 + u32 phone  -> client shows "%03d-%04d" of phone
# phone int = prefix*10000 + suffix (2560001 -> "256-0001")
udp_sessions = {}   # token -> phone int (for the 'I' stage)
# Per-player session ids: the server keys accounts by the (token, session)
# pair in the join packet. Clients never run H auth (token=-1 for all), so
# the K reply's session must be unique per account or every player shares
# one server account (bodies hijack each other).
import zlib
SESSIONS_PATH = Path('/opt/noxus-master/sessions.json')
sessions_by_key = {}
ip_to_session = {}

def load_sessions():
    global sessions_by_key
    try:
        sessions_by_key = json.loads(SESSIONS_PATH.read_text())
    except Exception:
        sessions_by_key = {}

def save_sessions():
    SESSIONS_PATH.write_text(json.dumps(sessions_by_key, indent=2, sort_keys=True))

def session_for_account(account):
    key = 'acct:' + account
    if key not in sessions_by_key:
        sid = zlib.crc32(account.encode('utf-8')) | 1  # nonzero
        if sid in sessions_by_key.values():
            sid = (sid + 1) | 1
        sessions_by_key[key] = sid
        save_sessions()
    return sessions_by_key[key]

def token_for_account(account):
    # Unique persistent identity token. The server keys player identity on
    # the (token, session) pair -- every client sends token=-1 without steam
    # auth, which merges all players into one identity. One token per account.
    key = 'tok:' + account
    if key not in sessions_by_key:
        tid = zlib.crc32(('tok:' + account).encode('utf-8')) | 1
        while tid == 0xFFFFFFFF or tid in sessions_by_key.values():
            tid = (tid + 2) | 1
        sessions_by_key[key] = tid
        save_sessions()
    return sessions_by_key[key]

def session_for_ip(ip):
    sid = ip_to_session.get(ip)
    if sid:
        return sid
    key = 'ip:' + ip
    if key in sessions_by_key:
        sid = sessions_by_key[key]
    else:
        sid = zlib.crc32(ip.encode('utf-8')) | 1
        if sid in sessions_by_key.values():
            sid = (sid + 1) | 1
        sessions_by_key[key] = sid
        save_sessions()
    ip_to_session[ip] = sid
    return sid

udp_sessions = {}
udp_addr_phone = {} # (ip,port) -> phone int fallback

def phone_str_to_int(s):
    try:
        prefix, suffix = s.split('-')
        return int(prefix) * 10000 + int(suffix)
    except Exception:
        return 2560001

def udp_phone_for(key):
    pkey = 'udp:' + key
    if pkey in phones_by_key:
        return phone_str_to_int(phones_by_key[pkey])
    number = assign_phone(pkey)
    return phone_str_to_int(number)

def account_from_udp_ident(ident_bytes):
    # The client sends a fixed 32-byte account buffer. Bytes after the first
    # NUL are uninitialised and change between launches (for example
    # CROW\0 451 vs CROW\0 482), so they must never be part of the persistent
    # identity key. Accept only ordinary printable account names; genuinely
    # binary identifiers retain the legacy udp:<hex> fallback.
    raw_name = ident_bytes.split(b'\0', 1)[0]
    if not raw_name or len(raw_name) > 31:
        return None
    try:
        account = raw_name.decode('utf-8')
    except UnicodeDecodeError:
        return None
    if not account.strip() or any(ord(c) < 0x20 or ord(c) == 0x7f for c in account):
        return None
    return account

def udp_assign(addr, ident_bytes):
    account = account_from_udp_ident(ident_bytes)
    if account is not None:
        identity_key = 'acct:' + account
        phone = phone_str_to_int(assign_phone(account))
    else:
        raw_key = ident_bytes.hex()
        identity_key = 'udp:' + raw_key
        phone = udp_phone_for(raw_key)
    token = struct.unpack('<I', hashlib.sha1(identity_key.encode()).digest()[:4])[0]
    udp_sessions[token] = phone
    udp_addr_phone[(addr[0], addr[1])] = phone
    print(f'{now()} AUTH H key={identity_key[:64]} phone={phone} token={token:#x}', flush=True)
    return token, phone

def udp_auth_reply(data, addr):
    ptype = data[4]
    if ptype == 0x48:
        if len(data) < 41:
            return None
        token, phone = udp_assign(addr, data[5:37])
        return b'7DFP' + bytes([0x48]) + struct.pack('<III', token, 1, phone)
    if ptype == 0x49:
        if len(data) < 13:
            return None
        token = struct.unpack('<I', data[5:9])[0]
        phone = udp_sessions.get(token) or udp_addr_phone.get((addr[0], addr[1]))
        if phone is None:
            print(f'{now()} AUTH I token={token:#x} from={addr} -> reject', flush=True)
            return b'7DFP' + bytes([0x49]) + struct.pack('<II', 0, 0)
        print(f'{now()} AUTH I token={token:#x} phone={phone} -> accept', flush=True)
        return b'7DFP' + bytes([0x49]) + struct.pack('<II', 1, phone)
    # 'J' server list request: reply 7DFP 'J' + u32 count + u32 start + entries
    # (u32 ip reversed-octets + u16 port LE, same encoding as the 0x40 reply).
    if ptype == 0x4a:
        if not gate_is_authorized(addr[0]):
            print(f'{now()} GATE list denied from={addr[0]}', flush=True)
            return b'7DFP' + bytes([0x4a]) + struct.pack('<II', 0, 0)
        print(f'{now()} LIST J from={addr} -> 1 server', flush=True)
        return (b'7DFP' + bytes([0x4a]) + struct.pack('<II', 1, 0) +
                encode_host(GAME_HOST) + struct.pack('<H', GAME_PORT))
    # 'K' introduce (client asks master to authorize joining server ip:port):
    # reply 7DFP 'K' + u32 session -- client stores it in the join packet.
    if ptype == 0x4b:
        if not gate_is_authorized(addr[0]):
            print(f'{now()} GATE intro denied from={addr[0]}', flush=True)
            return b'7DFP' + bytes([0x4b]) + struct.pack('<I', 0)
        sid = None
        if len(data) >= 9:
            tok = struct.unpack('<I', data[5:9])[0]
            if tok != 0xFFFFFFFF:
                sid = sessions_by_key.get('ses:tok:%d' % tok)
        if sid is None:
            sid = session_for_ip(addr[0])
        print(f'{now()} INTRO K from={addr} session={sid} len={len(data)} hex={binascii.hexlify(data[:32]).decode()}', flush=True)
        return b'7DFP' + bytes([0x4b]) + struct.pack('<I', sid)
    return None

def serverinfo_text(client_ip):
    phone = assign_phone(client_ip)
    return f'078\t{MASTER_HOST}\t{MASTER_PORT}\t{phone}'.encode('ascii')

def list_text():
    return (f'078 {GAME_HOST} {GAME_PORT}\n').encode('ascii')

def phone_key(handler):
    parsed = urllib.parse.urlsplit(handler.path)
    qs = urllib.parse.parse_qs(parsed.query)
    for name in ('steamid', 'id', 'account', 'user', 'ticket'):
        if name in qs and qs[name] and qs[name][0]:
            return qs[name][0]
    return handler.client_address[0]

class Handler(http.server.BaseHTTPRequestHandler):
    def _log_request(self):
        print(f'{now()} HTTP REQ method={self.command} path={self.path} headers={dict(self.headers)} from={self.client_address[0]}', flush=True)

    def _send_json(self, status, value):
        body = json.dumps(value, separators=(',', ':')).encode('utf-8')
        self.send_response(status)
        self.send_header('Content-Type', 'application/json')
        self.send_header('Cache-Control', 'no-store')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self):
        parsed = urllib.parse.urlsplit(self.path)
        if parsed.path != '/gate/login':
            self._send_json(404, {'ok': False})
            return

        try:
            length = int(self.headers.get('Content-Length', '0'))
            if length < 2 or length > 4096:
                raise ValueError('invalid body length')
            request = json.loads(self.rfile.read(length))
            challenge_id = str(request.get('id', ''))
            proof = str(request.get('proof', ''))
            if len(challenge_id) != 32 or len(proof) != 64:
                raise ValueError('invalid proof')
        except Exception:
            self._send_json(400, {'ok': False, 'error': 'invalid_request'})
            return

        ip = self.client_address[0]
        if gate_verify(ip, challenge_id, proof):
            print(f'{now()} GATE authorized ip={ip}', flush=True)
            self._send_json(200, {'ok': True, 'expiresIn': GATE_SESSION_SECONDS})
        else:
            status = 429 if gate_rate_limited(ip) else 403
            print(f'{now()} GATE rejected ip={ip} status={status}', flush=True)
            self._send_json(status, {'ok': False, 'error': 'denied'})

    def do_GET(self):
        self._log_request()
        parsed = urllib.parse.urlsplit(self.path)
        path = parsed.path
        
        if path == '/gate/challenge':
            challenge = gate_new_challenge(self.client_address[0])
            if challenge:
                self._send_json(200, challenge)
            else:
                self._send_json(429 if gate_password else 503,
                                {'ok': False, 'error': 'unavailable'})
            return
        elif path == '/gate/status':
            self._send_json(200, {'authorized': gate_is_authorized(self.client_address[0])})
            return
        elif path == '/gate/check':
            if self.client_address[0] not in ('127.0.0.1', '::1'):
                self._send_json(404, {'ok': False})
                return
            requested_ip = urllib.parse.parse_qs(parsed.query).get('ip', [''])[0]
            self._send_json(200, {'authorized': gate_is_authorized(requested_ip)})
            return
        elif path == '/anewzero/serverinfo.php':
            body = serverinfo_text(self.client_address[0])
        elif path == '/token':
            key = phone_key(self)
            tid = token_for_account(key)
            sid = session_for_account(key)
            ip_to_session[self.client_address[0]] = sid
            sessions_by_key['ses:tok:%d' % tid] = sid
            save_sessions()
            print(f'{now()} TOKEN key={key} -> {tid} session={sid}', flush=True)
            body = str(tid).encode('ascii')
        elif path == '/phone':
            key = phone_key(self)
            number = assign_phone(key)
            sid = session_for_account(key)
            ip_to_session[self.client_address[0]] = sid
            print(f'{now()} PHONE key={key} -> {number} session={sid} query={parsed.query}', flush=True)
            body = number.encode('ascii')
        elif path == '/stream/manifest.txt' or (path.startswith('/stream/') and len(path) > len('/stream/')):
            import pathlib, re
            name = path[len('/stream/'):]
            if name == 'roster.txt':
                # live roster: account name -> phone, from the assignment DB
                lines = []
                for key, number in phones_by_key.items():
                    if key.startswith('udp:') or re.fullmatch(r'[0-9.]+', key):
                        continue
                    lines.append(f'{key}	{number}')
                data = ('\n'.join(lines) + ('\n' if lines else '')).encode('ascii')
                print(f'{now()} STREAM roster.txt len={len(data)} to={self.client_address[0]}', flush=True)
                self.send_response(200)
                self.send_header('Content-Type', 'text/plain')
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(data)
                return
            if name == 'manifest.txt':
                send_file = pathlib.Path('/opt/subrosa/stream/manifest.txt')
            elif '/' in name or '\\' in name or '..' in name:
                send_file = None
            else:
                send_file = pathlib.Path('/opt/subrosa/stream') / name
            if send_file and send_file.is_file():
                data = send_file.read_bytes()
                ctype = 'video/mp4' if name.endswith(('.mp4', '.MP4')) else (
                    'text/plain' if name.endswith('.txt') else 'application/octet-stream')
                print(f'{now()} STREAM {name} len={len(data)} to={self.client_address[0]}', flush=True)
                self.send_response(200)
                self.send_header('Content-Type', ctype)
                self.send_header('Content-Length', str(len(data)))
                self.send_header('Connection', 'close')
                self.end_headers()
                self.wfile.write(data)
                return
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            return
        elif path in ('/hlist', '/list1', '/list2', '/list3', '/list4', '/servers', '/serverlist'):
            body = list_text()
        else:
            print(f'{now()} HTTP unknown path={path} query={parsed.query} from={self.client_address[0]}', flush=True)
            self.send_response(404)
            self.send_header('Content-Length', '0')
            self.send_header('Connection', 'close')
            self.end_headers()
            return
        
        print(f'{now()} HTTP reply path={path} len={len(body)} body={body[:160]!r} from={self.client_address[0]}', flush=True)
        self.send_response(200)
        self.send_header('Content-Type', 'text/plain')
        self.send_header('Content-Length', str(len(body)))
        self.send_header('Connection', 'close')
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, fmt, *args):
        pass

    def handle_one_request(self):
        try:
            # Read request line manually to log even on reset
            self.raw_requestline = self.rfile.readline(65537)
            if not self.raw_requestline:
                self.close_connection = True
                return
            if not self.parse_request():
                return
            self._log_request()
            mname = 'do_' + self.command
            if not hasattr(self, mname):
                self.send_error(501, "Unsupported method")
                return
            getattr(self, mname)()
        except (ConnectionResetError, BrokenPipeError) as e:
            print(f'{now()} HTTP client reset from={self.client_address[0]} error={e}', flush=True)
        except Exception as e:
            print(f'{now()} HTTP error from={self.client_address[0]} error={e}', flush=True)

class ThreadingHTTPServer(socketserver.ThreadingMixIn, http.server.HTTPServer):
    daemon_threads = True
    allow_reuse_address = True

class UDPServer:
    def __init__(self, host='0.0.0.0', port=MASTER_PORT):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.sock.bind((host, port))
        self.port = port

    def serve_forever(self):
        print(f'{now()} UDP master listening on 0.0.0.0:{MASTER_PORT}, advertising {GAME_HOST}:{GAME_PORT}', flush=True)
        while True:
            data, addr = self.sock.recvfrom(8192)
            ptype = data[4] if len(data) >= 5 and data[:4] in (b'7DFP', b'7DFF') else None
            print(f'{now()} UDP in/{self.port} {addr[0]}:{addr[1]} len={len(data)} type={("0x%02x" % ptype) if ptype is not None else "bad"} hex={binascii.hexlify(data[:512]).decode()}', flush=True)
            if ptype in (0x48, 0x49, 0x4a, 0x4b):
                reply = udp_auth_reply(data, addr) or GAME_REPLY
            elif ptype in (0x40, 0x41, 0x42, 0x43):
                reply = GAME_REPLY
            else:
                reply = GAME_REPLY
            self.sock.sendto(reply, addr)
            print(f'{now()} UDP out {addr[0]}:{addr[1]} len={len(reply)} hex={binascii.hexlify(reply[:512]).decode()}', flush=True)

def main():
    threading.Thread(target=UDPServer().serve_forever, daemon=True).start()
    # The vanilla client parses the master port from serverinfo as field+2,
    # so the auth channel ('H') lands on 28017 while the Noxus-repack client
    # sends J/K to the advertised 28015. Serve both.
    try:
        threading.Thread(target=UDPServer(port=MASTER_PORT + 2).serve_forever, daemon=True).start()
    except OSError as e:
        print(f'{now()} UDP {MASTER_PORT + 2} unavailable: {e}', flush=True)
    # the client does ALL master HTTP (hlist/phone) on the port from
    # serverinfo.php, not on 80 -- serve the same handler on both
    httpd80 = ThreadingHTTPServer(('0.0.0.0', 80), Handler)
    threading.Thread(target=httpd80.serve_forever, daemon=True).start()
    httpd28015 = ThreadingHTTPServer(('0.0.0.0', 28015), Handler)
    print(f'{now()} HTTP Noxus master/list listening on 0.0.0.0:80 and 0.0.0.0:28015', flush=True)
    httpd28015.serve_forever()

if __name__ == '__main__':
    main()

