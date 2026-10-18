#!/bin/bash
# start_qemu_virt.sh - QEMU ARM64 virt + NVMe 驱动开发环境启动脚本
# 机器: QEMU virt (arm64) + Generic PCIe host (gpex)
# 设备: NVMe SSD (-device nvme)
# 退出 QEMU: Ctrl + a 然后按 x
set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
QEMU_DIR="${SCRIPT_DIR}/../qemu"

QEMU_BIN="/usr/bin/qemu-system-aarch64"
KERNEL="${QEMU_DIR}/linux-6.5/arch/arm64/boot/Image"
INITRD="${QEMU_DIR}/rootfs_arm64.cpio.gz"
NVME_IMG="${QEMU_DIR}/nvme.img"

echo "====================================="
echo "  QEMU ARM64 virt + NVMe 开发环境"
echo "  退出 QEMU: Ctrl + a 然后按 x"
echo "====================================="

${QEMU_BIN} \
  -M virt,gic-version=3 \
  -cpu cortex-a53 \
  -m 512M \
  -smp 2 \
  -kernel "${KERNEL}" \
  -initrd "${INITRD}" \
  -append "console=ttyAMA0 rdinit=/init" \
  -device nvme,serial=deadbeef,drive=nvme0 \
  -drive file="${NVME_IMG}",if=none,id=nvme0,format=raw \
  -nographic
