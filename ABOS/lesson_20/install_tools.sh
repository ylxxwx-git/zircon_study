#!/usr/bin/env bash
# =========================================================================
# install_tools.sh - Install AArch64 Cross-Compiler, Binutils, QEMU & Python3
#
# Supports:
#   - Debian / Ubuntu / gLinux (apt-get)
#   - Fedora / RHEL / CentOS   (dnf / yum)
#   - Arch Linux               (pacman)
#   - macOS                    (Homebrew)
# =========================================================================
set -e

echo "=================================================================="
echo "  ABOS Toolchain & QEMU Installer"
echo "=================================================================="

if command -v apt-get >/dev/null 2>&1; then
    echo "[*] Detected Debian/Ubuntu/gLinux (apt-get). Installing packages..."
    sudo apt-get update
    sudo apt-get install -y \
        build-essential \
        gcc-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        qemu-system-arm \
        python3
elif command -v dnf >/dev/null 2>&1; then
    echo "[*] Detected Fedora/RHEL (dnf). Installing packages..."
    sudo dnf install -y \
        make \
        gcc-aarch64-linux-gnu \
        binutils-aarch64-linux-gnu \
        qemu-system-aarch64 \
        python3
elif command -v pacman >/dev/null 2>&1; then
    echo "[*] Detected Arch Linux (pacman). Installing packages..."
    sudo pacman -Sy --noconfirm \
        base-devel \
        aarch64-linux-gnu-gcc \
        aarch64-linux-gnu-binutils \
        qemu-system-aarch64 \
        python
elif command -v brew >/dev/null 2>&1; then
    echo "[*] Detected macOS (Homebrew). Installing packages..."
    brew install aarch64-elf-gcc qemu python3 make
else
    echo "[!] Unsupported package manager. Please manually install:"
    echo "    - aarch64-linux-gnu-gcc (or aarch64-elf-gcc)"
    echo "    - aarch64-linux-gnu-binutils"
    echo "    - qemu-system-aarch64"
    echo "    - python3 & make"
    exit 1
fi

echo ""
echo "=================================================================="
echo "  Verifying Installed Tools:"
echo "=================================================================="
if command -v aarch64-linux-gnu-gcc >/dev/null 2>&1; then
    aarch64-linux-gnu-gcc --version | head -n 1
elif command -v aarch64-elf-gcc >/dev/null 2>&1; then
    aarch64-elf-gcc --version | head -n 1
fi

if command -v qemu-system-aarch64 >/dev/null 2>&1; then
    qemu-system-aarch64 --version | head -n 1
fi

python3 --version
echo "[+] All ABOS build & simulation tools are ready! Run 'make run' now."
