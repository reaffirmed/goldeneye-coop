#!/usr/bin/env python3
"""Mock Discord client for tests without Discord: the RPC socket the game's
port/src/coop_discord.c talks to, one per copy.

    discord_mock.py <dir>

listens on <dir>/discord-ipc-0 (run the game with XDG_RUNTIME_DIR=<dir>),
answers the handshake with READY and every command with its reply, and
writes what the game sends to <dir>/discord.log, one JSON frame a line
(SET_ACTIVITY with the activity, SUBSCRIBE, SEND_ACTIVITY_JOIN_INVITE ...).

A test plays Discord's side by dropping files in <dir>:
    join          the secret, sent as ACTIVITY_JOIN (a Join clicked in Discord)
    ask           "<user id> <name>", sent as ACTIVITY_JOIN_REQUEST (Ask to Join)
Each is sent once and deleted.
"""
import json
import os
import socket
import struct
import sys
import time

def frame(op, obj):
    data = json.dumps(obj).encode()
    return struct.pack("<II", op, len(data)) + data

def main():
    d = sys.argv[1]
    os.makedirs(d, exist_ok=True)
    path = os.path.join(d, "discord-ipc-0")
    if os.path.exists(path):
        os.unlink(path)
    log = open(os.path.join(d, "discord.log"), "a", buffering=1)
    srv = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
    srv.bind(path)
    srv.listen(1)
    while True:
        conn, _ = srv.accept()
        conn.setblocking(False)
        buf = b""
        log.write(json.dumps({"mock": "connected", "t": time.time()}) + "\n")
        alive = True
        while alive:
            try:
                chunk = conn.recv(65536)
                if not chunk:
                    alive = False
                buf += chunk
            except BlockingIOError:
                pass
            while len(buf) >= 8:
                op, n = struct.unpack("<II", buf[:8])
                if len(buf) < 8 + n:
                    break
                msg = json.loads(buf[8:8 + n].decode() or "{}")
                buf = buf[8 + n:]
                log.write(json.dumps({"op": op, "msg": msg, "t": time.time()}) + "\n")
                if op == 0:
                    conn.sendall(frame(1, {"cmd": "DISPATCH", "evt": "READY", "data": {
                        "v": 1, "user": {"id": "1000", "username": "mockuser", "global_name": "Mock User"}}}))
                elif op == 1:
                    conn.sendall(frame(1, {"cmd": msg.get("cmd"), "evt": None, "data": None, "nonce": msg.get("nonce")}))
                elif op == 2:
                    alive = False
            for name in ("join", "ask"):
                p = os.path.join(d, name)
                if not os.path.exists(p):
                    continue
                text = open(p).read().strip()
                os.unlink(p)
                if name == "join":
                    ev = {"cmd": "DISPATCH", "evt": "ACTIVITY_JOIN", "data": {"secret": text}}
                else:
                    uid, _, uname = text.partition(" ")
                    ev = {"cmd": "DISPATCH", "evt": "ACTIVITY_JOIN_REQUEST", "data": {
                        "user": {"id": uid, "username": uname.lower(), "global_name": uname, "avatar": None}}}
                conn.sendall(frame(1, ev))
                log.write(json.dumps({"mock": "sent", "event": ev, "t": time.time()}) + "\n")
            time.sleep(0.02)
        conn.close()
        log.write(json.dumps({"mock": "disconnected", "t": time.time()}) + "\n")

if __name__ == "__main__":
    main()
