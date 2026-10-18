# 学习路线图 + 源码精讲 + 必掌握知识库

> 目标：从「只会 C + 字符设备驱动」到「能对 1086 行 NVMe 驱动逐行讲清楚 + 扛住面试追问」。
> 使用方法：按阶段顺序推进，每个阶段有「验收标准」，过了再进下一阶段；源码精讲配合 `driver/pcie_nvme.c` 对照阅读。

---

## 零、现状诊断（先认清差距）

| 项目依赖的知识 | 你的现状 | 缺口程度 |
|---|---|---|
| 内核模块 / 注册 / probe | 已会（字符设备） | 无，直接复用 |
| 块设备子系统（bio/request/gendisk/blk-mq） | 未学（有正点原子视频） | 大，**最关键** |
| PCIe（枚举/配置空间/BAR/MMIO/MSI-X） | 未学（有韦东山视频） | 大 |
| NVMe 协议（SQ/CQ/doorbell/PRP） | 无前置 | 中，但结构清晰可速成 |
| DMA 与内存一致性 | 字符设备涉及少 | 中 |
| 中断（上半部/下半部/软中断） | 字符设备略接触 | 中 |
| QEMU / 内核编译 / rootfs | 听过、略知 | 环境类，照做即可 |

**结论**：你缺的不是 C 能力，是「块设备框架」和「PCIe 协议」两块硬骨头。这两块正是你手上两个视频教程覆盖的。所以学习路径是「块设备 → PCIe → NVMe → 串起来精读本项目」。

---

## 一、学习路线（7 阶段，由浅入深）

### 阶段 0：环境跑通 + QEMU 学到什么程度（1~3 天）

**定位先想清楚**：QEMU 对你（驱动/BSP 岗）是「开发调试环境」，不是学习目标。要学的是「会用 + 能讲清原理和差异」，**不要学** KVM/虚拟化加速原理、QEMU 设备模型开发（那是虚拟化岗的事）。

**启动前先扫盲：整机启动原理（第 1 天先读，消除"一头雾水"）**

1. **你的环境没有用 U-Boot**。真实板子（i.MX6ULL）启动链：ROM → U-Boot（初始化 DDR、读 SD 卡内核、传设备树）→ 内核 → 根文件系统。本项目 `-kernel Image` 让 QEMU 把内核**直接加载进内存并跳转**，U-Boot 整段被跳过——U-Boot 的职责全被 QEMU 用软件代劳。面试要能讲清这个区别。
2. **模拟的板子：QEMU "virt"，一张不存在的板子**。`-M virt` 不对应任何真实板卡，是 QEMU 官方设计的万能虚拟 ARM64 平台。`-cpu cortex-a53`（CPU）、`-m 512M`（内存=进程里一块大数组）、`-smp 2`（2 核）、`gic-version=3`（GICv3 中断控制器，MSI-X 前提）、机器内置 gpex PCIe 主机桥 + PL011 串口（ttyAMA0）、`-nographic`（串口当终端）。关键差异：真实板子的设备树是 `.dtb` 文件，virt 的设备树是 QEMU 启动时**现场生成**的，按 ARM64 启动协议放进 x0 寄存器传给内核。
3. **内核启动时序**：QEMU 把 Image 加载进 guest 内存 → 把 initrd 也加载进内存、地址写进设备树 → `-append` 变成内核 cmdline 写进设备树 → QEMU 内置 mini 固件设置 CPU 状态（EL2、MMU 关、x0=设备树地址）→ 跳内核入口 → 内核初始化、解析设备树 → 解压 initramfs 挂根文件系统 → 执行 `rdinit=/init`。
4. **根文件系统 = initramfs 内存压缩包**：`qemu/rootfs/` 目录（busybox + `init -> bin/busybox` 符号链接）打包 `find . | cpio -o -H newc | gzip > rootfs_arm64.cpio.gz`。两个后果：rootfs 在内存里，guest 改动重启即丢（所以改驱动要重新打包 cpio）；唯一持久的是宿主机文件 nvme.img。
5. **虚拟 NVMe 设备 = 前端/后端分离的纯软件模拟**，三层结构：

