#!/usr/bin/env python3
"""
fake_imap_smtp.py — Minimal fake IMAP + SMTP server for testing Dobby's email channel.

Starts two servers:
  IMAP on port 11433  (plain, no TLS — set imap_url = imap://127.0.0.1:11433)
  SMTP on port 11025  (plain, no TLS — set smtp_url = smtp://127.0.0.1:11025)

Usage:
  python3 fake_imap_smtp.py [--imap-port 11433] [--smtp-port 11025] [--verbose]

Then configure dobby.conf:
  [email]
  imap_url      = imap://127.0.0.1:11433
  smtp_url      = smtp://127.0.0.1:11025
  address       = dobby@test.local
  poll_interval = 5
  inbox         = INBOX

Inject test emails:
  curl -s http://127.0.0.1:18080/inject \
       -d 'from=alice@test.local&subject=Hello&body=Can+you+help?'

Check replies:
  curl -s http://127.0.0.1:18080/replies

Health check:
  curl -s http://127.0.0.1:18080/status
"""

import argparse
import asyncio
import json
import logging
import sys
import threading
import time
from collections import deque
from datetime import datetime
from email.utils import formatdate
from http.server import BaseHTTPRequestHandler, HTTPServer
from urllib.parse import parse_qs, urlparse

log = logging.getLogger("fake_email")

# ── Shared state ─────────────────────────────────────────────────────────────

class MailStore:
    """Thread-safe store for injected messages and captured SMTP replies."""

    def __init__(self):
        self._lock = threading.Lock()
        self._messages = []   # list of dicts: {uid, from, subject, body, seen}
        self._replies  = []   # list of dicts: {to, subject, body, ts}
        self._uid_seq  = 1

    def inject(self, from_addr, subject, body):
        with self._lock:
            uid = self._uid_seq
            self._uid_seq += 1
            self._messages.append({
                "uid":     uid,
                "from":    from_addr,
                "subject": subject,
                "body":    body,
                "seen":    False,
            })
            log.info(f"[Store] Injected UID={uid} from={from_addr} subject={subject!r}")
            return uid

    def search_unseen(self):
        with self._lock:
            return [m["uid"] for m in self._messages if not m["seen"]]

    def fetch(self, uid):
        with self._lock:
            for m in self._messages:
                if m["uid"] == uid:
                    m["seen"] = True
                    # Build minimal RFC 2822 message
                    msg = (
                        f"From: {m['from']}\r\n"
                        f"To: dobby@test.local\r\n"
                        f"Subject: {m['subject']}\r\n"
                        f"Date: {formatdate()}\r\n"
                        f"Message-ID: <test-{uid}@fake.local>\r\n"
                        f"\r\n"
                        f"{m['body']}\r\n"
                    )
                    return msg
            return None

    def record_reply(self, to, subject, body):
        with self._lock:
            self._replies.append({
                "to":      to,
                "subject": subject,
                "body":    body,
                "ts":      datetime.now().isoformat(),
            })
            log.info(f"[Store] Reply captured → {to}: {subject!r}")

    def status(self):
        with self._lock:
            return {
                "messages": len(self._messages),
                "unseen":   sum(1 for m in self._messages if not m["seen"]),
                "replies":  len(self._replies),
            }

    def get_replies(self):
        with self._lock:
            return list(self._replies)


store = MailStore()

# ── Fake IMAP server ─────────────────────────────────────────────────────────
#
# Implements just enough of IMAP4rev1 for libcurl:
#   - CAPABILITY
#   - LOGIN
#   - SELECT
#   - SEARCH UNSEEN
#   - FETCH <uid> (RFC822)
#   - STORE <uid> +FLAGS (\Seen)
#   - LOGOUT

