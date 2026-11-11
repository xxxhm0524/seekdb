#!/usr/bin/env bash
set -euo pipefail

# Probe the actual locked image build, including HTTPS/possibly proxied APT.
# Direct DNS alone is not a sufficient test in a proxy-only environment.
# This selection applies only to image construction. Database test sessions
# retain their protocol-mandated --network=none runtime configuration.
mvd_script_dir=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
cd -- "$mvd_script_dir/.."
python3 - <<'PY'
import os, urllib.parse, urllib.request
print('Runner proxy variables present:', ', '.join(k for k in
    ('http_proxy','https_proxy','no_proxy','all_proxy','HTTP_PROXY','HTTPS_PROXY','NO_PROXY','ALL_PROXY')
    if os.environ.get(k)) or 'none', flush=True)
for url in ('https://snapshot.ubuntu.com/ubuntu/20261110T160000Z/dists/noble/InRelease',
            'https://archive.ubuntu.com/ubuntu/dists/noble/InRelease'):
    host = urllib.parse.urlparse(url).hostname
    try:
        with urllib.request.urlopen(url, timeout=12) as response:
            response.read(64)
            print('Host HTTPS:', host, response.status, flush=True)
    except Exception as error:
        print('Host HTTPS:', host, type(error).__name__,
              type(getattr(error, 'reason', error)).__name__, getattr(error, 'code', None), flush=True)
PY
for mvd_domain in snapshot.ubuntu.com archive.ubuntu.com; do
  printf 'Host resolver: %s\n' "$mvd_domain"
  timeout 8 getent ahostsv4 "$mvd_domain" || true
done
mvd_build_network=''
for mvd_mode in default host; do
  printf 'Locked compiler-image probe: network=%s\n' "$mvd_mode"
  if bash "$mvd_script_dir/docker_build_with_proxy.sh" --progress=plain --network="$mvd_mode" --target build .; then
    mvd_build_network=$mvd_mode
    break
  fi
done
if [[ -z "$mvd_build_network" ]]; then
  printf 'The locked compiler image failed in both build network modes; inspect the transport/build logs.\n' >&2
  exit 1
fi
printf 'MVD_BUILD_NETWORK=%s\n' "$mvd_build_network" >> "$GITHUB_ENV"
printf 'Selected Docker build network: %s\n' "$mvd_build_network"
