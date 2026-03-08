# Email Channel — Test Suite

Three layers of testing, each runnable independently.

---

## Layer 1 — Unit tests (no dependencies)

Tests all parsing logic in isolation: `extract_addr`, `extract_header`,
`extract_body`, and UID list parsing from IMAP SEARCH responses.

```bash
cd tests/email
gcc -std=c17 -D_GNU_SOURCE -Wall -Wextra \
    -I../../src \
    test_email_parsing.c -o test_email_parsing
./test_email_parsing
```

No libcurl, no IMAP server, no Dobby binary needed. Runs in < 1ms.

---

## Layer 2 — Fake IMAP+SMTP server

A Python 3 server (no dependencies beyond stdlib) that implements enough
of IMAP4rev1 and SMTP to exercise the full libcurl path in `email_channel.c`.

```bash
cd tests/email
python3 fake_imap_smtp.py --verbose
```

Control API (while server is running):

```bash
# Inject a test email
curl -s http://127.0.0.1:18080/inject \
     -d 'from=alice@test.local&subject=Hello&body=Can+you+help?'

# Check what Dobby replied
curl -s http://127.0.0.1:18080/replies | jq .

# Server status
curl -s http://127.0.0.1:18080/status

# Reset all messages and replies
curl -s http://127.0.0.1:18080/clear -d ''
```

Configure `dobby.conf` to point at it:

```ini
[email]
imap_url      = imap://127.0.0.1:11433
smtp_url      = smtp://127.0.0.1:11025
address       = dobby@test.local
poll_interval = 5
inbox         = INBOX
```

No `EMAIL_PASSWORD` needed — the fake server accepts any credentials.

---

## Layer 3 — End-to-end integration test

Starts the fake server, starts Dobby, injects emails, waits for replies,
verifies everything worked.

```bash
cd tests/email

# Build Dobby first if you haven't already
cd ../..
mkdir -p build && cd build && cmake .. && make -j$(nproc) && cd ../..

# Run integration test
./tests/email/run_integration_test.sh ./build/bin/dobby
```

Without a Dobby binary it still runs the protocol smoke tests:
```bash
./tests/email/run_integration_test.sh   # skips Dobby, tests fake server only
```

---

## Layer 4 — Real email (Gmail)

1. Create a Gmail App Password: `myaccount.google.com/apppasswords`
2. Set `dobby.conf`:
   ```ini
   [email]
   imap_url      = imaps://imap.gmail.com:993
   smtp_url      = smtps://smtp.gmail.com:465
   address       = yourbot@gmail.com
   poll_interval = 30
   ```
3. Set in `.env`: `EMAIL_PASSWORD=your-app-password`
4. Run `./build/bin/dobby --log-level debug`
5. Send an email to `yourbot@gmail.com`
6. Watch the debug log and wait for the reply

---

## What to look for in logs

```
[DEBUG] Email channel configured: imap=... addr=... poll=5s
[DEBUG] Email poll thread started, interval=5s
[DEBUG] Email IMAP search result: 1 2
[DEBUG] Email from=alice@test.local uid=1 len=42
[DEBUG] Email sent to alice@test.local
```

If the poll thread starts but no messages arrive, check:
- `imap_url` — must match what the server is listening on
- Credentials in `EMAIL_PASSWORD` env var
- Firewall / port reachability (`curl -v imap://host:port`)

If messages arrive but no replies, check:
- `smtp_url` — must be reachable
- The LLM provider is running (Ollama or API key set)
- `/tmp/dobby_test.log` for agent errors