class IMAPSession(asyncio.Protocol):

    GREETING = b"* OK Dobby Fake IMAP Server ready\r\n"

    def __init__(self):
        self.transport = None
        self.buf = b""
        self.authed = False
        self.selected = False
        log.debug("[IMAP] New connection")

    def connection_made(self, transport):
        self.transport = transport
        self.transport.write(self.GREETING)

    def data_received(self, data):
        self.buf += data
        while b"\r\n" in self.buf:
            line, self.buf = self.buf.split(b"\r\n", 1)
            self._handle(line.decode(errors="replace"))

    def _send(self, *lines):
        for line in lines:
            log.debug(f"[IMAP] S: {line}")
            self.transport.write((line + "\r\n").encode())

    def _handle(self, line):
        log.debug(f"[IMAP] C: {line}")
        parts = line.split(None, 2)
        if len(parts) < 2:
            return
        tag, cmd = parts[0], parts[1].upper()
        rest = parts[2] if len(parts) > 2 else ""

        if cmd == "CAPABILITY":
            self._send("* CAPABILITY IMAP4rev1 AUTH=PLAIN",
                       f"{tag} OK CAPABILITY completed")

        elif cmd == "LOGIN":
            self.authed = True
            self._send(f"{tag} OK LOGIN completed")

        elif cmd == "SELECT":
            self.selected = True
            unseen = store.search_unseen()
            self._send(
                f"* {len(store._messages)} EXISTS",
                f"* {len(unseen)} RECENT",
                f"* OK [UNSEEN {unseen[0] if unseen else 0}]",
                f"* FLAGS (\\Seen \\Recent)",
                f"{tag} OK [READ-WRITE] SELECT completed",
            )

        elif cmd == "SEARCH":
            # SEARCH UNSEEN or SEARCH ALL
            uids = store.search_unseen()
            uid_str = " ".join(str(u) for u in uids)
            self._send(f"* SEARCH {uid_str}", f"{tag} OK SEARCH completed")

        elif cmd == "UID" and rest.upper().startswith("SEARCH"):
            uids = store.search_unseen()
            uid_str = " ".join(str(u) for u in uids)
            self._send(f"* SEARCH {uid_str}", f"{tag} OK UID SEARCH completed")

        elif cmd == "FETCH" or (cmd == "UID" and rest.upper().startswith("FETCH")):
            # Extract UID — handle both "FETCH 5 RFC822" and "UID FETCH 5 RFC822"
            fetch_rest = rest if cmd == "FETCH" else rest[5:].strip()
            uid_str = fetch_rest.split()[0]
            try:
                uid = int(uid_str.rstrip("."))
            except ValueError:
                self._send(f"{tag} BAD invalid UID")
                return
            msg = store.fetch(uid)
            if msg:
                self._send(
                    f"* {uid} FETCH (RFC822 {{{len(msg.encode())}}}",
                )
                self.transport.write(msg.encode())
                self._send(")", f"{tag} OK FETCH completed")
            else:
                self._send(f"{tag} NO message not found")

        elif cmd == "STORE":
            # Mark seen — just acknowledge
            self._send(f"{tag} OK STORE completed")

        elif cmd == "LOGOUT":
            self._send("* BYE Dobby IMAP server logging out",
                       f"{tag} OK LOGOUT completed")
            self.transport.close()

        elif cmd == "NOOP":
            self._send(f"{tag} OK NOOP completed")

        else:
            self._send(f"{tag} BAD command not recognised: {cmd}")

    def connection_lost(self, exc):
        log.debug("[IMAP] Connection closed")


# ── Fake SMTP server ─────────────────────────────────────────────────────────

class SMTPSession(asyncio.Protocol):

    GREETING = b"220 fake.local Dobby Fake SMTP\r\n"

    def __init__(self):
        self.transport = None
        self.buf = b""
        self.rcpt = None
        self.mail_from = None
        self.in_data = False
        self.data_buf = []
        log.debug("[SMTP] New connection")

    def connection_made(self, transport):
        self.transport = transport
        self.transport.write(self.GREETING)

    def data_received(self, data):
        self.buf += data
        while b"\r\n" in self.buf:
            line, self.buf = self.buf.split(b"\r\n", 1)
            self._handle(line.decode(errors="replace"))

    def _send(self, line):
        log.debug(f"[SMTP] S: {line}")
        self.transport.write((line + "\r\n").encode())

    def _handle(self, line):
        log.debug(f"[SMTP] C: {line}")

        if self.in_data:
            if line == ".":
                # End of DATA
                self.in_data = False
                raw = "\r\n".join(self.data_buf)
                self._parse_and_record(raw)
                self.data_buf = []
                self._send("250 OK message accepted")
            else:
                # Unstuff leading dot
                self.data_buf.append(line[1:] if line.startswith("..") else line)
            return

        upper = line.upper()
        if upper.startswith("EHLO") or upper.startswith("HELO"):
            self._send("250-fake.local Hello")
            self._send("250 AUTH PLAIN")
        elif upper.startswith("AUTH"):
            self._send("235 Authentication successful")
        elif upper.startswith("MAIL FROM"):
            self.mail_from = line
            self._send("250 OK")
        elif upper.startswith("RCPT TO"):
            # Extract address
            lt = line.find("<")
            gt = line.find(">")
            self.rcpt = line[lt+1:gt] if lt >= 0 and gt > lt else line[9:]
            self._send("250 OK")
        elif upper == "DATA":
            self.in_data = True
            self._send("354 Start mail input; end with <CRLF>.<CRLF>")
        elif upper == "QUIT":
            self._send("221 Bye")
            self.transport.close()
        elif upper.startswith("NOOP"):
            self._send("250 OK")
        else:
            self._send("500 Unknown command")

    def _parse_and_record(self, raw):
        # Extract Subject and body from the raw message
        subject = ""
        lines = raw.split("\r\n") if "\r\n" in raw else raw.split("\n")
        in_headers = True
        body_lines = []
        for ln in lines:
            if in_headers:
                if ln == "":
                    in_headers = False
                elif ln.upper().startswith("SUBJECT:"):
                    subject = ln[8:].strip()
            else:
                body_lines.append(ln)
        body = "\n".join(body_lines).strip()
        store.record_reply(
            to=self.rcpt or "unknown",
            subject=subject,
            body=body,
        )

    def connection_lost(self, exc):
        log.debug("[SMTP] Connection closed")


