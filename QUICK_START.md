# Dobby — Quick Start Guide

---

## 1. Build

```bash
git clone <repo>
cd dobby

mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release ..
make -j$(nproc)
cd ..
```

The binary is at `build/bin/dobby`.

---

## 2. Choose a Provider

### Option A — Local Ollama (no API key needed)

```bash
# Install and start Ollama: https://ollama.com
ollama pull mistral-nemo:12b
```

`dobby.conf` is already set up for Ollama — no changes needed.

### Option B — Groq (fast, free tier)

Get a key at console.groq.com, then:

```bash
# .env
API_KEY=gsk_...
```

```ini
# dobby.conf
[provider]
type  = groq
model = llama-3.3-70b-versatile
```

### Option C — OpenAI

```bash
# .env
API_KEY=sk-...
```

```ini
[provider]
type  = openai
model = gpt-4o
```
---

## 3. Run

```bash
# Interactive CLI + web dashboard
./build/bin/dobby

# Headless (web + email, no terminal needed)
./build/bin/dobby --no-cli

# CLI only (no HTTP server)
./build/bin/dobby --no-http
```

Open **http://localhost:8080** for the web dashboard.

---

## 4. Talk to Dobby

**From the CLI:**
```
You: What's the disk usage on this machine?
You: /status
You: /help
```

**From the web dashboard:**
Visit http://localhost:8080 → Chat tab.

**From curl:**
```bash
curl -X POST http://localhost:8080/api/chat \
  -H "Content-Type: application/json" \
  -d '{"message": "How much memory is available?"}'
```

---

## 5. Set Up Email (optional)

### Gmail

1. Go to [myaccount.google.com/apppasswords](https://myaccount.google.com/apppasswords)
2. Create an App Password for "Mail"
3. Enable IMAP in Gmail Settings → Forwarding and POP/IMAP

```ini
# dobby.conf
[email]
imap_url      = imaps://imap.gmail.com:993
smtp_url      = smtps://smtp.gmail.com:465
address       = you@gmail.com
poll_interval = 60
inbox         = INBOX
subject_tag   = [Dobby]
```

```bash
# .env
EMAIL_PASSWORD=your16charapppassword
```

### Self-hosted IMAP

```ini
[email]
imap_url = imap://mail.yourdomain.com:143
smtp_url = smtp://mail.yourdomain.com:587
address  = dobby@yourdomain.com
```

### Restrict who can email Dobby

```ini
# allowlist.conf
[email]
allow = you@gmail.com, trusted@example.com, *@mycompany.com
```

Or manage at runtime:
```
/email allow  colleague@example.com
/email deny   spammer@example.com
/email allowlist
/email status
```

---

## 6. Skills

Skills extend what Dobby knows how to do. They live in `~/.dobby/skills/` and are seeded from the bundled `skills/` directory on first run.

**View loaded skills:**
```
/skills
```

**Add a custom skill:**
```bash
mkdir -p ~/.dobby/skills/my-skill
cat > ~/.dobby/skills/my-skill/SKILL.md << 'SKILL'
---
name: My Skill
description: Does something useful
always: false
---

# My Skill

Instructions for the agent about how to use this skill...
SKILL
```

Restart Dobby (or reload) to pick it up.

---

## 7. Docker Quick Start

```bash
# Copy and edit config
cp .env.example .env
# Add API_KEY=... (or EMAIL_PASSWORD=...) to .env

# Build and start
docker compose up

# In the background
docker compose up -d
docker compose logs -f dobby
```

The workspace (memory, history) is persisted in the `dobby-workspace` Docker volume.

---

## 8. Common Flags

| Flag | Description |
|---|---|
| `--no-cli` | Disable interactive terminal (headless mode) |
| `--no-http` | Disable HTTP server and dashboard |
| `--port N` | Override HTTP port (default: 8080) |
| `--config /path/dobby.conf` | Use custom config file |
| `--debug` | Enable debug logging |
| `--version` | Print version and exit |

---

## Troubleshooting

**Dobby starts but gives empty responses:**
- Check `LOG_LEVEL=debug` and inspect logs
- Verify the Ollama URL is reachable: `curl http://localhost:11434/api/tags`
- Make sure the model is pulled: `ollama list`

**Email not being received:**
- Check `/email status` — is `running: true`?
- Verify IMAP credentials and App Password
- Set `log level = debug` and watch for IMAP errors

**Skills directory empty:**
- Check `[paths]` section in `dobby.conf` — `skills_src = skills` should resolve to the `skills/` dir next to the binary
- Or set `SKILLS_SRC=/absolute/path/to/skills` in `.env`

**Cannot connect to Ollama in Docker:**
- The compose file sets `host.docker.internal:host-gateway` — ensure Docker Desktop or equivalent supports this
- Or use `OLLAMA_URL=http://172.17.0.1:11434`

**Build fails (readline not found):**
- readline is optional — Dobby builds and runs without it (using fgets)
- Install with: `apt-get install libreadline-dev`
