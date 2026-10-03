#!/usr/bin/env bash
# Boot-proofs a grpc-poppler image: a green build is not "done" until the
# artifact starts under the flags it ships with. Hermetic (no documents, no
# network beyond the docker socket), so it runs in CI and before any push.
#
#   1. closure: every shared library the binary links resolves inside the
#      image. The runtime base carries no ldd and may carry no shell, so the
#      dynamic loader answers directly: LD_TRACE_LOADED_OBJECTS=1 makes it
#      print the closure and exit, which is all ldd does.
#   2. boot: the server reaches its own "listening on" line under the
#      hardened run flags (read-only rootfs, no capabilities), and runs as
#      uid 65532. The prebuilt fontconfig cache means a read-only rootfs is
#      enough: nothing is written at startup.
#   3. licenses: the image is GPL object code, so it carries the license
#      texts (this repository's and poppler's) and names its source.
set -euo pipefail

usage() {
  echo "Usage: $0 IMAGE" >&2
  exit 64
}
[[ $# -eq 1 ]] || usage
image=$1
binary=/usr/local/bin/grpc_poppler
container="grpc-poppler-smoke-$$"

cleanup() {
  docker rm -f "$container" >/dev/null 2>&1 || true
}

# Polls the container log for a line until it appears or the deadline passes.
wait_for_log() {
  local pattern=$1 deadline=$2
  for _ in $(seq 1 "$deadline"); do
    if docker logs "$container" 2>&1 | grep -q "$pattern"; then
      return 0
    fi
    if [[ "$(docker inspect -f '{{.State.Running}}' "$container" 2>/dev/null)" != "true" ]]; then
      break
    fi
    sleep 1
  done
  echo "container did not log '$pattern'; logs:" >&2
  docker logs "$container" >&2 || true
  return 1
}

echo "== smoke: library closure of the shipped binary"
trace=$(docker run --rm --entrypoint "$binary" -e LD_TRACE_LOADED_OBJECTS=1 "$image" 2>&1)
echo "$trace"
if grep -q "not found" <<<"$trace"; then
  echo "unresolved shared libraries or symbol versions in $image" >&2
  exit 1
fi

echo "== smoke: boot to listening under the hardened run flags"
trap cleanup EXIT
docker run -d --name "$container" \
  --read-only --cap-drop ALL --security-opt no-new-privileges:true \
  "$image" >/dev/null
wait_for_log "grpc-poppler listening on" 60
# The prebuilt cache must be accepted as it is: fontconfig complaining
# about cache directories means it rescans the fonts instead. Poppler
# touches fontconfig only when a document needs a substitute font, so this
# catches complaints raised at startup; the build keeps the fonts and the
# cache in one staged tree so they cannot disagree later.
if docker logs "$container" 2>&1 | grep -q "Fontconfig error"; then
  echo "fontconfig rejected the prebuilt cache; logs:" >&2
  docker logs "$container" >&2 || true
  exit 1
fi

processes=$(docker top "$container" -o uid,pid,args | tail -n +2)
echo "$processes"
foreign_uid=$(awk '$1 != 65532' <<<"$processes" || true)
if [[ -n "$foreign_uid" ]]; then
  echo "a process is not running as uid 65532" >&2
  exit 1
fi

echo "== smoke: license texts and source label"
for path in /usr/share/doc/grpc-poppler/LICENSE /usr/share/doc/poppler/COPYING \
  /usr/share/doc/poppler/COPYING3; do
  if ! docker cp "$container:$path" - >/dev/null 2>&1; then
    echo "$image does not carry $path" >&2
    exit 1
  fi
done
source_label=$(docker image inspect \
  -f '{{index .Config.Labels "org.opencontainers.image.source"}}' "$image")
if [[ -z "$source_label" || "$source_label" == "<no value>" ]]; then
  echo "$image has no org.opencontainers.image.source label" >&2
  exit 1
fi
echo "source: $source_label"

echo "smoke-test: OK ($image)"