```
【宿主机文件】 nvme.img                     ← 数据真正落在这里（相当于空白硬盘）
      ▲ pread/pwrite
【QEMU 内】块后端 drive=nvme0               ← -drive file=nvme.img,if=none,id=nvme0
【QEMU 内】NVMe 控制器模拟（软件状态机）     ← -device nvme,drive=nvme0，源码 ../qemu/hw/nvme/ctrl.c
      ▲ MMIO 写被 CPU 模拟器拦截
【guest 内核】 pcie_nvme.ko                 ← 你写的驱动，看到的和真设备一模一样
```

   驱动运行时每一步都发生在 QEMU 进程里：写 doorbell → CPU 模拟器拦截 MMIO → `nvme_mmio_write` → `nvme_process_sq` 读 SQE 执行 → 读写变成对 nvme.img 的 pread/pwrite → 写 CQE + 翻相位位 → `msix_notify` 注入 MSI-X → guest CPU 跳进你的中断处理函数；"DMA" 实际是 QEMU 进程内的 memcpy。**接口层面（寄存器/DMA/中断）与真硬件等价，所以 QEMU 里写的驱动能平移上真机。**

**一句话总结**：QEMU 是宿主机上的一个用户态进程，用软件模拟 CPU/内存/中断控制器/PCIe 总线/NVMe 控制器；你的内核和驱动运行在这个进程里，数据最终落到宿主机文件 nvme.img。

**第一层：必须会（第 1 天，跑通即达标）**
1. 看懂 `start_qemu_virt.sh` 每一行：`-M virt,gic-version=3`（机器模型=虚拟 ARM 主板定义，含 GIC/PCIe host 拓扑）、`-cpu cortex-a53`、`-smp 2`（2 核）、`-m 512M`（内存）、`-kernel`（内核镜像 Image）、`-initrd`（根文件系统）、`-append "console=ttyAMA0 rdinit=/init"`（串口 + 首个用户态进程）、`-device nvme,serial=deadbeef,drive=nvme0`（**设备前端**）、`-drive file=...,if=none,id=nvme0`（**存储后端**）、`-nographic`（无图形，串口当终端）。
2. 理解三个概念：**内核镜像**（Image）、**根文件系统**（rootfs，busybox 做 /init）、**机器模型**（virt：QEMU 用软件定义的一台"虚拟主板"，NVMe/PCIe host 都是它虚拟出来的）。
3. 跑通全流程：启动 → `insmod /pcie_nvme.ko` → `ls /dev/nvme0n1` → `dd` 读写 MATCH → 退出（`Ctrl+a` 然后 `x`）。

**第二层：应该会（第 2~3 天，面试差异化）**
4. **QEMU monitor 调试**：启动时按 `Ctrl+a c` 进 monitor，`info pci` 看 PCIe 设备枚举结果（能看到 QEMU NVMe 的 vendor/device ID），`info qtree` 看设备拓扑，`x/np addr` 查看内存。
5. **GDB 联调内核/驱动**：`-s -S` 挂起等 GDB 连，宿主机 `gdb-multiarch vmlinux` + `target remote :1234`，可对驱动函数打断点单步——这是驱动开发的核心调试手段，面试能讲。
6. **读 QEMU 的 NVMe 模拟源码**（`qemu/hw/nvme/ctrl.c`，本项目 QEMU 源码在 `../qemu` 下）：看懂三条关键逻辑——
   - `nvme_mmio_write` 收到 doorbell 写 → 调 `nvme_process_sq` 取命令、模拟执行
   - 执行完写 CQE 到 CQ 内存 → 按 IV 调 `msix_notify` 注入 MSI-X 中断
   - `irq_loss` 属性：QEMU NVMe 设备参数，置 1 时模拟"丢中断"（只写 CQE 不发中断）——本项目阶段 5b 的验证手段就靠它
   看完你会恍然大悟：**驱动敲的每一个 doorbell 都进了 QEMU 的模拟函数，你发出的每个 MSI-X 都是 QEMU 的软件注入**。这是理解"虚拟设备 vs 真实设备"最直观的方式。