# ── Control HTTP server ───────────────────────────────────────────────────────

class ControlHandler(BaseHTTPRequestHandler):

    def log_message(self, fmt, *args):
        log.debug(f"[HTTP] {fmt % args}")

    def do_GET(self):
        parsed = urlparse(self.path)
        if parsed.path == "/status":
            self._json(store.status())
        elif parsed.path == "/replies":
            self._json(store.get_replies())
        elif parsed.path == "/messages":
            with store._lock:
                self._json(store._messages)
        else:
            self._json({"error": "unknown path"}, 404)

    def do_POST(self):
        parsed = urlparse(self.path)
        length = int(self.headers.get("Content-Length", 0))
        body = self.rfile.read(length).decode()
        params = {k: v[0] for k, v in parse_qs(body).items()}

        if parsed.path == "/inject":
            from_addr = params.get("from", "user@test.local")
            subject   = params.get("subject", "Test message")
            msg_body  = params.get("body", "Hello Dobby!")
            uid = store.inject(from_addr, subject, msg_body)
            self._json({"ok": True, "uid": uid})
        elif parsed.path == "/clear":
            with store._lock:
                store._messages.clear()
                store._replies.clear()
            self._json({"ok": True})
        else:
            self._json({"error": "unknown path"}, 404)

    def _json(self, data, code=200):
        body = json.dumps(data, indent=2).encode()
        self.send_response(code)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", len(body))
        self.end_headers()
        self.wfile.write(body)


# ── Main ─────────────────────────────────────────────────────────────────────

async def start_servers(imap_port, smtp_port):
    loop = asyncio.get_running_loop()

    imap_srv = await loop.create_server(IMAPSession, "127.0.0.1", imap_port)
    smtp_srv = await loop.create_server(SMTPSession, "127.0.0.1", smtp_port)

    log.info(f"IMAP  listening on 127.0.0.1:{imap_port}")
    log.info(f"SMTP  listening on 127.0.0.1:{smtp_port}")

    async with imap_srv, smtp_srv:
        await asyncio.gather(imap_srv.serve_forever(), smtp_srv.serve_forever())


def run_control(port):
    srv = HTTPServer(("127.0.0.1", port), ControlHandler)
    log.info(f"HTTP control on 127.0.0.1:{port}")
    srv.serve_forever()


if __name__ == "__main__":
    p = argparse.ArgumentParser(description="Fake IMAP+SMTP server for Dobby email testing")
    p.add_argument("--imap-port", type=int, default=11433)
    p.add_argument("--smtp-port", type=int, default=11025)
    p.add_argument("--http-port", type=int, default=18080)
    p.add_argument("--verbose", action="store_true")
    args = p.parse_args()

    logging.basicConfig(
        level=logging.DEBUG if args.verbose else logging.INFO,
        format="%(asctime)s  %(name)s  %(message)s",
        datefmt="%H:%M:%S",
    )

    print(f"""
╔══════════════════════════════════════════════════════════╗
║     Dobby Fake Email Server                              ║
╠══════════════════════════════════════════════════════════╣
║  IMAP  imap://127.0.0.1:{args.imap_port:<5}  (plain, no TLS)    ║
║  SMTP  smtp://127.0.0.1:{args.smtp_port:<5}  (plain, no TLS)    ║
║  HTTP  http://127.0.0.1:{args.http_port:<5}  (control API)      ║
╠══════════════════════════════════════════════════════════╣
║  Inject a test email:                                    ║
║    curl -s http://127.0.0.1:{args.http_port}/inject \\         ║
║      -d 'from=user@test.local&subject=Hi&body=Hello'     ║
║  Check replies:                                          ║
║    curl -s http://127.0.0.1:{args.http_port}/replies           ║
║  Status:                                                 ║
║    curl -s http://127.0.0.1:{args.http_port}/status            ║
╚══════════════════════════════════════════════════════════╝
""")

    # Control server in background thread
    t = threading.Thread(target=run_control, args=(args.http_port,), daemon=True)
    t.start()

    try:
        asyncio.run(start_servers(args.imap_port, args.smtp_port))
    except KeyboardInterrupt:
        print("\nShutting down.")
