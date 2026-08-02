#!/usr/bin/env bash

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
FQBN="esp32:esp32:lilygo_t_display"
EXPECTED_CLI_VERSION="1.5.1"
EXPECTED_CORE_VERSION="3.3.10"
EXPECTED_DHT20_VERSION="0.3.2"
EXPECTED_TFT_VERSION="2.5.43"

usage() {
  echo "Usage: $0 [--display-on|--display-off|--both]"
}

mode="--both"
if [[ $# -gt 1 ]]; then
  usage >&2
  exit 2
fi
if [[ $# -eq 1 ]]; then
  mode="$1"
fi
case "$mode" in
  --display-on|--display-off|--both) ;;
  *) usage >&2; exit 2 ;;
esac

command -v arduino-cli >/dev/null || {
  echo "arduino-cli is required." >&2
  exit 1
}
command -v shasum >/dev/null || {
  echo "shasum is required." >&2
  exit 1
}

cli_version="$(arduino-cli version | sed -E 's/.*Version: ([^ ]+).*/\1/')"
core_version="$(arduino-cli core list | awk '$1 == "esp32:esp32" {print $2; exit}')"
library_list="$(arduino-cli lib list)"

require_library() {
  local name="$1"
  local version="$2"
  if ! grep -Eq "^${name}[[:space:]]+${version}([[:space:]]|$)" <<<"$library_list"; then
    echo "Required library/version missing: ${name} ${version}" >&2
    exit 1
  fi
}

if [[ "$cli_version" != "$EXPECTED_CLI_VERSION" ]]; then
  echo "Expected arduino-cli ${EXPECTED_CLI_VERSION}; found ${cli_version}." >&2
  exit 1
fi
if [[ "$core_version" != "$EXPECTED_CORE_VERSION" ]]; then
  echo "Expected esp32:esp32 core ${EXPECTED_CORE_VERSION}; found ${core_version:-missing}." >&2
  exit 1
fi
require_library "DHT20" "$EXPECTED_DHT20_VERSION"
if [[ "$mode" != "--display-off" ]]; then
  require_library "TFT_eSPI" "$EXPECTED_TFT_VERSION"
fi

tmp_root="$(mktemp -d "${TMPDIR:-/tmp}/cs147-firmware.XXXXXX")"
trap 'rm -rf "$tmp_root"' EXIT
sketch_dir="$tmp_root/firmware_build"
mkdir -p "$sketch_dir"
cp "$SCRIPT_DIR/src/main.cpp" "$sketch_dir/main.cpp"
cp "$SCRIPT_DIR/include/config.h" "$sketch_dir/config.h"
touch "$sketch_dir/firmware_build.ino"

output_root="$SCRIPT_DIR/build"
mkdir -p "$output_root"
chmod 700 "$output_root"
run_stamp="$(date -u +%Y%m%dT%H%M%SZ)"

config_state="ready"
if grep -Eq 'YOUR_WIFI_(SSID|PASSWORD)_HERE|YOUR_SERVER_HOST_OR_IP_HERE' \
    "$sketch_dir/config.h"; then
  config_state="placeholder"
  echo "WARNING: deployment config still has placeholders; resulting image will disable networking." >&2
fi

build_variant() {
  local variant="$1"
  local display_value="$2"
  local extra_flags
  if [[ "$display_value" == "1" ]]; then
    extra_flags="-DENABLE_DISPLAY=1 -DUSER_SETUP_LOADED=1 -DST7789_DRIVER -DTFT_WIDTH=135 -DTFT_HEIGHT=240 -DCGRAM_OFFSET -DTFT_MOSI=19 -DTFT_SCLK=18 -DTFT_CS=5 -DTFT_DC=16 -DTFT_RST=23 -DTFT_BL=4 -DTFT_BACKLIGHT_ON=HIGH -DLOAD_GLCD=1 -DLOAD_FONT2=1 -DLOAD_FONT4=1 -DLOAD_FONT6=1 -DLOAD_FONT7=1 -DLOAD_FONT8=1 -DSMOOTH_FONT -DSPI_FREQUENCY=27000000"
  else
    extra_flags="-DENABLE_DISPLAY=0"
  fi

  local source_hash config_hash build_script_hash build_id output_dir manifest
  source_hash="$(shasum -a 256 "$sketch_dir/main.cpp" | awk '{print $1}')"
  config_hash="$(shasum -a 256 "$sketch_dir/config.h" | awk '{print $1}')"
  build_script_hash="$(shasum -a 256 "$SCRIPT_DIR/build.sh" | awk '{print $1}')"
  build_id="$(printf '%s\n' "$source_hash" "$config_hash" "$variant" \
    "$cli_version" "$core_version" "$EXPECTED_DHT20_VERSION" \
    "$EXPECTED_TFT_VERSION" "$build_script_hash" "$extra_flags" \
    | shasum -a 256 | cut -c1-16)"
  printf '#define FIRMWARE_BUILD_ID "%s"\n' "$build_id" > "$sketch_dir/build_info.h"

  output_dir="$output_root/${run_stamp}-${variant}-${build_id}"
  mkdir -m 700 "$output_dir"
  echo "Building ${variant} (build ID ${build_id})..."
  arduino-cli compile --clean --fqbn "$FQBN" \
    --build-property "compiler.cpp.extra_flags=${extra_flags}" \
    --output-dir "$output_dir" "$sketch_dir"

  manifest="$output_dir/build-manifest.txt"
  {
    printf 'firmware_build_id=%s\n' "$build_id"
    printf 'variant=%s\n' "$variant"
    printf 'deployment_config=%s\n' "$config_state"
    printf 'source_sha256=%s\n' "$source_hash"
    printf 'config_sha256=%s\n' "$config_hash"
    printf 'build_script_sha256=%s\n' "$build_script_hash"
    printf 'fqbn=%s\n' "$FQBN"
    printf 'arduino_cli=%s\n' "$cli_version"
    printf 'esp32_core=%s\n' "$core_version"
    printf 'DHT20=%s\n' "$EXPECTED_DHT20_VERSION"
    printf 'TFT_eSPI=%s\n' "$EXPECTED_TFT_VERSION"
    printf 'compiler_cpp_extra_flags=%s\n' "$extra_flags"
    for artifact in "$output_dir"/*.bin "$output_dir"/*.elf; do
      printf 'artifact_sha256=%s %s\n' \
        "$(shasum -a 256 "$artifact" | awk '{print $1}')" \
        "$(basename "$artifact")"
    done
  } > "$manifest"
  chmod 600 "$manifest"
  echo "Artifacts: $output_dir"
}

case "$mode" in
  --display-on) build_variant "display-on" 1 ;;
  --display-off) build_variant "display-off" 0 ;;
  --both)
    build_variant "display-on" 1
    build_variant "display-off" 0
    ;;
esac