**第三层：面试必能讲（不是学，是能答）**
7. QEMU 与真机差异 5 个维度（背 `INTERVIEW.md` Q2 表格）：时序非实时、中断是软件注入、DMA 是 memcpy 模拟、无 IOMMU/SMMU、错误注入可控（真机无法随意丢中断，QEMU 一个参数搞定）。
8. fio 数据的正确解读：QEMU 上的数字证明「多队列架构带来收益」，不代表真实性能上限；面试官若质疑就说"模拟环境瓶颈在 CPU 软件模拟，真机上收益只会更明显"。
9. 为什么驱动学习选 QEMU：零硬件成本、可 GDB 单步、可注入故障（丢中断）——后两点真机上几乎做不到，这是本项目验证手段完整性的关键。

**验收**：能逐行解释 `start_qemu_virt.sh`；能用 monitor 查到 QEMU NVMe 的 PCI 信息；能讲清"我驱动里敲 doorbell 后，QEMU 内部发生了什么"。
**对应项目**：`README.md` 二、六节；QEMU 源码 `../qemu/hw/nvme/ctrl.c`。

### 阶段 1：块设备子系统（正点原子视频，1~2 周）
**这是整个项目最核心的地基**。NVMe 驱动本质上就是「一个注册到 blk-mq 框架的块设备驱动」。
**必学知识点**：
1. `struct gendisk`（块设备代表）、`struct request_queue`（请求队列）、`struct request`（单个 IO 请求）、`struct bio`（上层下发的 IO 单元）。
2. 字符设备 vs 块设备的本质区别：字符设备按字节流读写，块设备按「扇区/块」随机读写，有缓存层。
3. **blk-mq 多队列**：`tag_set`、`hw_ctx`、`queue_rq` 回调、`tag`（请求标签）、`blk_mq_alloc_disk`。
4. 一条 IO 的完整路径：用户 `read()` → VFS → page cache → bio → request → `queue_rq` → 设备。
**验收**：能画出「bio → request → queue_rq → 设备」的调用链，能解释 blk-mq 为什么比旧的单队列 request_fn 快。
**对应项目**：`pcie_nvme.c` 803-863 行（`nvme_mq_ops` / `nvme_setup_disk`）。

### 阶段 2：PCIe 协议（韦东山视频，1~2 周）
**目标**：理解「内核怎么找到设备、怎么访问设备寄存器、设备怎么触发中断」。
**必学知识点**：
1. PCIe 拓扑：Root Complex → Switch → Endpoint（NVMe 是 Endpoint）。
2. **配置空间**：vendor/device ID、class code、Command 寄存器（**Bit1 MEM 使能、Bit2 Bus Master**——项目坑 2 就栽在这）。
3. **BAR 空间**：设备把自己的寄存器/内存映射成一段 MMIO 地址，CPU 直接读写。
4. **枚举**：`pci_register_driver` + `id_table` 匹配 → probe 被调用。
5. **MSI / MSI-X 中断**：设备不靠物理中断线，靠「写一段内存」发中断，每个向量可独立绑 CPU。
**验收**：能解释「`pci_enable_device_mem` 和 `pci_set_master` 分别干了什么、为什么 NVMe 必须两个都调」。
**对应项目**：`pcie_nvme.c` 865-1037 行（probe）、坑 2、坑 8。

### 阶段 3：DMA 与内存一致性（2~3 天）
**目标**：理解「设备怎么读 CPU 内存里的命令/数据」。
**必学知识点**：
1. 为什么不能用 `kmalloc` 的虚拟地址直接给设备——设备用的是**总线地址/物理地址**，且要连续、要对齐。
2. `dma_alloc_coherent`（一致性映射，用于 SQ/CQ/PRP list，设备反复读）vs `dma_map_sg`（流式映射，用于数据 buffer，用完 unmap）。
3. 为什么 SQ/CQ 必须 4KB 对齐（NVMe 规范要求，见代码 941 行的检查）。
**验收**：能解释「`dma_map_sg` 返回的 `dma_addr_t` 是什么、和虚拟地址什么关系」。
**对应项目**：`pcie_nvme.c` 471-483 行（队列内存分配）、506-558 行（PRP 构造）。

