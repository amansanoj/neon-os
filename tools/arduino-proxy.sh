# Sourced by setup.sh. Arduino CLI treats a present-but-empty proxy as an
# explicit proxy at :0, so leave its override UNSET on direct-network runners.
# Preserve explicit CLI proxy configuration; otherwise reuse the session proxy.
NEON_DOWNLOAD_PROXY="${ARDUINO_NETWORK_PROXY:-${HTTPS_PROXY:-${https_proxy:-${HTTP_PROXY:-${http_proxy:-}}}}}"
if [[ -n "$NEON_DOWNLOAD_PROXY" ]]; then
  export ARDUINO_NETWORK_PROXY="$NEON_DOWNLOAD_PROXY"
else
  unset ARDUINO_NETWORK_PROXY
fi
unset NEON_DOWNLOAD_PROXY
