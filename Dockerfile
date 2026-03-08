# SPDX-License-Identifier: MIT
# Dobby AI Agent — Multi-stage Docker build
#
# Build:
#   docker build -t dobby .
#   docker build --build-arg BUILD_TYPE=Debug -t dobby:debug .
#
# Run (local Ollama on host):
#   docker run -it --rm \
#     -p 8080:8080 \
#     -e PROVIDER=ollama \
#     -e MODEL=mistral-nemo:12b \
#     -e OLLAMA_URL=http://host.docker.internal:11434 \
#     -v dobby-memory:/app/workspace/memory \
#     dobby
#
# Run (Groq):
#   docker run -it --rm \
#     -p 8080:8080 \
#     -e PROVIDER=groq \
#     -e MODEL=llama-3.3-70b-versatile \
#     -e API_KEY=gsk_... \
#     -v dobby-memory:/app/workspace/memory \
#     dobby --no-cli
#
# See docker-compose.yml for a full stack with optional LiteLLM proxy.

# ── Stage 1: Build ──────────────────────────────────────────────────────────
FROM ubuntu:24.04 AS builder

ARG BUILD_TYPE=Release
ARG DEBIAN_FRONTEND=noninteractive

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential \
    cmake \
    libcurl4-openssl-dev \
    libreadline-dev \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY . .

RUN mkdir -p build && cd build \
    && cmake -DCMAKE_BUILD_TYPE=${BUILD_TYPE} .. \
    && make -j"$(nproc)" \
    && strip --strip-unneeded bin/dobby

# ── Stage 2: Runtime ───────────────────────────────────────────────────────
FROM ubuntu:24.04

ARG DEBIAN_FRONTEND=noninteractive
LABEL org.opencontainers.image.title="Dobby AI Agent" \
      org.opencontainers.image.description="Persistent AI agent daemon — CLI, HTTP, and Email channels" \
      org.opencontainers.image.version="1.0.0" \
      org.opencontainers.image.licenses="MIT"

RUN apt-get update && apt-get install -y --no-install-recommends \
    # Runtime libraries
    ca-certificates \
    libcurl4 \
    libreadline8t64 \
    # Shell and process utilities
    bash \
    coreutils \
    procps \
    curl \
    # Networking tools (linux / network skills)
    iproute2 \
    iputils-ping \
    dnsutils \
    socat \
    openssh-client \
    rsync \
    # Terminal multiplexer (required for shell_exec tool)
    tmux \
    # System diagnosis (linux skill)
    sysstat \
    lsof \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /app

# ── Binary ─────────────────────────────────────────────────────────────────
COPY --from=builder /build/build/bin/dobby ./dobby

# ── Configuration ──────────────────────────────────────────────────────────
COPY dobby.conf allowlist.conf ./
# .env is NOT copied — pass secrets via environment variables at runtime.
# The binary reads EMAIL_PASSWORD, API_KEY, etc. directly from the environment.

# ── Static web assets (dashboard) ──────────────────────────────────────────
COPY assets/ ./assets/

# ── Bundled skills (seeded into workspace on first run; never overwritten) ──
# Skills live at /app/skills — seeded to $WORKSPACE/skills on startup.
COPY skills/ ./skills/
RUN chmod +x ./skills/tmux/scripts/*.sh

# ── Templates (SOUL.md, AGENT.md, TOOLS.md — seeded on first run) ──────────
COPY templates/ ./templates/

# ── Workspace volume — ONLY memory persists; skills come from image ────────
# Mounting this volume persists conversation memory and history across restarts.
# Skills are always re-seeded from the image on startup (never overwritten).
VOLUME ["/app/workspace"]

# Pre-create workspace dir so first-run seeding has somewhere to write
RUN mkdir -p workspace/memory workspace/skills

# ── Network ────────────────────────────────────────────────────────────────
EXPOSE 8080

# ── Health check ───────────────────────────────────────────────────────────
HEALTHCHECK --interval=30s --timeout=10s --start-period=15s --retries=3 \
    CMD curl -sf http://localhost:8080/api/status | grep -q '"status":"ok"' || exit 1

# ── Entrypoint ─────────────────────────────────────────────────────────────
# Default: headless HTTP gateway + email channel (no interactive TTY needed).
# Override CMD to add --no-http, --port, etc.
ENTRYPOINT ["./dobby"]
CMD ["--no-cli"]