### 阶段 4：NVMe 协议（3~5 天，无前置可速成）
**目标**：理解 NVMe 的「生产-消费」队列模型。
**必学知识点**：
1. **寄存器**：CAP（能力）、VS（版本）、CC（控制）、CSTS（状态）、AQA/ASQ/ACQ（Admin 队列配置）。
2. **SQ/CQ 队列对**：主机写 SQ（提交命令），设备写 CQ（返回结果）；**doorbell** 是通知机制——写 SQ tail doorbell 告诉设备有新命令，设备完成后写 CQ、主机收割后写 CQ head doorbell 释放。
3. **phase bit（P bit）**：不用清内存就能判断 CQ 条目是新是旧，转一圈翻转一次。
4. **命令格式**：SQE 64 字节（opcode + nsid + PRP + cdw10~15），CQE 16 字节（command_id + status）。
5. **PRP**：页对齐物理页指针，单页 PRP1，多页 PRP1+PRP2（指向 PRP list 页）。
6. **Admin 队列 vs I/O 队列**：Admin（qid=0）只跑管理命令（Identify/Create Queue），I/O 队列跑读写。
**验收**：能不看文档画出「主机提交一条 Read 命令到设备返回 CQE」的完整时序图，含 4 次 doorbell。
**对应项目**：`pcie_nvme.c` 237-501 行（doorbell/init/queue 创建）。

### 阶段 5：精读本项目源码（1 周，核心）
**目标**：1086 行逐模块讲清楚，用「数据流」串起来。
**方法**：按本文档「二、源码精讲」的顺序，边读边在纸上画数据流。每读完一个模块，合上代码用自己的话复述。
**验收**：能不看代码，从「用户 dd 写一个文件」讲到「数据落到 NVMe 盘」，中间每个环节的函数名、关键寄存器都能报出来。

### 阶段 6：中断下半部与并发（2~3 天，进阶）
**目标**：理解中断处理的分层设计（这也是面试高频点）。
**必学知识点**：
1. 硬中断上半部 vs 软中断下半部 vs workqueue，为什么要分。
2. 本项目：中断上半部 `nvme_irq` 只做「收割 CQ + 记录 status + 调 `blk_mq_complete_request`」，真正的 unmap + end_request 在软中断里 `nvme_mq_complete` 做。
3. `blk_mq_complete_request` 的同步/异步（IPI vs 直接）两种完成路径——坑 9 就源于此。
**验收**：能解释「为什么 unmap 不放在中断上半部」。

### 阶段 7：面试强化（持续，配合 INTERVIEW.md）
**目标**：把「会写」变成「能讲」。
**方法**：
1. 背熟 `INTERVIEW.md` 的 Q&A。
2. 针对每个踩坑（README 第四节 9 个坑），练习「现象 → 根因 → 解决」三段式口述。
3. 模拟追问：找个人（或让我）顺着「PRP 页对齐为什么」「doorbell 为什么要两个」「超时怎么判断真卡死」逐层问下去。
**验收**：每个知识点能在 30 秒内口述清楚。

---

## 二、源码精讲（1086 行，按数据流）

> 驱动本质是「数据搬运工」，分 4 层：PCIe 层（找到设备）→ NVMe 协议层（收发命令）→ blk-mq 层（接入内核框架）→ DMA 层（共享内存）。

### 2.1 数据结构（123-217 行，先看懂「有什么」）

**`struct nvme_sqe`（123-138）**：提交队列项，固定 64 字节。核心字段：
- `opcode`：命令类型（Read=0x02 / Write=0x01 / Identify=0x06）
- `command_id`：命令编号，本项目直接复用 `rq->tag`
- `prp1/prp2`：数据缓冲区物理地址（PRP 机制核心）
- `cdw10~15`：命令参数（读写的起始 LBA、长度等）

**`struct nvme_cqe`（141-148）**：完成队列项，固定 16 字节。核心字段：
- `command_id`：对应哪条命令（回查 request 用）
- `status`：**P[bit0] 相位 + SC[bit15:1] 状态码**，`>>1` 得到状态码

**`struct nvme_cmd`（160-166）**：per-request 私有数据（blk-mq 的 pdu）。存 sg 数组、PRP list 指针、状态。

**`struct nvme_queue`（168-181）**：一个队列对（SQ+CQ）。含 `sq_tail/cq_head/cq_phase`（环形队列游标）、`irq`（MSI-X 号）、`tags`（command_id→request 映射）。

