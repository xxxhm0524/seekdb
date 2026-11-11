#!/usr/bin/env bash
set -euo pipefail

# Forward only pre-existing runner proxy settings as Docker's predefined
# proxy arguments. Values are not echoed or stored in the final image.
mvd_proxy_args=()
for mvd_lower in http_proxy https_proxy no_proxy all_proxy; do
  mvd_upper=${mvd_lower^^}
  mvd_value=${!mvd_lower:-${!mvd_upper:-}}
  if [[ -n "$mvd_value" ]]; then
    export "$mvd_lower=$mvd_value"
    mvd_proxy_args+=(--build-arg "$mvd_lower")
  fi
done
exec docker build "${mvd_proxy_args[@]}" "$@"
