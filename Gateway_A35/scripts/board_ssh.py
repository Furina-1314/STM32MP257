#!/usr/bin/env python
"""Board SSH helper: run commands / put files on the ATK-DLMP257B.
Usage: python scripts/board_ssh.py command...   (each arg = one command)
       python scripts/board_ssh.py --put local remote
Credentials come from env or defaults (root@192.168.1.120:22, password 123456)."""
import os, sys
import paramiko

HOST = os.environ.get("BOARD_HOST", "192.168.1.120")
PORT = int(os.environ.get("BOARD_PORT", "22"))
USER = os.environ.get("BOARD_USER", "root")
PASSWORD = os.environ.get("BOARD_PASSWORD", "123456")

def client():
    c = paramiko.SSHClient()
    c.set_missing_host_key_policy(paramiko.AutoAddPolicy())
    c.connect(HOST, port=PORT, username=USER, password=PASSWORD,
              timeout=10, banner_timeout=10, auth_timeout=10)
    return c

def run(c, cmd, timeout=120):
    stdin, stdout, stderr = c.exec_command(cmd, timeout=timeout)
    out = stdout.read().decode("utf-8", "replace")
    err = stderr.read().decode("utf-8", "replace")
    rc = stdout.channel.recv_exit_status()
    return rc, out, err

def put_shell(c, local, remote):
    # The board SSH server has no SFTP subsystem; stream through cat.
    with open(local, "rb") as f:
        data = f.read()
    stdin, stdout, stderr = c.exec_command(f"cat > {remote}")
    stdin.write(data)
    stdin.channel.shutdown_write()
    rc = stdout.channel.recv_exit_status()
    err = stderr.read().decode("utf-8", "replace")
    if rc != 0 or err:
        print(f"put failed rc={rc} err={err}")
        sys.exit(1)
    print(f"put {local} ({len(data)} bytes) -> {remote}")

def main():
    args = sys.argv[1:]
    if not args:
        print(__doc__); sys.exit(2)
    c = client()
    try:
        if args[0] == "--put":
            put_shell(c, args[1], args[2])
            return
        for cmd in args:
            rc, out, err = run(c, cmd)
            print(f"$ {cmd}\n{out}", end="")
            if err.strip():
                print(f"[stderr] {err}", end="")
            print(f"[exit {rc}]")
    finally:
        c.close()

if __name__ == "__main__":
    main()