**`struct pcie_nvme_dev`（183-217）**：整个设备。含 Admin 队列、I/O 队列数组、`tag_set`、`gendisk`、`prp_pool`。

### 2.2 寄存器与 doorbell（42-49, 237-245 行）

NVMe 控制器是一段 MMIO 内存（BAR0），CPU 直接读写寄存器：
- `CAP`(0x00)：读能力，`MQES` 字段 = 最大队列深度（本项目读出 2048）
- `CC`(0x14)：写控制，`EN` 位使能控制器
- `CSTS`(0x1c)：读状态，`RDY` 位 = 控制器就绪
- `DBS`(0x1000)：doorbell 基地址

**doorbell 地址计算（237-245）**：每个队列占用 2 个 doorbell（SQ tail + CQ head），间隔由 `dstrd`（CAP 里的 doorbell stride）决定：
```
SQ doorbell = DBS + (2*qid)   * dstrd
CQ doorbell = DBS + (2*qid+1) * dstrd
```

### 2.3 控制器初始化（247-296 行）

`nvme_enable_ctrl`（247）是启动控制器的完整流程：
1. 检查 `CSTS.RDY` 是否已置位（防止重复初始化）
2. 写 `AQA`（Admin 队列深度）、`ASQ/ACQ`（Admin SQ/CQ 的**总线地址**）
3. 写 `CC.EN=1`（同时配好 MPS/CSS/IOSQES/IOCQES）
4. **轮询** `CSTS.RDY` 直到置 1（带超时），检查 `CFS`（致命错误位）

注意：`ASQ/ACQ` 写的是 `admin_sq_dma`（dma_addr_t 总线地址），不是虚拟地址——这是 DMA 设备的关键认知。

### 2.4 Admin 命令往返（298-407 行）

**提交（`nvme_admin_cmd` 330）**：拷贝 SQE 到 SQ 当前 tail → tail+1 → **写 SQ tail doorbell**（这一步才是真正通知设备）。

**等待（`nvme_admin_wait_cq` 299）**：轮询 CQ 当前 head 的 phase bit，等于期望 phase 说明完成 → 检查 status → head+1 → **写 CQ head doorbell**（释放 CQ 条目）。

**Identify（354-407）**：Admin 队列跑的第一个命令，读设备信息：
- `nvme_identify_ctrl`（CNS=1）：读 VID/MN/SN/固件版本（验证阶段 2）
- `nvme_identify_ns`（CNS=0）+ `nvme_parse_ns`（395）：读 `NSZE`（总 LBA 数）和 `LBADS`（LBA 大小），这两个决定块设备容量

**关键坑**：`NN` 字段在 Identify 数据的 **offset 516** 不是 512（坑 3）。

### 2.5 创建 I/O 队列（409-501 行）

Admin 队列负责创建 I/O 队列（自己不会跑读写）：
- `nvme_create_io_cq`（410）：opcode 0x05，`cdw11` 里 `IEN=1`（使能中断）+ `IV=qid-1`（**每个 CQ 绑独立 MSI-X 向量**，坑/考点）
- `nvme_create_io_sq`（426）：opcode 0x01，`cdw11` 里 `CQID=qid`（SQ 关联到哪个 CQ）
- `nvme_alloc_io_queue`（458）：分配 SQ/CQ 内存（`dmam_alloc_coherent`，保证总线地址连续+4KB 对齐）→ 创建 CQ/SQ → `request_irq` 绑定中断

### 2.6 blk-mq 数据通路（739-807 行，核心）

**`nvme_queue_rq`（775）**：blk-mq 框架在有新 request 时调用的入口：
1. `blk_mq_start_request` 标记开始
2. `nvme_map_and_submit` 构造并提交命令
3. 返回 `BLK_STS_OK` 表示已提交，**不等待完成**（异步）

**`nvme_map_and_submit`（739）**：把 request 翻译成 SQE：
- 零数据（flush）：opcode=FLUSH
- 有数据：算 SLBA（起始 LBA，按 `lba_shift` 换算 512B 扇区）→ 调 `nvme_setup_prps` 建 PRP → 填 opcode/nsid/cdw10~12 → 填 `command_id = rq->tag`
- `nvme_submit_sqe`（573）：拷贝到 SQ + 写 SQ tail doorbell

