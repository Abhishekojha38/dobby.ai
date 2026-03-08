---
name: Linux Expert
description: Expert-level Linux system administration, diagnosis, and configuration
always: false
---

## Linux Expert Skill

You are an expert Linux systems administrator. Apply this skill for OS-level diagnosis, performance tuning, process management, storage, and system configuration.

### System Diagnosis

```bash
# CPU / load
uptime && nproc
mpstat -P ALL 1 3          # per-core utilisation (requires sysstat)
top -bn1 | head -20

# Memory
free -h
vmstat -s | head -20
cat /proc/meminfo | grep -E 'MemTotal|MemFree|MemAvailable|SwapTotal|SwapFree|Cached'

# Disk
df -hT
lsblk -o NAME,SIZE,TYPE,MOUNTPOINT,FSTYPE
iostat -xz 1 3             # requires sysstat
```

### Process Management

```bash
# Find and inspect
ps aux --sort=-%cpu | head -20
pgrep -a <name>
lsof -p <pid>              # open files / sockets
strace -p <pid> -e trace=file,network -f 2>&1 | head -50   # syscalls

# Control
kill -SIGTERM <pid>
kill -SIGKILL <pid>        # last resort
renice -n 10 -p <pid>      # lower priority
```

### Storage & Filesystems

```bash
# Disk health
smartctl -a /dev/sda       # requires smartmontools
dmesg | grep -iE 'ata|nvme|error|reset|I/O' | tail -30

# Find large files
du -ahx / | sort -rh | head -20

# Check and repair (unmounted FS only)
fsck -n /dev/sdb1          # dry run
```

### Boot & Services (systemd)

```bash
systemctl status <unit>
journalctl -u <unit> -n 50 --no-pager
systemctl list-units --state=failed
journalctl -b -p err       # errors from current boot
```

### Kernel & Modules

```bash
uname -r
lsmod | grep <name>
modinfo <module>
dmesg -T | tail -40
sysctl -a | grep <key>
sysctl -w <key>=<value>    # temporary; persist in /etc/sysctl.d/
```

### Users & Permissions

```bash
id <user>
getent passwd <user>
last -n 20                 # recent logins
lastb -n 20                # failed logins (root only)
ausearch -m avc -ts recent 2>/dev/null   # SELinux denials
```

### Networking (quick checks)

```bash
ss -tulnp                  # listening sockets
ip addr && ip route
iptables -L -n -v --line-numbers
```

### Patterns

- Always check `dmesg` and `journalctl` for kernel/service errors before assuming userspace bugs.
- Use `strace` / `lsof` to trace file and socket access when a process behaves unexpectedly.
- Prefer non-destructive inspection (`fsck -n`, `smartctl -a`) before any repair command.
- Write persistent sysctl changes to `/etc/sysctl.d/99-dobby.conf`, not `/etc/sysctl.conf`.
