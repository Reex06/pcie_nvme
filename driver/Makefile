# pcie_nvme 模块 out-of-tree 编译
# 用法:
#   make         交叉编译出 pcie_nvme.ko
#   make clean   清理
KERNEL_DIR := /home/cxr/桌面/pcie_driver/qemu/linux-6.5
ARCH := arm64
CROSS_COMPILE := aarch64-linux-gnu-

obj-m += pcie_nvme.o

.PHONY: all clean

all:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_DIR) M=$(PWD) modules

clean:
	$(MAKE) ARCH=$(ARCH) CROSS_COMPILE=$(CROSS_COMPILE) -C $(KERNEL_DIR) M=$(PWD) clean
