#!/usr/bin/env bash
set -euo pipefail

# Usage: bash deploy.sh ghcr.io/owner/repo@sha256:...
IMAGE_REF="${1:?Image reference is required}"

SERVICE="epoll-server.service"
QUADLET="$HOME/.config/containers/systemd/epoll-server.container"

# GitHub Actionsから渡されるイメージ名を検証
if [[ ! "$IMAGE_REF" =~ ^ghcr\.io/sishihara0706/epoll-training@sha256:[0-9a-f]{64}$ ]]; then
  echo "ERROR: Invalid image reference" >&2
  exit 1
fi

if [[ ! -f "$QUADLET" ]]; then
  echo "ERROR: Quadlet file not found: $QUADLET" >&2
  exit 1
fi

if ! grep -q '^Image=' "$QUADLET"; then
  echo "ERROR: Image= not found in Quadlet" >&2
  exit 1
fi

# 旧設定をバックアップ
BACKUP=$(mktemp)
TMP=$(mktemp "${QUADLET}.XXXXXX")

cleanup() {
  rm -f "$BACKUP" "$TMP"
}
trap cleanup EXIT

cp "$QUADLET" "$BACKUP"

echo "=== Deploying $IMAGE_REF ==="

# 実際にTCP echoが応答するか検証
healthcheck() {
  for attempt in $(seq 1 30); do
    if systemctl --user is-active --quiet "$SERVICE"; then
      if python3 - <<'PY'
import socket
import sys

try:
    with socket.create_connection(("127.0.0.1", 8080), timeout=2) as sock:
        sock.settimeout(2)
        sock.sendall(b"deploy-healthcheck\n")

        data = b""
        while not data.endswith(b"\n"):
            chunk = sock.recv(1024)
            if not chunk:
                raise RuntimeError("Connection closed")
            data += chunk

        if data != b"deploy-healthcheck\n":
            raise RuntimeError(f"Unexpected response: {data!r}")
except Exception as exc:
    print(f"Healthcheck failed: {exc}", file=sys.stderr)
    sys.exit(1)
PY
      then
        echo "Healthcheck passed"
        return 0
      fi
    fi

    echo "Waiting for healthy server ($attempt/30)"
    sleep 1
  done

  return 1
}

# 新バージョンへの切り替え
deploy_new() {
  echo "=== Pull image ==="
  podman pull "$IMAGE_REF" || return 1

  echo "=== Update Quadlet ==="
  sed "s|^Image=.*|Image=$IMAGE_REF|" "$BACKUP" > "$TMP" || return 1
  chmod --reference="$QUADLET" "$TMP" || return 1
  mv -f "$TMP" "$QUADLET" || return 1

  echo "=== Restart service ==="
  systemctl --user daemon-reload || return 1
  systemctl --user restart "$SERVICE" || return 1

  echo "=== Verify service ==="
  healthcheck || return 1
}

if deploy_new; then
  echo "DEPLOY SUCCESS: $IMAGE_REF"
  exit 0
fi

echo "DEPLOY FAILED: attempting rollback" >&2

# 旧Quadlet設定に戻す
cp "$BACKUP" "$QUADLET"
systemctl --user daemon-reload

if ! systemctl --user restart "$SERVICE"; then
  echo "CRITICAL: Rollback restart failed" >&2
  exit 1
fi

if ! healthcheck; then
  echo "CRITICAL: Rollback healthcheck failed" >&2
  exit 1
fi

echo "ROLLBACK SUCCESS: previous version restored" >&2

# ロールバックが成功してもCDは失敗扱い
exit 1

