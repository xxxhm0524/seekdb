#!/usr/bin/env bash
set -euo pipefail

# Probe the actual BuildKit RUN network, not just docker run or host DNS.
# This selection applies only to image construction. Database test sessions
# retain their protocol-mandated --network=none runtime configuration.
mvd_probe_dir=$(mktemp -d)
mvd_base='ubuntu:24.04@sha256:fe81560990fae86dc4743202b2cb4d3df98d0bcf949173cecef15ce1127ef852'
for mvd_domain in snapshot.ubuntu.com archive.ubuntu.com; do
  printf 'Host resolver: %s\n' "$mvd_domain"
  timeout 8 getent ahostsv4 "$mvd_domain" || true
done
printf 'FROM %s\nRUN timeout 8 getent ahostsv4 snapshot.ubuntu.com && timeout 8 getent ahostsv4 archive.ubuntu.com\n' \
  "$mvd_base" > "$mvd_probe_dir/Dockerfile"

mvd_build_network=''
for mvd_mode in default host; do
  printf 'BuildKit DNS probe: network=%s\n' "$mvd_mode"
  if docker build --no-cache --progress=plain --network="$mvd_mode" "$mvd_probe_dir"; then
    mvd_build_network=$mvd_mode
    break
  fi
done
if [[ -z "$mvd_build_network" ]]; then
  printf 'Both BuildKit network modes failed their Ubuntu DNS probe.\n' >&2
  exit 1
fi
printf 'MVD_BUILD_NETWORK=%s\n' "$mvd_build_network" >> "$GITHUB_ENV"
printf 'Selected Docker build network: %s\n' "$mvd_build_network"
