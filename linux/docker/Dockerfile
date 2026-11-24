# The CardputerZero build environment: Debian trixie arm64, the OS the
# device runs, so the binary links against the same glibc. Runs natively on
# Apple Silicon and GitHub's ubuntu-24.04-arm runners; no cross toolchain.
# Used by scripts/build-cardputer-zero.sh.
FROM debian:trixie

RUN apt-get update \
 && apt-get install -y --no-install-recommends \
      build-essential ca-certificates cmake ninja-build pkg-config file \
      libpulse-dev libexpat1-dev libcjson-dev libsdl2-dev python3 \
 && rm -rf /var/lib/apt/lists/*
