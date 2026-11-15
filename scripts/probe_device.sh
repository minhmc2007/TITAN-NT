#!/usr/bin/env bash
set -euo pipefail

OUT="probe-out"
SERIAL_ARGS=()
PROBE=""

usage() {
  cat <<'EOF'
usage: scripts/probe_device.sh [--serial SERIAL] [--probe PATH] [--out DIR]

Collects the Milestone 0 gate data from a Galaxy A55 (SM-A556E) over adb into
OUT (default probe-out/):
  device_profile.json  key props, page size, kernel, clocks
  report.txt           probe transcript (cpuinfo, seccomp, dumpsys, ...)
  probe.ndjson         m0_probe output (futex_waitv, memfd, low-VA map, affinity)
  dso/                 pulled bionic DSOs, sha256, x18 SCS scan
EOF
}

while [ $# -gt 0 ]; do
  case "$1" in
    --serial) SERIAL_ARGS=(-s "$2"); shift 2 ;;
    --probe) PROBE="$2"; shift 2 ;;
    --out) OUT="$2"; shift 2 ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

command -v adb >/dev/null || {
  echo "adb not found; Arch ARM: pacman -S android-tools" >&2
  exit 1
}
adb "${SERIAL_ARGS[@]}" get-state >/dev/null 2>&1 || {
  echo "no device; check 'adb devices' and accept the USB debugging prompt" >&2
  exit 1
}

mkdir -p "$OUT/dso"
sh_() { adb "${SERIAL_ARGS[@]}" shell "$@" | tr -d '\r'; }
prop() { sh_ getprop "$1" | head -c 200; }

# Any DSO built with -ffixed-x18 is called through x18_guard; stock bionic is not.
if [ -z "$PROBE" ]; then
  for cand in dist/m0_probe/m0_probe dist/m0_probe/arm64-v8a/m0_probe; do
    if [ -f "$cand" ]; then PROBE="$cand"; break; fi
  done
fi
[ -f "${PROBE:-/nonexistent}" ] || {
  echo "m0_probe missing; fetch it with: gh run download -n m0-probe-arm64" >&2
  echo "then rerun with: --probe dist/m0_probe/m0_probe" >&2
  exit 1
}

REPORT="$OUT/report.txt"
{
  echo "== identity =="
  for k in ro.product.model ro.product.device ro.board.platform ro.soc.model \
           ro.build.version.release ro.build.version.sdk ro.build.fingerprint \
           ro.product.cpu.abilist ro.hardware.egl ro.hardware.vulkan \
           ro.opengles.version ro.vulkan.version; do
    printf '%s=%s\n' "$k" "$(prop "$k")"
  done

  echo
  echo "== uname =="
  sh_ uname -a
  echo "== pagesize =="
  sh_ getconf PAGESIZE
  echo "== cpuinfo =="
  sh_ cat /proc/cpuinfo | head -60

  echo "== clocks (kHz) =="
  for c in 0 1 2 3 4 5 6 7; do
    for f in cpuinfo_max_freq scaling_max_freq; do
      printf 'cpu%s.%s=%s\n' "$c" "$f" \
        "$(sh_ cat "/sys/devices/system/cpu/cpu$c/cpufreq/$f" 2>/dev/null || true)"
    done
  done

  echo
  echo "== seccomp =="
  sh_ cat /proc/self/status | grep -E 'Seccomp' || true
  echo "== selinux =="
  sh_ cat /proc/self/attr/current || true
  echo "== gpu props =="
  sh_ getprop | grep -iE 'gpu|vulkan|egl|radv|exynos' || true
  echo "== display =="
  sh_ wm size; sh_ wm density
  sh_ dumpsys SurfaceFlinger | grep -m 20 -iE 'refresh|activeMode|vsync' || true
  echo "== meminfo =="
  sh_ cat /proc/meminfo | head -5
} | tee "$REPORT"

adb "${SERIAL_ARGS[@]}" push "$PROBE" /data/local/tmp/m0_probe >/dev/null
sh_ chmod 755 /data/local/tmp/m0_probe
sh_ /data/local/tmp/m0_probe | tee "$OUT/probe.ndjson"

for p in \
  /apex/com.android.runtime/lib64/bionic/libc.so \
  /system/lib64/libc.so \
  /system/lib64/libaaudio.so \
  /system/lib64/libandroid.so \
  /system/lib64/libmediandk.so; do
  dst="$OUT/dso/$(printf '%s' "$p" | tr '/' '_')"
  adb "${SERIAL_ARGS[@]}" pull "$p" "$dst" >/dev/null 2>&1 ||
    echo "pull failed: $p" >>"$OUT/pull_failures.txt"
done

NODE="$(command -v node || command -v bun || true)"
if [ -n "$NODE" ] && [ -f scripts/dso_x18_scan.mjs ]; then
  : >"$OUT/dso/x18_scan.ndjson"
  for f in "$OUT"/dso/*.so; do
    [ -f "$f" ] || continue
    "$NODE" scripts/dso_x18_scan.mjs "$f" >>"$OUT/dso/x18_scan.ndjson"
  done
else
  echo "node/bun missing; scan later with: node scripts/dso_x18_scan.mjs $OUT/dso/*.so"
fi

( cd "$OUT/dso" && sha256sum ./*.so >sha256.txt )

j() { printf '%s' "$1" | sed 's/\\/\\\\/g; s/"/\\"/g'; }
num() { printf '%s' "$1" | tr -dc '0-9' | head -c 12; }

cat >"$OUT/device_profile.json" <<EOF
{
  "captured_at": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "model": "$(j "$(prop ro.product.model)")",
  "device": "$(j "$(prop ro.product.device)")",
  "board_platform": "$(j "$(prop ro.board.platform)")",
  "soc_model": "$(j "$(prop ro.soc.model)")",
  "android_release": "$(j "$(prop ro.build.version.release)")",
  "sdk": $(num "$(prop ro.build.version.sdk)" || echo 0),
  "fingerprint": "$(j "$(prop ro.build.fingerprint)")",
  "abilist": "$(j "$(prop ro.product.cpu.abilist)")",
  "kernel": "$(j "$(sh_ uname -r | head -1)")",
  "pagesize": $(num "$(sh_ getconf PAGESIZE)" || echo 0),
  "cpu_max_freq_khz": {
$(for c in 0 1 2 3 4 5 6 7; do
  v="$(sh_ cat "/sys/devices/system/cpu/cpu$c/cpufreq/cpuinfo_max_freq" 2>/dev/null || true)"
  printf '    "cpu%d": %s%s\n' "$c" "$(num "$v" || echo 0)" "$([ "$c" -lt 7 ] && echo ,)"
done)
  }
}
EOF

cat <<EOF

wrote:
  $OUT/device_profile.json
  $OUT/report.txt
  $OUT/probe.ndjson
  $OUT/dso/

send back device_profile.json, probe.ndjson, dso/x18_scan.ndjson and dso/sha256.txt.
m0_probe runs in the adb shell domain; untrusted_app seccomp is only proven once
Milestone 1 runs it through libtitan_forkd.so.
EOF
