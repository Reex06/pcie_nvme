# 基于 QEMU 的 ARM64 PCIe/NVMe 驱动开发

从零实现一个 NVMe SSD 块设备驱动，完整打通 **PCIe → NVMe 控制器 → blk-mq 块设备** 全链路，用户态可用 `dd`/`fio` 直接读写 `/dev/nvme0n1`。

- **平台**：QEMU `virt` 机器 (arm64) + Generic PCIe Host (gpex) + GICv3 ITS
- **设备**：NVMe SSD（PCI 厂商/设备号 `1b36:0010`，QEMU 模拟）
- **驱动**：out-of-tree 内核模块 `driver/pcie_nvme.c`（自写，非内核 `drivers/nvme/host/pci.c`）
- **内核**：Linux 6.5.0（已关掉 `CONFIG_BLK_DEV_NVME`，让自写驱动独占设备）

---

## 一、为什么做这个项目（选题考量）

原目标板 i.MX6ULL（32 位 ARM）**硬件上没有 PCIe 控制器**，无法开展 PCIe 驱动实践。切换为 QEMU arm64 `virt` 机器后，NVMe 是覆盖 PCIe 全链路最完整、含金量最高的设备：

| PCIe/NVMe 知识点 | 项目覆盖 |
|---|---|
| PCIe 枚举 / ECAM / BDF | `pci_register_driver` + probe 匹配 |
| BAR 空间 / MMIO | BAR0 映射 + 控制器寄存器读写 |
| DMA / 总线地址 | `dma_map_sg` + PRP 物理页 |
| 命令队列 / doorbell | Admin/I/O SQ/CQ + tail/head doorbell |
| 中断 | MSI-X + GICv3 ITS |
| 块设备栈 | blk-mq 多队列 + gendisk |

五个阶段（含 5b 健壮性增强）由浅入深，每阶段独立可验证，全程对标内核 `drivers/nvme/host/pci.c`。

---

## 二、环境搭建

### 2.1 交叉工具链与内核
```bash
# 交叉编译器（系统自带）
aarch64-linux-gnu-gcc --version   # 11.4.0

# 内核切 arm64 并编译（qemu/linux-6.5）
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- defconfig
./scripts/config --file .config -e PCI_HOST_GENERIC -e GIC_V3_ITS -e PCI_MSI \
    -e DEVTMPFS -e DEVTMPFS_MOUNT -d BLK_DEV_NVME -d NVME_CORE
make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- Image -j4
```
> 关掉 `BLK_DEV_NVME`：一个 PCIe 设备只能被一个驱动 claim，目标是从零实现，必须独占。

### 2.2 rootfs（busybox）
```bash
# qemu/rootfs 目录，busybox 静态链接为 /init
cp busybox-1.36.1/busybox rootfs/bin/busybox
ln -sf bin/busybox rootfs/init
# 打包
cd qemu/rootfs && find . | cpio -o -H newc | gzip > ../rootfs_arm64.cpio.gz
```

### 2.3 QEMU 启动参数（start_qemu_virt.sh）
```bash
qemu-system-aarch64 \
  -M virt,gic-version=3 \      # gic-version=3 是 MSI-X 的前提（见坑 8）
  -cpu cortex-a53 -m 512M -smp 2 \
  -kernel Image -initrd rootfs_arm64.cpio.gz \
  -append "console=ttyAMA0 rdinit=/init" \
  -device nvme,serial=deadbeef,drive=nvme0 \
  -drive file=nvme.img,if=none,id=nvme0,format=raw \
  -nographic
```

---

## 三、五阶段开发路线

### 阶段1：PCIe 枚举 + BAR0 映射 + 读寄存器
- `pci_register_driver` 匹配 `1b36:0010`，probe 里读 BDF/class
- `pci_enable_device_mem` + `pci_set_master` → `devm_ioremap` 映射 BAR0
- 读 `CAP`(0x00)/`VS`(0x08)/`CSTS`(0x1c)
- **验证**：BDF=`0000:00:02.0`，BAR0=`0x8000004000`/16KB，MQES=2048，NVMe 1.4

### 阶段2：控制器初始化 + Admin Queue + Identify
- 写 `AQA`/`ASQ`/`ACQ` → `CC.EN`(0x00460001) → 等 `CSTS.RDY=1`
- 提交 Identify Controller(0x06, CNS=1) → 敲 SQ tail doorbell → 轮询 CQ phase bit → 敲 CQ head doorbell
- **验证**：`CC=0x00460001, CSTS.RDY=1`；Identify 返回 VID=`0x1b36`、MN=`QEMU NVMe Ctrl`、SN=`deadbeef`

