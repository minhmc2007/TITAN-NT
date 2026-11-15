#!/usr/bin/env bash
set -euo pipefail

# Collects M0 gate data from the app domain (the domain forkd runs in). rish is
# used only for shell-only queries and is skipped when Shizuku is down.
OUT="probe-out"
PROBE="dist/m0_probe/m0_probe"
APP_ID="${RISH_APPLICATION_ID:-com.termux}"
RISH_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
DEX="$RISH_DIR/rish_shizuku.dex"

while [ $# -gt 0 ]; do
  case "$1" in
    --probe) PROBE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    --rish) RISH_DIR="$2"; DEX="$RISH_DIR/rish_shizuku.dex"; shift 2 ;;
    -h|--help) echo "usage: probe_device.sh [--probe PATH] [--out DIR] [--rish DIR]"; exit 0 ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
  esac
done

[ -f "$PROBE" ] || { echo "missing $PROBE; gh run download -n m0-probe-arm64 -D dist/m0_probe" >&2; exit 1; }
chmod +x "$PROBE"
mkdir -p "$OUT/dso"
REPORT="$OUT/report.txt"

rish_up() {
  [ -f "$DEX" ] || return 1
  chmod 400 "$DEX"
  env RISH_APPLICATION_ID="$APP_ID" /system/bin/app_process \
    -Djava.class.path="$DEX" /system/bin --nice-name=rish \
    rikka.shizuku.shell.ShizukuShellLoader -c "$1"
}

{
  echo "== identity =="
  for k in ro.product.model ro.product.device ro.board.platform ro.soc.model \
           ro.build.version.release ro.build.version.sdk ro.build.fingerprint \
           ro.product.cpu.abilist ro.hardware.egl ro.hardware.vulkan ro.opengles.version; do
    printf '%s=%s\n' "$k" "$(getprop "$k")"
  done
  echo "== kernel =="
  cat /proc/version
  echo "== pagesize =="
  getconf PAGESIZE
  echo "== cpuinfo =="
  grep -E 'processor|Features|CPU part|BogoMIPS' /proc/cpuinfo | head -40
  echo "== clocks =="
  for c in 0 1 2 3 4 5 6 7; do
    for f in cpuinfo_max_freq scaling_max_freq scaling_cur_freq; do
      printf 'cpu%s.%s=%s\n' "$c" "$f" \
        "$(cat "/sys/devices/system/cpu/cpu$c/cpufreq/$f" 2>/dev/null)"
    done
  done
  echo "== meminfo =="
  head -3 /proc/meminfo
} >"$REPORT"

if rish_up 'id' >/dev/null 2>&1; then
  {
    echo "== display =="
    rish_up 'wm size; wm density'
    rish_up 'dumpsys SurfaceFlinger | grep -m 8 -iE "activeMode|vsyncRate"'
    echo "== seccomp policy files =="
    rish_up 'ls /system/etc/seccomp_policy/'
  } >>"$REPORT" 2>&1

  B64="$(base64 -w0 "$PROBE")"
  if rish_up "printf %s '$B64' | base64 -d > /data/local/tmp/m0_probe && chmod 755 /data/local/tmp/m0_probe" >/dev/null 2>&1; then
    set +e
    rish_up '/data/local/tmp/m0_probe' >"$OUT/probe_shell.ndjson" 2>&1
    echo "shell_exit=$?" >>"$OUT/probe_shell.ndjson"
    set -e
  fi
  echo "shell_domain=collected" >>"$REPORT"
else
  echo "shell_domain=unavailable" >>"$REPORT"
fi

set +e
"$PROBE" >"$OUT/probe_untrusted_app.ndjson" 2>&1
echo "local_exit=$?" >>"$OUT/probe_untrusted_app.ndjson"
set -e

for p in /system/lib64/libc.so /system/lib64/libaaudio.so \
         /system/lib64/libandroid.so /system/lib64/libmediandk.so \
         /system/bin/linker64; do
  dst="$OUT/dso/$(basename "$p")"
  cp "$p" "$dst"
done
( cd "$OUT/dso" && for f in *.so; do [ -e "$f" ] || continue; sha256sum "$f"; done >sha256.txt )

NODE="$(command -v node || command -v bun || true)"
[ -n "$NODE" ] && : >"$OUT/dso/x18_scan.ndjson" &&
  for f in "$OUT"/dso/*.so; do [ -f "$f" ] && "$NODE" scripts/dso_x18_scan.mjs "$f" >>"$OUT/dso/x18_scan.ndjson"; done

j() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
num() { printf '%s' "$1" | tr -dc '0-9' | head -c 12; }
getv() { sed -n "s/^$1=//p" "$REPORT" | head -1; }

cat >"$OUT/device_profile.json" <<EOF
{
  "captured_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "model": "$(j "$(getv ro.product.model)")",
  "device": "$(j "$(getv ro.product.device)")",
  "board_platform": "$(j "$(getv ro.board.platform)")",
  "soc_model": "$(j "$(getv ro.soc.model)")",
  "android_release": "$(j "$(getv ro.build.version.release)")",
  "sdk": $(num "$(getv ro.build.version.sdk)" || echo 0),
  "kernel": "$(j "$(sed -n 's/^Linux version \([^ ]*\).*/\1/p' "$REPORT")")",
  "pagesize": $(num "$(sed -n '/^== pagesize ==$/{n;p;}' "$REPORT")" || echo 0),
  "cpu_max_freq_khz": {
$(for c in 0 1 2 3 4 5 6 7; do
  v="$(sed -n "s/^cpu$c\.cpuinfo_max_freq=//p" "$REPORT" | head -1)"
  printf '    "cpu%d": %s%s\n' "$c" "$(num "$v" || echo 0)" "$([ "$c" -lt 7 ] && echo ,)"
done)
  }
}
EOF

cat <<EOF
wrote $OUT/device_profile.json, report.txt, probe_untrusted_app.ndjson, dso/
EOF
