#!/bin/sh
# Builds the peer bot (tests/peer_bot.cpp) against an older release, for lunashare_compat:
#   tools/build_old_peer.sh <tag, e.g. v1.8.5> <out dir> [extra cmake args, e.g. -DFETCHCONTENT_SOURCE_DIR_...=...]
# The release's own engine with today's bot around it (the bot uses only what every version since 1.8.4 has).
# Prints the bot's path at the end.
set -e
tag="$1"; out="$2"; shift 2 || { echo "usage: $0 <tag> <out dir> [cmake args]"; exit 2; }
repo="$(cd "$(dirname "$0")/.." && pwd)"
src="$out/src-$tag"
mkdir -p "$out"
if [ ! -d "$src" ]; then
    git -C "$repo" worktree add --detach "$src" "$tag" >/dev/null 2>&1 || git -C "$repo" archive "$tag" | (mkdir -p "$src" && tar -x -C "$src")
fi
cp "$repo/tests/peer_bot.cpp" "$src/tests/peer_bot.cpp"
if ! grep -q lunashare_peer_bot "$src/CMakeLists.txt"; then
    cat >> "$src/CMakeLists.txt" <<'EOF'
add_executable(lunashare_peer_bot tests/peer_bot.cpp)
target_link_libraries(lunashare_peer_bot PRIVATE lunashare_core)
EOF
fi
cmake -S "$src" -B "$out/build-$tag" -DCMAKE_BUILD_TYPE=Release -DLUNASHARE_BUILD_APP=OFF -DLUNASHARE_BUILD_TESTS=OFF \
    -DLUNASHARE_BUILD_SERVER=OFF -DLUNASHARE_BUILD_VOICE=OFF -DLUNASHARE_BUILD_VIDEO=OFF -DLUNASHARE_BUILD_PLAYER=OFF \
    -DLUNASHARE_BUILD_TORRENT=OFF -DLUNASHARE_BUILD_EMOJI=OFF "$@" >/dev/null
cmake --build "$out/build-$tag" --target lunashare_peer_bot -j4 >/dev/null
echo "$out/build-$tag/lunashare_peer_bot"