### 阶段3：I/O 队列 + blk-mq + Write/Read + PRP
- Admin 命令创建 I/O CQ/SQ（opcode 0x05/0x01），Identify Namespace 读 NSZE/LBAF
- `blk_mq_alloc_disk` + gendisk + `queue_rq`，构造 Write/Read 命令
- **PRP 数据路径**：单页走 PRP1；多页 PRP1 + PRP2 指向 `dma_pool` 的 PRP list 页（每页 512 个 8 字节条目）
- 完成路径暂用同步轮询（`BLK_MQ_F_BLOCKING` + mutex），先隔离数据正确性与中断两个正交问题
- **验证**：`/dev/nvme0n1` 注册成功（major 259，524288 LBA，256MB）；dd 单页 4K 与多页 64K 写读回 `cmp` 均 MATCH

### 阶段4：MSI-X 中断 + 异步完成
- `pci_alloc_irq_vectors(pdev, 1, nr, PCI_IRQ_MSIX | PCI_IRQ_MSI)` + `request_irq`
- Create I/O CQ 置 `IEN=1 + IV=0`，控制器写完 CQE 后触发中断
- 中断上半部收割 CQ → `command_id(=rq->tag)` 经 `blk_mq_tag_to_rq` 反查 request → `blk_mq_complete_request` 触发软中断 → `.complete` 回调 unmap + `blk_mq_end_request`
- **验证**：`/proc/interrupts` 中断计数 dd 后 2→10（证明走中断非轮询）；dd 4K/64K MATCH；`pdev->msix_enabled=1`

### 阶段5：多队列扩展（每 CPU 一队列，对标真实 NVMe 性能模型）
- `nr_queues = min(num_online_cpus(), MQES+1)`，每队列独立 SQ/CQ + 独立 MSI-X 向量（`IV=qid-1`）+ 独立 hctx
- `pci_alloc_irq_vectors` 前移到 `nvme_setup_disk` 之前：先确定实际向量数，再按最终 `nr_queues` 建 tag_set
- probe 循环 `for (i=0; i<nr_queues; i++) nvme_alloc_io_queue(dev, &queues[i], i+1)`；多 hctx 下 `command_id=rq->tag` 仍 per-hctx 唯一
- **验证**：`-smp 2` 下 2 个 `nvme-io` 中断行（irq=23/24）；dd 4K/64K MATCH；两队列中断计数均增长（中断分发到多队列）

### 阶段5b：I/O 命令超时处理（设备不响应时 request 不永久挂起）
- `tag_set.timeout = io_timeout * HZ` + `nvme_mq_ops.timeout = nvme_timeout`，blk-mq 到点自动回调
- `nvme_timeout` 先主动 `nvme_process_cq(q)` 收割 CQ（覆盖「中断丢失」场景），再判断 request 是否已结束
- 未结束则打印卡住的 tag/opcode、返回 `BLK_EH_RESET_TIMER` 重置计时器继续等（进阶：发 Abort 命令或复位控制器）
- 模块参数 `irq_loss=1` 模拟丢中断，验证超时回调能回收 request、dd 不 hang
- **验证**：`insmod pcie_nvme.ko io_timeout=5 irq_loss=1` 后每条命令 5s 超时 → 回调 poll CQ 恢复，dd 写读 cmp MATCH，rmmod 干净

---

## 四、核心问题排查记录（面试重点）

> 每个问题都遵循「现象 → 根因 → 解决」链路，可追溯。

| # | 问题现象 | 根因分析 | 解决方案 |
|---|---|---|---|
| 1 | arm64 模块 modpost 报 `undefined symbol: arm64_use_ng_mappings / ioremap_prot` | arm64 的 `ioremap()` 是 inline 宏，展开引用两个**未 EXPORT_SYMBOL** 的符号，out-of-tree 模块无法引用 | 改用 `devm_ioremap()`（已导出 + devres 自动释放） |
| 2 | Admin 命令提交后 CQ 永不完成，等待超时 -110；日志 `enabling device (0000 -> 0002)` | `pci_enable_device_mem` 只置 Command 寄存器 **MEM 位(bit1)**，没置 **Bus Master 位(bit2)**；NVMe 靠 DMA 读 SQ/写 CQ，缺 Bus Master 设备读不到 SQ | 之后补 `pci_set_master(pdev)` |
| 3 | Identify 的 NN 读到 17510(0x00004466)，明显异常 | NN 在 **offset 516** 不是 512；offset 512 是 `sqes+cqes+maxcmd` = 0x00004466 = 17510 | 读 offset 516（对照 `include/linux/nvme.h` 的 `struct nvme_id_ctrl`） |
| 4 | Identify 返回 NN=256，但实际只有 1 个 namespace | QEMU 6.2 用 `num_namespaces=NVME_MAX_NAMESPACES` 填 NN，非实际 namespace 数 | QEMU 实现行为，非驱动 bug |
| 5 | `add_disk` 返回 -EINVAL，WARNING `block/genhd.c:438 WARN_ON(disk->minors)` | `disk->major=0`（动态分配 ext devt）时 `disk->minors` **必须为 0** | 去掉 `disk->minors=1` |
| 6 | 编译报 `linux/genhd.h not found`、`GENHD_FL_EXT_DEVT undeclared` | Linux 6.5 把 gendisk 定义并入 `blkdev.h`，移除 `GENHD_FL_EXT_DEVT` | 删 include 与 flags 行 |
| 7 | 阶段4 首次验证 **kernel panic**，Oops `pc=0x0`，`lr=blk_complete_reqs` | `blk_mq_complete_request` 异步完成走软中断 `blk_done_softirq → blk_complete_reqs → mq_ops->complete`，未实现该回调则跳 NULL 指针 | 实现 `.complete` 回调 `nvme_mq_complete`（unmap + `blk_mq_end_request`） |
| 8 | `/proc/interrupts` 显示 `MSI` 而非 MSI-X，日志出现 GICv2m | QEMU virt 默认给 cortex-a53 配 **GICv2**，GICv2m 只支持 MSI；MSI-X 需 GICv3 ITS | `-M virt,gic-version=3`，之后 `ITS-MSI` + `msix_enabled=1` |
| 9 | 超时回调里 `blk_mq_request_completed(rq)` 恒返回 false，误判「仍卡住」 | 本驱动多队列下 `blk_mq_complete_request` 走**同步完成**路径（非 IPI），`blk_mq_end_request → blk_mq_free_request` 把 state 重置为 `MQ_RQ_IDLE` 而非 `MQ_RQ_COMPLETE` | 改用 `blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT` 判断「已不在途」（COMPLETE/IDLE 都算） |

