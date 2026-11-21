#!/usr/bin/env bash
# One-time setup of the PS5Build WSL distro (Debian, run as root).
# Builds the PS5 RADV Vulkan driver (release archive) and the payload SDK fork it pins.
#
#   wsl -d PS5Build -u root -- bash /mnt/e/Pokopia-PS4/ps5/scripts/setup-build-host.sh
#
# Layout (on the distro's own disk; /mnt/e is much slower):
#   ~/ps5/PS5_Vulkan       driver repo (tools/build-radv.sh)
#   ~/ps5/PS5_Mesa         Mesa fork with the PS5 winsys
#   ~/ps5/PS5_PayloadSDK   payload SDK fork (platform heap, direct memory, libc gaps)
#   ~/ps5/PS5_Vulkan/.deps/native/ps5-payload-sdk   the SDK titles build with
#   ~/ps5/PS5_Vulkan/.deps/native/radv-release      libvulkan_radeon.ps5.a
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive

step() { printf '\n==> %s\n' "$*"; }

step "apt packages"
apt-get update -qq
apt-get install -y -qq \
    build-essential git curl wget unzip rsync tar xz-utils pkgconf cmake ninja-build \
    python3 python3-pip python3-venv python3-mako python3-markupsafe python3-yaml python3-packaging \
    clang lld llvm llvm-dev libclang-dev libclang-cpp-dev libclc-19-dev \
    llvm-spirv-19 libllvmspirvlib-19-dev spirv-tools glslang-tools \
    bison flex zlib1g-dev libzstd-dev libexpat1-dev libelf-dev ccache > /dev/null

# Mesa needs a newer meson than some distros ship; use pip in a venv.
step "meson"
python3 -m venv /opt/meson-venv
/opt/meson-venv/bin/pip install -q --upgrade meson
ln -sf /opt/meson-venv/bin/meson /usr/local/bin/meson
meson --version

mkdir -p ~/ps5 && cd ~/ps5
clone() { [[ -d $2/.git ]] || git clone --filter=blob:none "https://github.com/$1" "$2"; git -C "$2" fetch -q --all; }

step "repos"
clone mihawk-99/PS5_Vulkan PS5_Vulkan
clone mihawk-99/PS5_Mesa PS5_Mesa
clone mihawk-99/PS5_PayloadSDK PS5_PayloadSDK

cd ~/ps5/PS5_Vulkan
step "payload SDK fork + zlib (tools/setup-native-dependencies.sh)"
bash tools/setup-native-dependencies.sh

step "RADV release archive (tools/build-radv.sh release)"
bash tools/build-radv.sh release

step "done"
cat .deps/native/radv-release/PROVENANCE.txt
ls -la .deps/native/ps5-payload-sdk/bin | head -30
