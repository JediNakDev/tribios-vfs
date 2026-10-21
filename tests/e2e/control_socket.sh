#!/usr/bin/env bash
# A disconnected control client must not terminate the daemon with SIGPIPE.
source "$(dirname "$0")/lib.sh"
make_project
configure_project
start_daemon
tribios workspace create messages >/dev/null
socket_path="$(sed -n 's/^tribios_daemon: serving .* on //p' "$PROJECT/.tribios/daemon.log" | tail -1)"
[ -n "$socket_path" ] || fail "the daemon did not report its control socket"

python3 - "$socket_path" "$TRIBIOS_BIN" "$PROJECT" <<'PYTHON'
import socket
import subprocess
import sys

socket_path, cli, project = sys.argv[1:]
socket_path = socket_path.strip('"')
with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as connection:
    connection.settimeout(5)
    connection.connect(socket_path)
    # Disable replies before the request reaches the daemon.
    connection.shutdown(socket.SHUT_RD)
    connection.sendall(b"ping\n")
    connection.shutdown(socket.SHUT_WR)
    subprocess.run([cli, "--project", project, "daemon", "status"], check=True, timeout=5)

# Percent encoding makes this larger than a Unix socket's usual send buffer.
payload = "\n" * 120000
command = [cli, "--project", project, "fs"]
subprocess.run(command + ["write", "messages", "large.txt", payload, "0"],
               check=True, capture_output=True, timeout=10)
reply = subprocess.run(command + ["read", "messages", "large.txt"],
                       check=True, capture_output=True, text=True, timeout=10)
assert reply.stdout == payload + "\n", "the control connection truncated a large message"
PYTHON
stop_daemon
echo "PASS control socket"