### 关键设计决策

- **阶段3 同步轮询、阶段4 再切中断**：把「数据正确性（PRP/blk-mq）」与「中断」两个正交问题分开调试，出错定位快。
- **unmap 放 `.complete` 回调（软中断）而非中断上半部**：上半部只记 status + `blk_mq_complete_request`，对标内核 nvme pci.c，缩短硬中断占用。
- **command_id 直接复用 `rq->tag`**：单 hctx 下 tag 唯一，完成时 `blk_mq_tag_to_rq` 反查，无需额外映射表。
- **阶段5 每队列独立 MSI-X 向量（`IV=qid-1`）**：CQ 写满后触发绑定该队列的中断，中断处理 data 指向该队列，天然隔离；若所有 CQ 都绑 `IV=0`，则多队列共用一个中断，其余队列 CQ 无人收割 → IO 挂起。
- **`pci_alloc_irq_vectors` 前移到 `nvme_setup_disk` 前**：先拿到实际可用向量数再建 tag_set，避免 `nr_hw_queues` 超过实际中断数导致 hctx 悬空。
- **多 hctx 下 `command_id=rq->tag` 仍 per-hctx 唯一**：CQE 不带 hctx 信息，靠「哪个 CQ 收到 CQE」定位队列，`blk_mq_tag_to_rq(q->tags, tag)` 用该队列的 tags 反查。
- **超时回调判断用 `blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT` 而非 `blk_mq_request_completed()`**：见坑 9，同步完成路径下 state 被重置为 IDLE。

---

## 五、完整技术链路

```
用户态 dd/fio
    │ bio
    ▼
blk-mq (tag_set / hctx / queue_rq)
    │ rq->tag = command_id
    ▼
NVMe Write/Read 命令 + PRP(PRP1/PRP2)  ← dma_map_sg 得总线地址
    │ 写 SQ tail doorbell
    ▼
QEMU NVMe 控制器 (DMA 读 SQ/写 CQ)
    │ CQE phase bit + MSI-X 中断
    ▼
nvme_irq (上半部)：收割 CQ → blk_mq_tag_to_rq → blk_mq_complete_request
    │ 软中断
    ▼
nvme_mq_complete (.complete 回调)：unmap + blk_mq_end_request
    │
    ▼
bio end_io 唤醒等待进程
```

---

## 六、运行与验证

```bash
# 1. 编译驱动
cd driver && make                # 产出 pcie_nvme.ko

# 2. 打包进 rootfs
cp pcie_nvme.ko ../qemu/rootfs/
cd ../qemu/rootfs && find . | cpio -o -H newc | gzip > ../rootfs_arm64.cpio.gz

# 3. 启动 QEMU（串口交互）
cd ../.. && ./start_qemu_virt.sh

# 4. 加载并验证
insmod /pcie_nvme.ko
ls -l /dev/nvme0n1
dd if=/dev/urandom of=/tmp/src bs=4096 count=16
dd if=/tmp/src of=/dev/nvme0n1 bs=4096 count=16 seek=16
sync
dd if=/dev/nvme0n1 of=/tmp/dst bs=4096 count=16 skip=16
cmp /tmp/src /tmp/dst && echo MATCH
grep nvme-io /proc/interrupts    # 观察中断计数增长
```