### 2.7 PRP 构造（506-558 行，NVMe 数据通路核心）

**PRP 规则**：数据缓冲区必须是「页对齐的物理页列表」，长度页整数倍。
- **单页**（`length <= PAGE_SIZE`）：`prp1` 直接指向 sg[0] 的 dma 地址，`prp2=0`
- **多页**：`prp1` = 第一页地址；`prp2` = 指向一个 PRP list 页（`dma_pool_alloc` 分配的一页，每页放 512 个 8 字节条目），list 里是第 2~N 页的地址

**流程**：`blk_rq_map_sg`（把 request 的 bio 转 sg 表）→ `dma_map_sg`（映射，拿到总线地址）→ 逐 sg 按页拆分填入 PRP list。

### 2.8 中断完成（627-703 行）

**中断上半部 `nvme_irq`（695）**：
- `irq_loss=1` 时直接返回（模拟丢中断，验证超时用）
- 否则调 `nvme_process_cq`

**`nvme_process_cq`（671）**：收割 CQ 所有 phase 匹配的 CQE，对每个 CQE 调 `nvme_complete_rq`，最后写 CQ head doorbell。

**`nvme_complete_rq`（627）**：
1. 取 `command_id`（= 提交时的 `rq->tag`）
2. `blk_mq_tag_to_rq` 反查 request
3. 记录 status 到 pdu
4. `blk_mq_complete_request` 触发块层软中断

**下半部 `nvme_mq_complete`（660）**（软中断上下文）：unmap 数据 + `blk_mq_end_request` 真正结束 request。

**关键设计**：上半部只做轻活（收割+标记），重活（unmap）放下半部——缩短硬中断占用，对标内核 nvme pci.c。

### 2.9 超时处理（706-737 行）

`tag_set.timeout` 到点 → 块层调 `nvme_timeout`：
1. **先 `nvme_process_cq` 主动收割 CQ**（覆盖「中断丢失」场景）
2. 判断 request 是否结束：用 `blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT`（**不是** `blk_mq_request_completed`，坑 9）
3. 结束了返回 `BLK_EH_DONE`；没结束返回 `BLK_EH_RESET_TIMER` 继续等

### 2.10 错误码细分（584-624 行）

CQE `status >> 1` 得到 `SCT<<8|SC`（bit10:8=SCT 错误类型，bit7:0=SC 具体错误）。`nvme_error_status` 用 `switch (status & 0x7ff)`（屏蔽 M/DNR/CRD）把 NVMe 错误映射成 blk-mq 通用返回码（`BLK_STS_MEDIUM`/`NOSPC`/`TRANSPORT` 等），对标内核 `core.c:nvme_error_status`。

**考点**：同一 SC 值在不同 SCT 下含义不同，`0x80~0xbf`=介质错误(SCT=2)、`0x70~0x7f`=命令特定(SCT=1)，所以必须用 `SCT<<8|SC` 完整编码（不能只看裸 SC）。

### 2.11 probe / remove 生命周期（865-1082 行）

**probe（865）**：PCIe 枚举匹配到设备后的总入口，按顺序：
`pci_enable_device_mem` + `pci_set_master`（开 Bus Master，坑 2）→ 分配 dev → `pci_request_mem_regions` → `devm_ioremap` 映射 BAR0 → 读 CAP/VS/CSTS → 分配 Admin 队列内存 → 使能控制器 → Identify → 建 prp_pool → 算队列数 → `pci_alloc_irq_vectors`（**先定中断数再建 tag_set**）→ `nvme_setup_disk` → 循环建 I/O 队列 → `add_disk` 注册块设备。

**remove（1039）**：反向清理，顺序严格（先 `del_gendisk` quiesce 队列，再 free_irq、删队列、free 资源）。

---

## 三、必掌握知识库（面试 checklist）

> 学完能对下面每一条「不看资料口述 30 秒」= 达标。

