#!/usr/bin/env bash
set -euo pipefail

container_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$container_dir/.." && pwd)"
rebit_dir="$(cd "$repo_dir/.." && pwd)"

runner_src="$container_dir/runner/lan_room_runner.c"
build_dir="$container_dir/build"
runner_bin="$build_dir/melonds-lan-room-runner"

libretro_include="${LIBRETRO_INCLUDE:-$rebit_dir/cloud-game/pkg/worker/caged/libretro/nanoarch}"
core_path="${CORE_PATH:-$rebit_dir/cloud-game/assets/cores/melondsds_libretro.so}"
rom_path="${ROM_PATH:-$rebit_dir/cloud-game/assets/games/nds/blocksds-local-multiplayer.nds}"
runtime_dir="${RUNTIME_DIR:-$container_dir/runtime/lan2}"
players="${PLAYERS:-2}"
frames="${FRAMES:-900}"

if [[ ! -f "$libretro_include/libretro.h" ]]; then
  echo "libretro.h not found at $libretro_include" >&2
  echo "Set LIBRETRO_INCLUDE=/path/to/libretro/include" >&2
  exit 1
fi

mkdir -p "$build_dir" "$runtime_dir"

cc="${CC:-gcc}"

"$cc" -std=c17 -O2 -Wall -Wextra -Wno-format-truncation \
  -I "$libretro_include" \
  "$runner_src" \
  -ldl \
  -o "$runner_bin"

exec "$runner_bin" \
  --core "$core_path" \
  --rom "$rom_path" \
  --runtime "$runtime_dir" \
  --players "$players" \
  --frames "$frames"
