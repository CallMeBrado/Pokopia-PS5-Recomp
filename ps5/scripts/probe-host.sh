#!/usr/bin/env bash
# Report what the WSL build host has for the PS5 toolchain.
clang --version | head -1
ld.lld --version
whoami
sudo -n true 2>/dev/null && echo SUDO_OK || echo SUDO_NEEDS_PASSWORD
ls /opt
echo "--- ps5-payload-dev/sdk latest release"
curl -s https://api.github.com/repos/ps5-payload-dev/sdk/releases/latest | grep -E '"(tag_name|browser_download_url)"'
echo "--- mihawk-99/PS5_Vulkan latest release"
curl -s https://api.github.com/repos/mihawk-99/PS5_Vulkan/releases/latest | grep -E '"(tag_name|browser_download_url)"'
echo "--- mihawk-99/PS5_PayloadSDK latest release"
curl -s https://api.github.com/repos/mihawk-99/PS5_PayloadSDK/releases/latest | grep -E '"(tag_name|browser_download_url)"'