### A. 块设备 / blk-mq（最高频）
- [ ] bio / request / gendisk 三者关系
- [ ] blk-mq 多队列 vs 旧单队列，为什么快
- [ ] `queue_rq` 回调的职责和上下文（原子上下文，不能睡眠）
- [ ] `tag` 是什么、`command_id` 为什么能复用 tag
- [ ] `blk_mq_complete_request` 同步/异步两条路径的区别
- [ ] `tag_set` 的字段：`nr_hw_queues`/`queue_depth`/`cmd_size`/`timeout`

### B. PCIe
- [ ] 枚举流程：`pci_register_driver` → probe
- [ ] 配置空间 Command 寄存器 MEM 位 vs Bus Master 位
- [ ] BAR 是什么、`devm_ioremap` 干什么
- [ ] MSI vs MSI-X 区别，为什么 NVMe 要 MSI-X
- [ ] `pci_alloc_irq_vectors` 的返回值含义（实际分配到的向量数）

### C. NVMe 协议
- [ ] SQ/CQ 队列对模型（生产-消费）
- [ ] 4 次 doorbell（提交 2 次 + 完成 2 次）各自作用
- [ ] phase bit 机制，为什么不用清内存
- [ ] SQE 64 字节 / CQE 16 字节字段布局
- [ ] Admin 队列 vs I/O 队列分工
- [ ] PRP 单页/多页两种情况，页对齐约束
- [ ] `IV=qid-1` 每队列独立中断向量的意义
- [ ] CQE status 的 SCT/SC/M/DNR 位含义

### D. DMA 与内存
- [ ] `dma_alloc_coherent` vs `dma_map_sg` 的使用场景
- [ ] 为什么 SQ/CQ 要 4KB 对齐
- [ ] 总线地址（dma_addr_t）和虚拟地址的区别
- [ ] `dma_pool` 的作用（本项目 PRP list 页）

### E. 中断子系统
- [ ] 上半部/下半部/软中断/workqueue 区别
- [ ] 为什么 unmap 放 `.complete` 回调不放中断上半部
- [ ] `request_irq` 的 dev_id 参数作用（本项目传 `q` 指针）

### F. 工程与调试（加分项）
- [ ] 9 个踩坑每个的「现象→根因→解决」三段式
- [ ] 分阶段验证方法论（数据正确性/中断正交调试）
- [ ] QEMU vs 真实 NVMe 的 5 个维度差异
- [ ] QEMU 整机启动原理（无 U-Boot 的 -kernel 启动链、initramfs、前端/后端分离）
- [ ] fio 数据怎么证明多队列价值（numjobs 1→2 翻倍）

---

## 四、学习方法论（保证真学会）

1. **先跑通再看懂**：每个阶段先在 QEMU 里跑出结果（`dd MATCH`、中断计数增长），再回头看代码。有感性认识，代码才读得进去。
2. **画图不背术语**：数据流、doorbell 时序、中断路径，全部自己画一遍。面试官也是看你能不能「画出来讲清楚」。
3. **合上书复述**：读完一个模块，关掉代码，用自己的话讲一遍，讲不出来就是没懂。
4. **对标内核**：每个功能都去 `drivers/nvme/host/pci.c` 和 `core.c` 找对应实现，看内核为什么这么写（这是本项目最大的加分项——你不是在背，是在对照权威实现）。
5. **踩坑当案例**：9 个坑每个都是一道面试题，背熟「现象→根因→解决」。
6. **不懂就问**：卡在哪个知识点，直接问，我按你当前的水平讲。

---

## 五、时间估算（供参考）

| 阶段 | 内容 | 预估 |
|---|---|---|
| 0 | 环境跑通 + QEMU 原理 | 1~3 天 |
| 1 | 块设备/blk-mq（正点原子） | 1~2 周 |
| 2 | PCIe（韦东山） | 1~2 周 |
| 3 | DMA/一致性 | 2~3 天 |
| 4 | NVMe 协议 | 3~5 天 |
| 5 | 精读源码 | 1 周 |
| 6 | 中断/并发进阶 | 2~3 天 |
| 7 | 面试强化 | 持续 |

总计约 4~6 周，取决于每天投入。阶段 1、2 是硬骨头，后面的 NVMe 和源码精读会因为你有了块设备+PCIe 底子而变得顺理成章。
