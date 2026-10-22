# 源码精讲：从零读懂 pcie_nvme.c（1086 行）

> 使用方式：VSCode 打开 `driver/pcie_nvme.c` 对照阅读，每讲标了行号。
> `Ctrl+点击` 任何内核函数可跳转到 linux-6.5 内核源码（clangd 已配置）。
> 每讲结束有「口述自测」，答不上来就是没懂，回头重读。

---

## 第 0 讲：整体骨架（先建立全局印象）

**这个驱动在干一件什么事？** 用你熟悉的字符设备驱动做锚点：

| | 字符设备（点灯/按键） | 这个 NVMe 驱动 |
|---|---|---|
| 设备在哪 | CPU 直接读写寄存器 | 设备是**外置的**，挂在 PCIe 总线上 |
| 数据怎么传 | 读写寄存器 | **DMA**（设备自己读内存） |
| 通知机制 | 无/轮询 | **doorbell 写寄存器** + **MSI-X 中断** |
| 内核框架 | file_operations | **blk-mq**（块设备框架） |

一句话：**NVMe 驱动 = 让内核块设备框架能往一块"挂在 PCIe 上的 SSD"读写数据，靠"命令队列 + DMA + 中断"协作。**

---

**先补三块地基**——没有它们，后面 14 讲都只能半懂。读懂本驱动只需要这三块。

### 地基 1：三个地址世界（全驱动最难的概念，一次讲透）

主机和设备是两个世界，**各说各的地址**：

```
世界 A：内核虚拟地址（CPU 的世界）     世界 B：总线地址（设备的世界）
┌───────────────────────────────┐   ┌───────────────────────────────┐
│ 指针、kmalloc、memcpy 用       │   │ 设备 DMA 自己读写内存用        │
│ dev->admin_sq（CPU 写命令用它） │◄─对─┤ admin_sq_dma（设备只认它）  │
└───────────────────────────────┘   └───────────────────────────────┘
                 世界 C：MMIO（设备的"身体"）
     ┌────────────────────────────────────┐
     │ 设备的寄存器映射进 CPU 地址空间      │
     │ dev->bar0：CPU 用 readl/writel 访问 │
     └────────────────────────────────────┘
```

- **世界 A（内核虚拟地址）**：CPU 执行代码用的地址。`kmalloc`、`dma_alloc_coherent` 返回的指针就是。代码里 `memcpy(&dev->admin_sq[tail], sqe, ...)` 用的是它。
- **世界 B（总线地址/DMA 地址）**：**设备** DMA 读内存时用的地址。设备不认识指针。所以 `dma_alloc_coherent` 成对返回两个值：指针（A 世界）+ `admin_sq_dma`（B 世界）。**同一块 SQ 内存，两套地址**——这就是为什么 262 行写 ASQ 寄存器的是 `admin_sq_dma` 而不是 `admin_sq`。写错 = 设备去读一个不存在的地址 = 挂。
- **世界 C（MMIO）**：设备"身上"的寄存器（CAP/CC/doorbell）不在内存里，通过 `ioremap` 映射到 CPU 地址空间后用 `readl/writel` 访问。`dev->bar0` 就是。doorbell 为什么走 bar0？因为 doorbell 是设备的寄存器，不是内存。

**一句话**：CPU 访问设备寄存器走 MMIO（C），设备读内存走总线地址（B），CPU 自己读内存走虚拟地址（A）。以后看到代码里的 `*_dma` 后缀，就告诉自己：这是给设备看的地址。

### 地基 2：PCIe 枚举（为什么这个驱动没有"设备树匹配"）

PCIe 设备出厂自带**身份证**：配置空间里的 vendor ID + device ID（本驱动 38-39 行的 `0x1b36:0x0010`）。内核启动扫 PCIe 总线，逐个读身份证 → 和已注册驱动的 `id_table`（219 行）比对 → 对上就调 `probe`。整个过程**不需要设备树节点**——这解释了 virt_machine.dts 里 pcie 节点下为什么没有 nvme 子节点：PCIe 设备自描述，设备树只管"无法枚举"的平台设备。

### 地基 3：NVMe 协议全链路（后面 14 讲都是这张图的局部放大）

```
主机驱动                             设备（QEMU hw/nvme/ctrl.c 模拟）
 ① memcpy 命令进 SQ（内存）
 ② writel 敲 SQ tail doorbell ──────▶ 看见门铃
 ③ ◀─────────────────────────────── 设备 DMA 读走 SQE
 ④ ◀─────────────────────────────── 设备按 PRP 指路 DMA 搬数据
 ⑤ 设备把结果写进 CQ（内存）
 ⑥ ◀─────────────────── MSI-X 中断── 设备发中断通知
 ⑦ 主机收割 CQ、敲 CQ head doorbell ─▶ 归还位置
```

| 讲次 | 放大这张图的哪一段 |
|---|---|
| 第 3 讲 | ①②：门铃 + 开机 |
| 第 4 讲 | ①~⑦ 完整一圈（轮询版） |
| 第 7、8 讲 | ①④：命令产生 + 设备怎么找到数据 |
| 第 9、10 讲 | ⑤⑥⑦：完成与中断（及中断丢失兜底） |
| 第 13 讲 | 把这些零件按依赖顺序组装 |
| 第 14 讲 | 全图串讲 |

**文件从上到下的组织**（1086 行分 11 块）：

```
 38-121 行   宏定义：寄存器偏移、位域、opcode、状态码
123-217 行   数据结构：SQE / CQE / nvme_cmd / nvme_queue / pcie_nvme_dev
219-245 行   驱动注册表 + 可调参数 + doorbell 地址计算
247-296 行   控制器使能/关闭
298-338 行   Admin 命令提交与等待
340-407 行   Identify 读设备信息
409-501 行   创建 I/O 队列（SQ+CQ+中断）
503-578 行   PRP 构造（数据通路核心）
580-737 行   错误码 + 中断完成 + 超时
739-863 行   blk-mq 数据通路 + 框架接入
865-1082 行  probe / remove 生命周期
```

**读代码的顺序就是数据流的顺序**：先有数据结构（"有什么"），再有寄存器（"怎么通知"），再有命令往返（"怎么对话"），再有数据通路（"怎么搬数据"），最后是生命周期（"怎么组装"）。

---

## 第 1 讲：寄存器与常量（38~121 行）

### 1.1 PCIe 设备身份（38-39 行）

```c
#define NVME_VENDOR_ID  0x1b36
#define NVME_DEVICE_ID  0x0010
```

vendor ID `0x1b36` 是 **Red Hat（QEMU 作者之一）的 ID**——QEMU 模拟的 NVMe 设备就报这个 ID。PCIe 驱动靠这对 ID 匹配设备（后面 219-223 行的 id_table）。真机上插的 NVMe SSD 报的是真实厂商 ID（三星 0x144d、Intel 0x8086 等），所以这个驱动**专为 QEMU 设备写，换真盘要加 ID**——这是面试常问的"QEMU 与真机差异"第 1 点。

> **来源**：PCI ID 注册表（`/usr/share/hwdata/pci.ids`，`lspci -nn` 显示的厂商名就来自它）；QEMU 侧定义在 `qemu/hw/nvme/ctrl.c:8081`（`PCI_VENDOR_ID_REDHAT`）。真机查自己盘的 ID：`lspci -nn`。

### 1.2 寄存器偏移（41-49 行）—— NVMe 控制器就是一段内存

NVMe 规范规定控制器的寄存器全部在 **BAR0** 里，各寄存器有固定偏移：

| 偏移 | 名字 | 含义 | 读写 |
|---|---|---|---|
| 0x00 | CAP | 能力（队列深度上限、超时粒度等） | 只读 |
| 0x08 | VS | 规范版本 | 只读 |
| 0x14 | CC | 控制（**EN 位 = 电源开关**） | 读写 |
| 0x1c | CSTS | 状态（**RDY 位 = 就绪灯**） | 只读 |
| 0x24 | AQA | Admin 队列深度 | 写 |
| 0x28 | ASQ | Admin SQ 的**总线地址** | 写 |
| 0x30 | ACQ | Admin CQ 的**总线地址** | 写 |
| 0x1000 | DBS | Doorbell 寄存器基地址 | 写 |

类比：CAP/VS 是设备的"说明书"，CC 是"开关"，CSTS 是"状态灯"，AQA/ASQ/ACQ 是"告诉设备你的邮箱在哪"。

> **来源**：NVMe 规范 Controller Registers 章（寄存器总表，按图号找）→ 内核 `include/linux/nvme.h:122` 起的 `enum nvme_reg`（值与本表逐项一致）→ QEMU 设备侧 `include/block/nvme.h`（用 `offsetof(NvmeBar, ...)` 生成同样偏移）。

### 1.3 位域宏（51-71 行）—— 从 64 位寄存器里抠字段

以 CAP 为例（QEMU 实际值 `0x103403fff`）：

```c
#define NVME_CAP_MQES(cap)  ((int)((cap) & 0xffff))          // bit0-15：最大队列深度-1
#define NVME_CAP_DSTRD(cap) (1u << (2 + (((cap) >> 32) & 0xf)))  // bit32-35：doorbell 间隔
#define NVME_CAP_TO(cap)    ((((cap) >> 24) & 0xff))         // bit24-31：超时粒度
```

- **MQES**：`0x3ff` = 1023，即最大 1024 队列深度。本项目 978 行用它算 I/O 队列数上限。
- **DSTRD**：doorbell stride，两个相邻 doorbell 寄存器之间隔几个字节。公式 `1<<(2+n)`，n=0 时 4 字节。本项目读出 4。
- **TO**：控制器超时粒度（500ms 单位），使能控制器时用它算轮询超时（270 行）。

> **来源**：NVMe 规范 CAP 寄存器位域图 → 内核 `include/linux/nvme.h:155-157`（`NVME_CAP_MQES/NVME_CAP_TIMEOUT/NVME_CAP_STRIDE`，驱动里改名为 TO/DSTRD）。QEMU 设备报的 CAP 实际值 `0x103403fff` 定义在 QEMU 设备初始化代码里。

CC 的字段（56-63 行）是面试重点，**写一次 CC 同时配 6 样东西**：

```c
#define NVME_CC_ENABLE     BIT(0)     // 1=开机
#define NVME_CC_CSS_NVM    (0 << 4)   // 命令集：NVM（就是普通 SSD）
#define NVME_CC_MPS_4K     (0 << 7)   // 内存页大小：4KB（必须与主机页大小一致）
#define NVME_CC_AMS_RR     (0 << 11)  // 仲裁机制：Round-Robin
#define NVME_CC_IOSQES     (6 << 16)  // I/O SQ 条目大小 = 2^6 = 64B
#define NVME_CC_IOCQES     (4 << 20)  // I/O CQ 条目大小 = 2^4 = 16B
```

**考点**：为什么 IOSQES 填 6？因为 SQE 固定 64 字节，CC 里存的是"以 2 为底的对数"，`2^6=64`。写成 `6 << 16` 而不是 `64 << 16`——这是 NVMe 规范的设计（省位）。

> **来源**：NVMe 规范 CC 寄存器位域图 → 内核 `include/linux/nvme.h:193` 起（`NVME_CC_ENABLE`、`NVME_CC_IOSQES` 等，值与本项目一致）。

### 1.4 命令 opcode（73-83 行）

```c
/* Admin 命令（管理用，跑在 qid=0 的队列上） */
NVME_ADMIN_DELETE_SQ 0x00 / CREATE_SQ 0x01 / DELETE_CQ 0x04 / CREATE_CQ 0x05 / IDENTIFY 0x06
/* I/O 命令（读写用，跑在 qid>=1 的队列上） */
NVME_CMD_FLUSH 0x00 / WRITE 0x01 / READ 0x02
```

**考点**：同一 opcode 值（如 0x00）在 Admin 队列是"删除 SQ"，在 I/O 队列是"Flush"——opcode 含义随队列类型变化。

> **来源**：NVMe 规范命令定义章（Admin Command Set 与 NVM Command Set 各自的 opcode 表）→ 内核 `include/linux/nvme.h:1136`（`enum nvme_admin_opcode`）、`:821`（`enum nvme_opcode`）。

### 1.5 状态码常量（91-120 行）—— 阶段 5c 新增

```c
#define NVME_SC_SUCCESS         0x0    // 成功
#define NVME_SC_LBA_RANGE       0x80   // 访问越界（SCT=2 介质类）
#define NVME_SC_CAP_EXCEEDED    0x81   // 容量不足（→ BLK_STS_NOSPC）
#define NVME_SC_WRITE_FAULT     0x280  // 写介质错误（→ BLK_STS_MEDIUM）
```

注意这些值已经是 `status >> 1` 之后的**完整编码 `SCT<<8|SC`**（不是裸 SC）。为什么 `LBA_RANGE` 是 0x80 而不是 0x00？因为它的 SCT=2（介质类错误）占了高 3 位。裸 SC 相同的值在不同 SCT 下含义完全不同——第 11 讲详解。

> **来源**：NVMe 规范 CQE status 字段定义章（状态码总表）→ 内核 `include/linux/nvme.h:1847` 起的状态码 enum（`NVME_SC_LBA_RANGE=0x80` @1885、`NVME_SC_WRITE_FAULT=0x280` @1967——与本项目宏值逐位一致，本项目就是从这里抄的）。

### 1.6 这些值到底从哪来（来源体系，写驱动必备技能）

写任何一个硬件驱动，常量来源只有三个层次。**每写一个宏，先想清楚它属于哪一层**：

| 层次 | 是什么 | 怎么获取 | 什么时候用它 |
|---|---|---|---|
| ① NVMe 规范 | 协议的"法律条文"，所有值的源头 | nvmexpress.org → 免费下载 NVMe Base Specification | 理解语义、核对位域含义时 |
| ② 内核头文件 | 别人按规范抄好的现成定义 | 本机就有：`qemu/linux-6.5/include/linux/nvme.h` | 写代码抄值时（快、值已验证） |
| ③ 设备实现 | 设备认不认这个值，看设备代码 | QEMU：`qemu/include/block/nvme.h`、`qemu/hw/nvme/ctrl.c`；真机：没源码，靠实测命令 | 怀疑设备行为不符时 |

**第 ① 层怎么用**：规范里的寄存器定义在一张总表（Controller Registers 章，按"图号"找——小节号随规范版本变动）。查一个寄存器 = 看它的位域图：偏移在哪、每个 bit 干什么、读写属性。**写驱动的正确姿势是"对着位域图写宏"，不是背值**。

**第 ② 层怎么用**（最快路径，命令直接复制运行）：

```bash
# 查寄存器偏移
grep -n "NVME_REG_CAP\|NVME_REG_CC\|NVME_REG_DBS" qemu/linux-6.5/include/linux/nvme.h
# 查 CAP 位域宏
grep -n "NVME_CAP_MQES\|NVME_CAP_TIMEOUT\|NVME_CAP_STRIDE" qemu/linux-6.5/include/linux/nvme.h
# 查状态码
grep -n "NVME_SC_LBA_RANGE\|NVME_SC_WRITE_FAULT" qemu/linux-6.5/include/linux/nvme.h
```

本项目的宏几乎全部来自这份头文件（已对照：值逐位一致）。**但只抄不理解 = 面试一问就穿**，所以抄完必须回到第 ① 层读懂位域含义。

**第 ③ 层怎么用**：驱动写的值设备认不认，看设备实现。QEMU 是开源的——`hw/nvme/ctrl.c` 里的 `NVME_CAP_MQES(...)` 校验就是证据（设备真会检查你写的队列深度合不合法）。真机没有源码，用实测命令验证：

```bash
lspci -nn                                # 查 vendor/device ID（本驱动 38-39 行的来源）
lspci -xxx -s <BDF>                      # dump 256B 配置空间（身份证全貌）
cat /sys/bus/pci/devices/<BDF>/resource  # 看 BAR0 基址和长度（probe 里 pci_resource_start 的来源）
```

**面试话术**："我的寄存器宏对照 NVMe 规范位域图逐位核对，参考内核 include/linux/nvme.h，并用 QEMU 设备源码验证过设备侧行为"——一句话证明你不是抄代码，是读规范写的驱动。

**口述自测**：CC 寄存器一次写入配了哪 6 样东西？DSTRD 是干什么用的？

---

## 第 2 讲：数据结构（123~217 行）—— 五个核心结构体

NVMe 的灵魂是 **SQ/CQ 队列对**：主机往 SQ 放"命令纸条"，敲 doorbell 通知设备；设备完成后往 CQ 放"结果纸条"，发中断通知主机。这张纸条就是 SQE，结果就是 CQE。

### 2.1 `struct nvme_sqe`（123-138 行）—— 提交队列项，固定 64 字节

```c
struct nvme_sqe {
	u8  opcode;       // 干什么：READ=0x02 / WRITE=0x01
	u8  flags;        // 标志（本项目没用，填 0）
	u16 command_id;   // 命令编号，完成后靠它对上号
	u32 nsid;         // 命名空间 ID（一个盘可分多个逻辑盘）
	u64 rsvd2;
	u64 metadata;     // 元数据（本项目不用）
	u64 prp1;         // ★ 数据缓冲区物理地址 1
	u64 prp2;         // ★ 数据缓冲区物理地址 2
	u32 cdw10..cdw15; // 命令参数（读写时：起始 LBA、长度）
};
```

**重点盯 `prp1/prp2`**：设备靠这两个字段自己去内存找数据。所以填的是**总线地址**（DMA 地址），不是 `kmalloc` 出来的虚拟地址——这是 DMA 设备与字符设备最本质的区别。字符设备是 CPU 用虚拟地址读写寄存器，NVMe 是**设备**用总线地址读写内存。

### 2.2 `struct nvme_cqe`（141-148 行）—— 完成队列项，固定 16 字节

```c
struct nvme_cqe {
	u32 result;       // 命令结果（Identify 时是数据）
	u32 rsvd1;
	u16 sq_head;      // 设备回报：SQ 已消费到哪
	u16 sq_id;        // 哪个队列的完成
	u16 command_id;   // 对应哪条命令
	u16 status;       // ★ bit0 = P(相位位)，bit15:1 = 状态码
};
```

**P 位（相位位）是本项目理解门槛之一**：CQ 是环形缓冲区，内存反复用。怎么判断某条 CQE 是"新写入的结果"还是"上轮残留的旧结果"？设备每写一圈就把 P 位翻转一次；主机记住"当前该收到 P=几"，读到不匹配的就说明"还没写完/是旧的"。**不需要清内存，转一圈翻一次**——这是高性能设计（避免每轮 memset 4KB 队列）。

### 2.3 `struct nvme_cmd`（159-166 行）—— 每个请求的"草稿纸"

```c
struct nvme_cmd {
	struct scatterlist sg[NVME_MAX_SEGS]; // 数据分段表（bio 拆出来的）
	int nsegs;                            // 段数
	void *prp_list;                       // PRP list 页虚拟地址
	dma_addr_t prp_list_dma;              // PRP list 页总线地址
	u16 status;                           // CQE 状态（中断记下，complete 回调读）
};
```

内核每下发一个 IO 请求，blk-mq 会在 request 后面**按 `cmd_size` 额外分配一块内存**给它当私有草稿纸（pdu）。数据在途期间，sg 表、PRP list 页都挂在上面；中断里记录的 status 也放这，供下半部使用。**考点**：`blk_mq_rq_to_pdu(rq)` 就是拿到这块草稿纸。

### 2.4 `struct nvme_queue`（168-181 行）—— 一对 SQ+CQ

```c
struct nvme_queue {
	struct pcie_nvme_dev *dev;   // 反向指回总管家
	u16 qid;                     // 0=Admin，1..N=I/O
	u16 q_depth;                 // 深度（I/O 队列 64）
	struct nvme_sqe *sq;         // SQ 内存（虚拟地址）
	dma_addr_t sq_dma;           // SQ 内存（总线地址，给设备用）
	struct nvme_cqe *cq;         // CQ 内存
	dma_addr_t cq_dma;
	u16 sq_tail;                 // 游标1：下一条命令写到 SQ 的哪个位置
	u16 cq_head;                 // 游标2：下一条结果从 CQ 的哪个位置读
	u8  cq_phase;                // 游标3：当前期望收到 P=0 还是 P=1
	int irq;                     // 绑定的 MSI-X 中断号
	struct blk_mq_tags *tags;    // command_id → request 映射表
};
```

**三个游标是队列的全部状态**：`sq_tail`（写命令）、`cq_head`（读结果）、`cq_phase`（判新旧）。后面所有代码都在维护这三个数。

### 2.5 `struct pcie_nvme_dev`（183-217 行）—— 整个设备的"总管家"

probe 成功后，这个结构体就是全局上下文，什么都能从它找到：`pdev`（PCI 设备）、`bar0`（MMIO 映射）、Admin 队列三件套（SQ/CQ/游标）、Identify 缓冲区、namespace 信息（`nsze` 总扇区数 + `lba_shift`）、I/O 队列数组、`tag_set`（blk-mq 配置）、`disk`（块设备）、`prp_pool`（PRP 页池）。

**结构体关系图**：

```
pcie_nvme_dev（总管家）
 ├─ pdev ──────── PCI 设备（配置空间、中断向量）
 ├─ bar0 ──────── MMIO 映射 → 寄存器 + doorbell
 ├─ admin_sq/admin_cq ── Admin 队列对（qid=0）
 ├─ identify ──── 4096B DMA 缓冲（Identify 命令的数据落点）
 ├─ queues[] ──── nvme_queue × N（每 CPU 一对 SQ/CQ）
 │     ├─ sq/cq ── 队列内存
 │     └─ tags ─── blk-mq 的 tag 映射表
 ├─ tag_set ───── blk-mq 配置（几个队列、多深、pdu 多大）
 ├─ disk ──────── gendisk（/dev/nvme0n1 就是它）
 └─ prp_pool ──── PRP list 页池
```

**口述自测**：P 位为什么能判断 CQE 新旧而不需要清内存？`nvme_cmd` 这块"草稿纸"是谁分配的？

---

## 第 3 讲：doorbell 与控制器使能（237~296 行）

### 3.1 doorbell 门铃（237-245 行）

主机和设备之间**唯一的"通知"手段就是写 doorbell 寄存器**：

```c
static void __iomem *nvme_sq_doorbell(struct pcie_nvme_dev *dev, u16 qid)
{
	return dev->bar0 + NVME_REG_DBS + (2 * qid) * dev->dstrd;
}
static void __iomem *nvme_cq_doorbell(struct pcie_nvme_dev *dev, u16 qid)
{
	return dev->bar0 + NVME_REG_DBS + (2 * qid + 1) * dev->dstrd;
}
```

每个队列有 **2 个门铃**：SQ tail（主机说"我放了新命令"）和 CQ head（主机说"我读完了，这位置可以复用"）。内存布局：

```
DBS + 0    qid=0 的 SQ tail doorbell
DBS + 4    qid=0 的 CQ head doorbell
DBS + 8    qid=1 的 SQ tail doorbell
DBS + 12   qid=1 的 CQ head doorbell
...
```

间距 `dstrd` 从 CAP 读出（QEMU 为 4 字节）。

**为什么叫 doorbell？** 类比餐厅：主机（顾客）写 SQ 就像把点单放进传送带，敲门铃（写 SQ tail）通知后厨（设备）"有单了"；后厨做好放 CQ，主机吃完敲 CQ head 门铃"盘子收走了"。**没有中断也照样能工作**——后厨出餐快，顾客一直盯着（轮询）也行，这就是阶段 3 的轮询模式；阶段 4 才加上中断（服务员叫号）。

### 3.2 `nvme_enable_ctrl`（247-286 行）—— 控制器的"开机五步"

```c
static int nvme_enable_ctrl(struct pcie_nvme_dev *dev)
{
	u64 cap = readq(dev->bar0 + NVME_REG_CAP);
	...
	/* 第1步：检查 CSTS.RDY，防止重复初始化 */
	csts = readl(dev->bar0 + NVME_REG_CSTS);
	if (csts & NVME_CSTS_RDY) { ... return -EBUSY; }

	/* 第2步：写 AQA = Admin 队列深度（32-1 写进低 16 位和高 16 位） */
	writel(((NVME_ADMIN_Q_DEPTH - 1) << NVME_AQA_ASQS_SHIFT) |
	       ((NVME_ADMIN_Q_DEPTH - 1) << NVME_AQA_ACQS_SHIFT),
	       dev->bar0 + NVME_REG_AQA);
	/* 第3步：写 ASQ/ACQ = Admin SQ/CQ 的总线地址 */
	writeq(dev->admin_sq_dma, dev->bar0 + NVME_REG_ASQ);
	writeq(dev->admin_cq_dma, dev->bar0 + NVME_REG_ACQ);

	/* 第4步：写 CC.EN=1 —— 这是"开机键"，同时配好 MPS/CSS/IOSQES/IOCQES */
	cc = NVME_CC_ENABLE | ...;
	writel(cc, dev->bar0 + NVME_REG_CC);

	/* 第5步：轮询 CSTS.RDY 直到置 1（带超时），再检查致命错误位 CFS */
	timeout = jiffies + msecs_to_jiffies((NVME_CAP_TO(cap) + 1) * 500);
	while (!((csts = readl(dev->bar0 + NVME_REG_CSTS)) & NVME_CSTS_RDY)) {
		if (time_after(jiffies, timeout)) { ... return -ETIMEDOUT; }
		cpu_relax();
	}
	if (csts & NVME_CSTS_CFS) { ... return -EIO; }
	...
}
```

**考点**：
1. ASQ/ACQ 写的是 `admin_sq_dma`（**总线地址**），不是 `admin_sq`（虚拟地址）——设备只认识总线地址。
2. AQA 存的是"深度-1"（31），规范规定。
3. 超时时间用 CAP.TO 算：`(TO+1)×500ms`，不拍脑袋。
4. `cpu_relax()` 是轮询礼貌（ARM 上是 hint 指令，降功耗），别写成空转 `;`。
5. 顺序不能乱：**先配 AQA/ASQ/ACQ，最后才 EN=1**。EN 一置位设备就开工，邮箱没配好就开工 = 完蛋。

### 3.3 `nvme_disable_ctrl`（288-296 行）—— 关机

```c
writel(0, dev->bar0 + NVME_REG_CC);          // EN=0
while ((csts = readl(dev->bar0 + NVME_REG_CSTS)) & NVME_CSTS_RDY)
	cpu_relax();                              // 等 RDY 熄灭
```

**口述自测**：一条队列有几个 doorbell？各敲在什么时机？为什么使能控制器的顺序是先 AQA/ASQ/ACQ 后 CC.EN？

---

## 第 4 讲：Admin 命令往返（298~338 行）—— 第一次"对话"

### 4.1 `nvme_admin_wait_cq`（299-327 行）—— 轮询等结果

```c
static int nvme_admin_wait_cq(struct pcie_nvme_dev *dev, u32 *result)
{
	struct nvme_cqe *cqe = &dev->admin_cq[dev->cq_head];   // 当前该读的位置
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);
	u16 status;

	/* 轮询：CQE 的 P 位 == 期望的 phase，说明设备已写入新结果 */
	while ((le16_to_cpu(cqe->status) & 0x1) != dev->cq_phase) {
		if (time_after(jiffies, timeout)) { ... return -ETIMEDOUT; }
		cpu_relax();
	}

	/* status >> 1 去掉 P 位得到状态码，非 0 即失败 */
	status = le16_to_cpu(cqe->status) >> 1;
	if (status) { ... return -EIO; }

	if (result)
		*result = le32_to_cpu(cqe->result);    // 取出结果（如 Identify 的数据长度）

	/* 消费掉这条 CQE：head+1，转一圈翻转 phase，敲 CQ head doorbell */
	dev->cq_head = (dev->cq_head + 1) % NVME_ADMIN_Q_DEPTH;
	if (dev->cq_head == 0)
		dev->cq_phase = !dev->cq_phase;
	writel(dev->cq_head, nvme_cq_doorbell(dev, 0));

	return 0;
}
```

**注意 `le16_to_cpu`**：SQE/CQE 是设备读写的，NVMe 规范规定**小端**；ARM64 跑 Linux 默认小端，此时代价为 0，但写对大小端是协议驱动的职业素养（大端平台如部分 MIPS 上不转换就是灾难）。

### 4.2 `nvme_admin_cmd`（330-338 行）—— 提交 + 等待

```c
static int nvme_admin_cmd(struct pcie_nvme_dev *dev, const struct nvme_sqe *sqe,
			  u32 *result)
{
	memcpy(&dev->admin_sq[dev->sq_tail], sqe, sizeof(*sqe));  // 1. 命令放进 SQ
	dev->sq_tail = (dev->sq_tail + 1) % NVME_ADMIN_Q_DEPTH;   // 2. tail 前进
	writel(dev->sq_tail, nvme_sq_doorbell(dev, 0));           // 3. ★ 敲 SQ tail 门铃

	return nvme_admin_wait_cq(dev, result);                   // 4. 等结果
}
```

**完整时序图**（一条 Admin 命令的一生，共 4 次门铃/2 类门铃）：

```
主机                         设备
  │ memcpy(SQ[tail]=命令)       │
  │ writel(SQ tail doorbell) ──▶│ 看到门铃，取走命令，执行
  │                             │ 执行完：写 CQ[head]=结果(P=phase)
  │ ◀───────────────────────────│ （阶段4起：发 MSI-X 中断）
  │ 轮询/中断发现 P 匹配         │
  │ 读结果，head+1               │
  │ writel(CQ head doorbell) ──▶│ 知道该位置可复用
```

**阶段 3 是纯轮询（poll）**：Admin 命令都在初始化时发、次数少，轮询简单可靠，不值得开中断。阶段 4 之后 I/O 队列才用中断。**考点**：为什么 Admin 队列不配中断？——初始化阶段同步轮询更简单；且 Admin 命令少、延迟不敏感。

**口述自测**：`writel(dev->sq_tail, ...)` 这一行的作用是什么？不敲会怎样？（答：设备不知道有新命令，命令躺在 SQ 里永远不会被执行。）

---

## 第 5 讲：Identify 读设备信息（340~407 行）

Identify 是 Admin 队列跑的第一个命令，相当于**盘问设备**："你是谁？多大？"

### 5.1 Identify Controller（354-379 行）—— 问"你是谁"

```c
sqe.opcode = NVME_ADMIN_IDENTIFY;
sqe.command_id = 1;
sqe.nsid = 0;
sqe.prp1 = cpu_to_le64(dev->identify_dma);   // 结果写到这个 DMA 缓冲
sqe.cdw10 = cpu_to_le32(0x1);                // CNS=1：要 Controller 信息
```

设备把 4096 字节的 Identify 数据 DMA 进 `dev->identify` 缓冲。`nvme_dump_identify_ctrl`（367）从中抠字段：VID（厂商）、MN（型号）、SN（序列号）、FR（固件版本）、NN（命名空间数量）。

**坑 3 就在这**（371 行注释）：NN 字段在 offset **516**，不是 512。因为结构体前面有 4 字节对齐填充。这个坑的教训：**协议字段偏移要对着规范结构体核对，不能凭"感觉下一个是 512"猜**。

### 5.2 Identify Namespace（382-407 行）—— 问"你多大"

```c
sqe.nsid = cpu_to_le32(dev->nsid);  // 问 1 号命名空间
sqe.cdw10 = cpu_to_le32(0x0);       // CNS=0：要 Namespace 信息
```

`nvme_parse_ns`（395）抠出两个关键数：

```c
u64 nsze = le64_to_cpu(*(__le64 *)(dev->identify + 0));   // LBA 总数
u8 flbas  = *(u8 *)(dev->identify + 26);                  // 当前 LBA 格式索引
u8 lbaf_idx = flbas & 0xf;
u32 lbaf = le32_to_cpu(*(__le32 *)(dev->identify + 128 + lbaf_idx * 4));
u8 lbads = (lbaf >> 16) & 0xff;                           // LBA 大小 = 2^lbads

dev->nsze = nsze;        // 比如 QEMU 默认 1G 盘 = 2097152 个 512B LBA
dev->lba_shift = lbads;  // 9（512B）或 12（4KB）
```

**这两个数决定 /dev/nvme0n1 的容量**（851 行 `set_capacity(disk, nsze << (lba_shift-9))` 用的就是它）。

**考点**：LBA 和 512B 扇区的关系。块设备层统一以 512B 扇区为单位（`blk_rq_pos` 返回扇区号），NVMe 的 LBA 可能更大（4KB 盘）。所以全驱动凡涉及地址/长度都要 `>> (lba_shift - 9)` 换算——754 行、767 行都是。

**口述自测**：Identify 的数据是怎么到主机内存的？`lba_shift=12` 时，块层给的扇区号 4096 对应 NVMe 的 LBA 几？

---

## 第 6 讲：创建 I/O 队列（409~501 行）

Admin 队列只跑管理命令，**自己不下场搬数据**。真正干读写的是 I/O 队列——由 Admin 发命令创建。

### 6.1 Create I/O CQ（410-423 行）

```c
sqe.opcode = NVME_ADMIN_CREATE_CQ;      // opcode 0x05
sqe.prp1 = cpu_to_le64(q->cq_dma);      // CQ 内存的总线地址
sqe.cdw10 = cpu_to_le32((q->qid & 0xffff) | ((q->q_depth - 1) << 16));
                                        // 低16位=qid，高16位=深度-1
sqe.cdw11 = cpu_to_le32(0x3 | ((q->qid - 1) << 16));
                                        // ★ PC=1 物理连续, IEN=1 使能中断,
                                        //   IV=qid-1 绑定第 qid-1 号 MSI-X 向量
```

**`cdw11` 是本项目最精妙的参数之一**：
- `PC=1`：CQ 内存物理连续（`dmam_alloc_coherent` 保证），所以 PRP1 一个地址就能描述整个 CQ
- `IEN=1`：这个 CQ 完成时**发中断**（Admin 队列没设这个位，所以是轮询）
- `IV=qid-1`：**每个 I/O 队列绑一个独立的 MSI-X 向量**——中断到来时按向量号直接知道是哪个队列完成的。这是多队列性能的根基（阶段 5），也是面试必问的"NVMe 怎么把中断分到多个 CPU"的答案。

### 6.2 Create I/O SQ（426-439 行）

```c
sqe.opcode = NVME_ADMIN_CREATE_SQ;      // opcode 0x01
sqe.prp1 = cpu_to_le64(q->sq_dma);
sqe.cdw10 = cpu_to_le32((q->qid & 0xffff) | ((q->q_depth - 1) << 16));
sqe.cdw11 = cpu_to_le32(0x1 | (q->qid << 16));  // PC=1, CQID=qid：SQ 关联到哪个 CQ
```

**顺序必须是先 CQ 后 SQ**（NVMe 规范）：SQ 要挂到 CQ 上，CQ 不存在就创建 SQ 会失败。

### 6.3 `nvme_alloc_io_queue`（458-501 行）—— 一个队列对的完整诞生

```c
q->irq = pci_irq_vector(dev->pdev, qid - 1);     // 向量号 = qid-1（对应上文的 IV）
q->tags = dev->tag_set.tags[qid - 1];            // 借 blk-mq 的 tag 表做映射

q->sq = dmam_alloc_coherent(&dev->pdev->dev,
		q->q_depth * sizeof(struct nvme_sqe), &q->sq_dma, GFP_KERNEL);
q->cq = dmam_alloc_coherent(...);                // 队列内存：一致性 DMA

if ((q->sq_dma & 0xfff) || (q->cq_dma & 0xfff)) { ... return -EIO; }  // 4KB 对齐检查

nvme_create_io_cq(dev, q);   // 1. 先 CQ
nvme_create_io_sq(dev, q);   // 2. 后 SQ

request_irq(q->irq, nvme_irq, 0, "nvme-io", q);  // 3. 绑中断（dev_id=q 指针）
```

三个细节：
1. **`dmam_alloc_coherent`**：devres 托管版的一致性 DMA 分配，remove 时自动释放（1070 行注释）；返回的 `sq_dma` 保证物理连续、4KB 对齐、cache 一致性。
2. **4KB 对齐检查是防御性编程**：coherent 分配理论上一定对齐，但驱动对协议约束（PRP 要求页对齐）显式校验，不合格就拒绝——真实项目该有的态度。
3. **`request_irq` 的 `dev_id=q`**：中断处理函数 `nvme_irq(irq, data)` 的 `data` 就是它，中断来了直接拿到队列指针，不用查表。

**队列拓扑图**（2 核 QEMU 实际拓扑）：

```
         Admin 队列对 (qid=0, 深度32, 轮询)
              │ 创建 ↓
   ┌──────────┼──────────┐
 qid=1              qid=2           (每 CPU 一对)
 SQ↔CQ 深度64       SQ↔CQ 深度64
 irq=vec0          irq=vec1
 └── blk-mq hctx0 ──┴── blk-mq hctx1 ──┘
        ↓ tag 映射
 /dev/nvme0n1（一个 gendisk，多 hw queue）
```

**口述自测**：为什么创建顺序是先 CQ 后 SQ？`IV=qid-1` 这个绑定解决了什么问题？

---

## 第 7 讲：blk-mq 数据通路（739~807 行）—— 框架的入口

### 7.1 `nvme_queue_rq`（775-791 行）—— blk-mq 框架的回调

```c
static blk_status_t nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct nvme_queue *q = hctx->driver_data;   // init_hctx 时挂上去的队列指针
	struct request *rq = bd->rq;
	struct nvme_cmd *cmd = blk_mq_rq_to_pdu(rq);  // 拿到"草稿纸"
	blk_status_t ret;

	blk_mq_start_request(rq);   // 标记开始（超时计时从这里起算）

	ret = nvme_map_and_submit(q, rq, cmd);   // 翻译成 SQE 并提交
	if (ret != BLK_STS_OK)
		nvme_unmap_data(q->dev, cmd, rq);   // 失败就地清理

	return ret;   // BLK_STS_OK = "已提交，不等结果"（异步）
}
```

**关键认知**：`queue_rq` 运行在**原子上下文（自旋锁内），不能睡眠**。所以里面的分配要用 `GFP_ATOMIC`（533 行的 PRP 池分配），映射失败要当场清理。返回 `BLK_STS_OK` 不代表 IO 完成，只代表"已送达设备"——真正的完成在中断里。

### 7.2 `nvme_map_and_submit`（739-773 行）—— request → SQE 的翻译器

```c
static blk_status_t nvme_map_and_submit(struct nvme_queue *q,
					struct request *rq, struct nvme_cmd *cmd)
{
	struct nvme_sqe sqe = { 0 };

	cmd->prp_list = NULL;
	cmd->nsegs = 0;

	if (blk_rq_bytes(rq) == 0) {
		/* flush 等零数据命令：不用 PRP，直接发 FLUSH */
		sqe.opcode = NVME_CMD_FLUSH;
		sqe.nsid = cpu_to_le32(dev->nsid);
	} else {
		/* 扇区号 → NVMe LBA：块层 512B 扇区，NVMe 按 lba_shift 算 */
		u64 slba = blk_rq_pos(rq) >> (dev->lba_shift - 9);

		ret = nvme_setup_prps(q, cmd, rq, &sqe);   // 建 PRP（第 8 讲）
		if (ret != BLK_STS_OK)
			return ret;

		sqe.opcode = (rq_data_dir(rq) == WRITE) ?
			     NVME_CMD_WRITE : NVME_CMD_READ;
		sqe.nsid = cpu_to_le32(dev->nsid);
		sqe.cdw10 = cpu_to_le32(slba & 0xffffffff);   // SLBA 低 32 位
		sqe.cdw11 = cpu_to_le32(slba >> 32);          // SLBA 高 32 位
		sqe.cdw12 = cpu_to_le32(
			(blk_rq_sectors(rq) >> (dev->lba_shift - 9)) - 1);  // NLB-1
	}
	sqe.command_id = rq->tag;   // ★ command_id 直接复用 blk-mq 的 tag

	nvme_submit_sqe(q, &sqe);   // 落 SQ + 敲门铃
	return BLK_STS_OK;
}
```

**`sqe.command_id = rq->tag` 是全驱动的"对号机制"**：blk-mq 给每个 request 发一个唯一的 tag（0~queue_depth-1 的编号），提交时写进 SQE；设备完成后原样写回 CQE 的 command_id；中断里 `blk_mq_tag_to_rq(tags, command_id)` 反查 request。**一个字段打通提交与完成，零额外内存**——这就是 blk-mq 和 NVMe 天生合拍的原因（两者都是"命令带编号"模型）。面试必问。

### 7.3 `nvme_submit_sqe`（573-578 行）

```c
static void nvme_submit_sqe(struct nvme_queue *q, struct nvme_sqe *sqe)
{
	memcpy(&q->sq[q->sq_tail], sqe, sizeof(*sqe));   // 命令进 SQ
	q->sq_tail = (q->sq_tail + 1) % q->q_depth;      // tail 前进
	writel(q->sq_tail, nvme_sq_doorbell(q->dev, q->qid));  // 敲门铃
}
```

和 Admin 版（330 行）逻辑相同，但**不等待**——提交完直接返回，这是阶段 4 异步化的核心改动。

**口述自测**：`queue_rq` 为什么不能睡眠？`command_id = rq->tag` 为什么可行？

---

## 第 8 讲：PRP 构造（506~578 行）—— 数据通路的灵魂

**PRP（Physical Region Page）**：NVMe 规范规定，命令里描述数据缓冲区的方式是"页对齐的物理页指针列表"。因为 NVMe 是 DMA 设备，它要的是**物理页地址**，不是虚拟地址。

### 8.1 两种情况的布局图

**单页**（数据 ≤ 4KB）：`prp1` 直接指向数据页，`prp2=0`：

```
sqe.prp1 ──▶ [ 数据页 (DMA 地址) ]
sqe.prp2 = 0
```

**多页**（数据 > 4KB）：`prp1` = 第一页；`prp2` = 指向一个 **PRP list 页**，页内依次存第 2~N 页的地址：

```
sqe.prp1 ──▶ [ 数据页1 ]
sqe.prp2 ──▶ [ PRP list 页（512 个 8B 条目） ]
              条目0 → 数据页2
              条目1 → 数据页3
              ...
              条目k → 数据页N
```

一页 4096B 放 512 个条目，所以单 PRP list 最多描述 513 页 ≈ 2MB——这正是 854 行 `blk_queue_max_hw_sectors(4096)` 的来历（块层保证一个请求不超过 2MB，PRP list 一页就够）。**这是一个漂亮的联动**：硬件协议限制 → 驱动给块层的声明 → 块层保证约束，全链路闭环。

### 8.2 `nvme_setup_prps`（506-558 行）逐步走

```c
/* 第1步：request 的 bio 链表 → sg 表（scatterlist：多个不连续内存段） */
cmd->nsegs = blk_rq_map_sg(rq->q, rq, cmd->sg);
/* 第2步：dma_map_sg —— 映射并返回总线地址（可能合并段、可能用 IOMMU） */
cmd->nsegs = dma_map_sg(&dev->pdev->dev, cmd->sg, cmd->nsegs,
			rq_data_dir(rq));
/* 第3步：单页直接填 prp1，完事 */
if (length <= PAGE_SIZE) {
	sqe->prp1 = cpu_to_le64(sg_dma_address(&cmd->sg[0]));
	sqe->prp2 = 0;
	return BLK_STS_OK;
}
/* 第4步：多页——从 dma_pool 借一个 PRP list 页（原子上下文，GFP_ATOMIC） */
cmd->prp_list = dma_pool_alloc(dev->prp_pool, GFP_ATOMIC, &cmd->prp_list_dma);
if (!cmd->prp_list)
	return BLK_STS_RESOURCE;   // 借不到页：资源不足，明确报错

/* 第5步：逐段逐页把地址填进 list（跳过 prp1 已指向的第一页） */
prp_list = cmd->prp_list;
nprps = 0;
for_each_sg(cmd->sg, sg, cmd->nsegs, i) {
	dma_addr_t dma = sg_dma_address(sg);
	int npages = DIV_ROUND_UP(sg_dma_len(sg), PAGE_SIZE);
	for (j = 0; j < npages; j++) {
		if (first) { first = 0; continue; }   // 跳过第一页
		prp_list[nprps++] = cpu_to_le64(dma + j * PAGE_SIZE);
	}
}
sqe->prp2 = cpu_to_le64(cmd->prp_list_dma);   // prp2 = list 页的地址
```

三个考点：
1. **`blk_rq_map_sg` + `dma_map_sg` 两步缺一不可**：前者是"逻辑上把 bio 拆成段"，后者是"映射成总线地址"。用 `sg_dma_address/sg_dma_len` 取映射后的地址，不能用 `sg_phys`（IOMMU 平台下两者不同）。
2. **`dma_pool`**：预建的一页大小的池子。为什么不用每次 `dma_alloc_coherent` 一页？——`queue_rq` 在原子上下文，且 IO 频繁，池化分配快且无碎片。PRP list 页用完还回池（565 行 `dma_pool_free`），**页本身是共享的，用的时候借用，用完归还**。
3. **PRP 要求数据页对齐、长度页整数倍**：块设备 IO 天然满足（页缓存按页管理），所以按页拆分安全。真实 NVMe 驱动还要处理非对齐场景（页内偏移要拆 PRP 链），本项目用块层约束规避了。

### 8.3 `nvme_unmap_data`（561-571 行）—— 完成后的清理

```c
if (cmd->prp_list) {
	dma_pool_free(dev->prp_pool, cmd->prp_list, cmd->prp_list_dma);  // 还池
	cmd->prp_list = NULL;
}
if (cmd->nsegs > 0)
	dma_unmap_sg(&dev->pdev->dev, cmd->sg, cmd->nsegs, rq_data_dir(rq));  // 解映射
```

映射是借，解映射是还。**方向参数两次必须一致**（map 时 `rq_data_dir(rq)`，unmap 时也是）——DMA 方向影响 cache 维护操作，写反了在非一致性平台上会数据损坏。这类"成对 API 参数必须一致"是驱动面试高频陷阱题。

**口述自测**：为什么 max_hw_sectors 设 4096（2MB）？PRP list 页为什么用 dma_pool 而不是每次现分配？

---

## 第 9 讲：中断完成（626~703 行）—— 上半部与下半部

阶段 4 起，I/O 完成不再轮询，改走中断。设计分两层：

```
设备写 CQE ──▶ MSI-X 中断 ──▶ 上半部 nvme_irq（硬中断）
                                  │ 只做：收割 CQ + 记状态 + blk_mq_complete_request
                                  ▼
                           blk-mq 软中断（BLOCK softirq）
                                  │ 调 .complete 回调
                                  ▼
                          下半部 nvme_mq_complete（软中断上下文）
                                  只做：unmap + end_request
```

**为什么分两层？** 硬中断期间本 CPU 别的活全停，所以要快。unmap（cache 操作、池归还）相对重，放软中断做。对标内核 `drivers/nvme/host/pci.c` 的相同设计。

### 9.1 `nvme_irq`（695-703 行）—— 硬中断入口

```c
static irqreturn_t nvme_irq(int irq, void *data)
{
	struct nvme_queue *q = data;   // request_irq 时传入的队列指针

	if (irq_loss)
		return IRQ_NONE;   // ★ 故障注入开关：模拟丢中断（验证超时恢复用）

	return nvme_process_cq(q) ? IRQ_HANDLED : IRQ_NONE;
}
```

`IRQ_HANDLED` vs `IRQ_NONE` 是中断协议：没收割到任何 CQE 就报 NONE（可能是共享中断里别的设备的中断，让内核继续找别的 handler）。**`irq_loss` 是本项目最妙的设计**——驱动自带故障注入，一个模块参数就能演练"中断丢了怎么办"，这是"过程可追溯"的工程证据（阶段 5b 的验证手段）。

### 9.2 `nvme_process_cq`（671-692 行）—— 收割 CQ

```c
static int nvme_process_cq(struct nvme_queue *q)
{
	struct nvme_cqe *cqe;
	int handled = 0;

	for (;;) {
		cqe = &q->cq[q->cq_head];
		if ((le16_to_cpu(cqe->status) & 0x1) != q->cq_phase)
			break;   // P 位不匹配：没有新结果了，收工

		handled++;
		nvme_complete_rq(q, cqe);   // 处理这一条

		q->cq_head = (q->cq_head + 1) % q->q_depth;
		if (q->cq_head == 0)
			q->cq_phase = !q->cq_phase;   // 转一圈，翻转期望相位
	}

	if (handled)
		writel(q->cq_head, nvme_cq_doorbell(q->dev, q->qid));  // 批量释放 CQ 位置
	return handled;
}
```

**为什么用 `for(;;)` 一次收完所有就绪的 CQE？** 一次中断可能对应多条完成（设备攒批），只收一条就浪费中断。收完统一敲一次 CQ head doorbell——**门铃按批敲，不按条敲**，这是高性能驱动的标准姿势。

### 9.3 `nvme_complete_rq`（627-657 行）—— 把 CQE 对上 request

```c
u16 command_id = le16_to_cpu(cqe->command_id);

/* command_id 即提交时的 rq->tag，blk-mq 反向查 request */
rq = blk_mq_tag_to_rq(q->tags, command_id);
if (unlikely(!rq)) { ... return; }   // 找不到：防御（正常不会发生）

cmd = blk_mq_rq_to_pdu(rq);
cmd->status = status;   // 记下状态，下半部读

if (status) { ... }     // 失败：打日志（sct/sc/dnr 全拆开，方便定位）

blk_mq_complete_request(rq);   // ★ 触发异步完成
```

**`blk_mq_complete_request` 是"提交 → 完成"闭环的另一半**：它把 request 推进完成流程（多数情况下排队到软中断，调 `.complete` 回调）。

### 9.4 `nvme_mq_complete`（660-668 行）—— 下半部收尾

```c
static void nvme_mq_complete(struct request *rq)
{
	struct pcie_nvme_dev *dev = rq->q->queuedata;
	struct nvme_cmd *cmd = blk_mq_rq_to_pdu(rq);
	blk_status_t ret = nvme_error_status(cmd->status);  // 状态码 → blk 返回码

	nvme_unmap_data(dev, cmd, rq);   // 还 PRP 页、解 sg 映射
	blk_mq_end_request(rq, ret);     // 真正结束 request（唤醒等待者、回收 tag）
}
```

到 `blk_mq_end_request`，一条 IO 才算完整落幕：tag 回收（后续请求可复用这个 tag——**这就是 command_id 能复用的闭环**）、bio 回报结果、page cache 唤醒。

**口述自测**：为什么不把 unmap 放中断上半部？`blk_mq_end_request` 之前 tag 能不能给新请求用？

---

## 第 10 讲：超时处理（705~737 行）—— 阶段 5b 的精华

`tag_set.timeout = io_timeout * HZ`（832 行）声明"每条命令 30 秒"，超时未完成，块层会调 `.timeout` 回调：

```c
static enum blk_eh_timer_return nvme_timeout(struct request *rq)
{
	struct nvme_queue *q = rq->mq_hctx->driver_data;
	...
	dev_warn(..., "I/O 命令超时: qid=%u tag=%u dir=%s，主动轮询 CQ 恢复\n", ...);

	/* ★ 超时≠设备死了。最常见原因是「中断丢了」：
	 * 设备已写 CQE，但中断没被处理。所以第一反应不是杀命令，
	 * 而是主动收割一遍 CQ——把已就绪的命令补完成。 */
	nvme_process_cq(q);

	/* 判断本请求是否已结束：用「不在 IN_FLIGHT」判断。
	 * 不能用 blk_mq_request_completed()——本项目多队列下完成是同步路径，
	 * 完成后 state 被重置为 MQ_RQ_IDLE 而非 MQ_RQ_COMPLETE（坑 9）。 */
	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)
		return BLK_EH_DONE;      // 已补完成，无事发生

	dev_err(..., "命令仍卡住: ... 无 CQE，重置计时器继续等待...\n");
	return BLK_EH_RESET_TIMER;   // 真卡住：再给一次机会（进阶：Abort/复位）
}
```

三个层次：
1. **设计哲学**："超时先怀疑中断，不怀疑设备"——poll CQ 一步区分了"中断丢了（可恢复）"和"设备真卡死（不可恢复）"，这是生产级驱动的判断力。`irq_loss=1` 模块参数就是专门演练这个场景的。
2. **坑 9**：`blk_mq_request_completed()` 在**多队列非 IPI 完成路径**下是假阴性——request 完成后 state 被重置为 `MQ_RQ_IDLE`（不是 `MQ_RQ_COMPLETE`），用它判断会误报"还在飞"。改判 `!= MQ_RQ_IN_FLIGHT`（IDLE 和 COMPLETE 都算结束）。这个坑极能体现"你读过内核实现、调试过状态机"。
3. **`BLK_EH_RESET_TIMER`** 是"缓刑"：再等 30 秒。生产级会进一步发 Abort 命令或复位控制器（`NVME_CTRL_RESETTING` 流程），本项目在注释里如实说明边界——面试时主动说"我知道更完整的做法是什么"，反而加分。

**口述自测**：超时后第一件事为什么是 `nvme_process_cq`？`blk_mq_request_completed` 在本项目为什么不能用？

---

## 第 11 讲：错误码细分（584~624 行）—— 阶段 5c

CQE 的 `status`（16 位，`>>1` 后）编码：

```
bit 14      DNR（Don't Retry：不要重试）
bit 13:12   M（More：更多状态）/ CRD（重试延迟）
bit 11:9    SCT（Status Code Type：错误大类）
bit 8:0     SC（Status Code：具体错误）
```

`switch (status & 0x7ff)` 就是**屏蔽 M/DNR/CRD，只按 `SCT<<8|SC` 分类**。为什么必须用完整编码？因为**同一个 SC 值在不同 SCT 下含义完全不同**：

```
SC=0x02  SCT=0（通用）→ INVALID_FIELD（命令字段非法）
SC=0x02  SCT=2（介质）→ 是别的错误
SC=0x80  SCT=2（介质）→ LBA_RANGE（访问越界）
SC=0x80  SCT=1（命令）→ 完全是另一种错
```

所以宏定义里 `NVME_SC_LBA_RANGE = 0x80`、`NVME_SC_WRITE_FAULT = 0x280`——**值是 `SCT<<8|SC` 的完整编码，不是裸 SC**（91 行注释明确写了）。这是新手最容易抄错的点。

映射逻辑（对标内核 `drivers/nvme/host/core.c:252 nvme_error_status`）：

| NVMe 错误 | blk_status_t | 上层表现 |
|---|---|---|
| CAP_EXCEEDED | NOSPC | ENOSPC（磁盘满） |
| WRITE_FAULT/READ_ERROR 等 | MEDIUM | EIO（介质坏） |
| GUARD/APPTAG/REFTAG_CHECK | PROTECTION | 校验保护错误 |
| HOST_PATH_ERROR | TRANSPORT | 链路错误 |
| 其他 | IOERR | 兜底 EIO |

**意义**：块层拿 `blk_status_t` 转成用户态 errno——`dd` 写满盘时用户看到的是 `No space left on device` 而不是笼统的 `I/O error`。错误语义越精确，上层应用和运维越能对症下药。

**口述自测**：`status & 0x7ff` 为什么是 0x7ff？`LBA_RANGE` 为什么编码成 0x80 而不是 0x0？

---

## 第 12 讲：blk-mq 框架接入（802~863 行）

### 12.1 `nvme_mq_ops`（802-807 行）—— 给框架的回调集

```c
static const struct blk_mq_ops nvme_mq_ops = {
	.queue_rq  = nvme_queue_rq,   // 有新请求：提交给设备
	.complete  = nvme_mq_complete,// 完成时：收尾（软中断）
	.init_hctx = nvme_init_hctx,  // 每个硬件队列初始化：挂 driver_data
	.timeout   = nvme_timeout,    // 超时：恢复或放弃
};
```

**`init_hctx`（793-800）是关键细节**：`hctx->driver_data = &dev->queues[hctx_idx]`——blk-mq 的硬件队列和 NVMe 的 I/O 队列**下标一一对应**。之后 `queue_rq` 从 `hctx->driver_data` 拿到队列，`timeout` 从 `rq->mq_hctx->driver_data` 拿到队列。一个指针打通三个回调。

### 12.2 `nvme_setup_disk`（813-863 行）—— tag_set 字段逐项

```c
dev->tag_set.ops            = &nvme_mq_ops;
dev->tag_set.nr_hw_queues   = dev->nr_queues;   // 几个硬件队列（= I/O 队列数）
dev->tag_set.queue_depth    = NVME_IO_Q_DEPTH;  // 每队列 64 个 tag
dev->tag_set.numa_node      = NUMA_NO_NODE;
dev->tag_set.flags          = BLK_MQ_F_SHOULD_MERGE;  // 允许合并相邻 bio
dev->tag_set.cmd_size       = sizeof(struct nvme_cmd); // pdu 大小（草稿纸）
dev->tag_set.driver_data    = dev;
dev->tag_set.timeout        = io_timeout * HZ;   // 30 秒超时

ret = blk_mq_alloc_tag_set(&dev->tag_set);   // 分配 tag 表（每队列 64 个）

disk = blk_mq_alloc_disk(&dev->tag_set, dev); // 创建 gendisk + request_queue
...
disk->fops = &nvme_fops;             // 空 fops（块设备全靠队列，不用 open/read）
snprintf(disk->disk_name, DISK_NAME_LEN, "nvme%dn%d", 0, dev->nsid);  // 名字
set_capacity(disk, dev->nsze << (dev->lba_shift - 9));  // 容量（扇区数）
blk_queue_logical_block_size(disk->queue, 1u << dev->lba_shift);
blk_queue_physical_block_size(disk->queue, 1u << dev->lba_shift);
blk_queue_max_hw_sectors(disk->queue, 4096);  // ★ 2MB：与 PRP list 一页呼应
```

**块设备 vs 字符设备的框架差异在这张表里最直观**：字符设备注册 `file_operations` 的 open/read/write；块设备注册 `blk_mq_ops` 的 queue_rq，**连 open 都没有**——块层框架替你做了所有通用工作（缓存、调度、合并），驱动只管"把请求翻译成设备命令"。`disk->fops` 为空结构体就是证据。

**考点**：`BLK_MQ_F_SHOULD_MERGE` 去掉会怎样？性能下降——相邻 bio 不合并，每个 4K 都单独发命令，IOPS 浪费在命令开销上（fio 数据会难看）。阶段 6 的 fio 对比里 numjobs 扩展 IOPS 的前提就是合并+多队列共同作用。

**口述自测**：`hctx->driver_data` 是谁挂上去的？`cmd_size` 的作用是什么？

---

## 第 13 讲：probe / remove 生命周期（865~1082 行）

### 13.1 probe 十六步（865-1037 行）

PCIe 枚举发现 vendor/device ID 匹配的设备后，内核自动调 probe。**顺序就是资源依赖顺序**：

| 步 | 动作 | 行 | 作用/考点 |
|---|---|---|---|
| 1 | `pci_enable_device_mem` | 876 | 开 MEM 空间访问 |
| 2 | `pci_set_master` | 882 | 开 Bus Master（DMA 前提，坑 2） |
| 3 | `kzalloc(dev)` + `pci_set_drvdata` | 884-890 | 总管家挂到 pdev 上 |
| 4 | `pci_resource_start/len` 读 BAR0 | 892-898 | 拿 MMIO 基址/长度 |
| 5 | `pci_request_mem_regions` | 900 | 独占 BAR0（防别的驱动抢） |
| 6 | `devm_ioremap` | 906 | **物理地址 → 内核虚拟地址**（此后 CPU 能访问寄存器） |
| 7 | 读 CAP/VS/CSTS | 913-918 | 记录 DSTRD 等能力 |
| 8 | 分配 Admin SQ/CQ/Identify（coherent） | 921-946 | 含 4KB 对齐防御检查 |
| 9 | `nvme_enable_ctrl` | 952 | 开机（第 3 讲五步） |
| 10 | Identify Controller + 打印 | 957-960 | 验证链路 + 信息 |
| 11 | Identify Namespace | 963-967 | 拿容量与 LBA 格式 |
| 12 | `dma_pool_create` | 970 | PRP 页池 |
| 13 | 算队列数 `min(CPU 数, MQES+1)` | 978 | 每 CPU 一队列 |
| 14 | `pci_alloc_irq_vectors` | 981-989 | ★ 先定中断数，再定队列数（实际分配的向量数可能少于请求） |
| 15 | `nvme_setup_disk` + 循环建 I/O 队列 | 992-1001 | blk-mq + 队列对 |
| 16 | `add_disk` | 1003 | ★ 最后一步：此后 /dev/nvme0n1 可见、IO 可达 |

**两个"为什么是这个顺序"（面试高频）**：
1. **`pci_alloc_irq_vectors` 在 `nvme_setup_disk` 之前**：`nr_queues` 是 tag_set 的核心参数，必须等实际分到多少中断向量后才能定死。中断不够（比如系统 MSI-X 向量耗尽）就少建队列——降级而不是失败。
2. **`request_irq` 在 `add_disk` 之前**（491 行注释）：add_disk 之后用户态立刻能读写，如果中断还没绑，第一条 IO 就永远等不到完成。

### 13.2 错误处理 goto 链（1011-1036 行）

```
out_free_irq:         反删已建 I/O 队列 + free_irq
out_free_irq_vectors: pci_free_irq_vectors
out_destroy_pool:     dma_pool_destroy
out_disable_ctrl:     关控制器
out_release_regions:  释放 BAR0 区域
out_free_dev:         kfree(dev)
out_disable_device:   pci_disable_device
```

**每个 goto 标签只清理"到这个阶段为止已成功的资源"**——这是内核驱动的标准错误处理模式（内核不用 C++ RAII，靠 goto 链保证不泄漏）。面试官看到这个链条就知道你写过正经 probe。

### 13.3 remove 反向清理（1039-1073 行）

```c
del_gendisk(dev->disk);   // 1. 先从块层摘掉（内部 quiesce 队列、等在途 IO 结束）
for (...) {
	free_irq(...);            // 2. 此时无 IO，安全摘中断
	nvme_delete_io_queue(...); // 3. 删 I/O 队列（Admin 命令，控制器还活着）
}
pci_free_irq_vectors(pdev);  // 4. 释放中断向量
blk_mq_free_tag_set(...);    // 5. 释放 tag
dma_pool_destroy(...);       // 6. 毁池
put_disk(...);               // 7. 释放 gendisk
nvme_disable_ctrl(dev);      // 8. 关机（最后关控制器）
kfree(dev->queues); kfree(dev);  // 9. 释放内存（coherent 由 devres 自动清）
```

**顺序哲学 = probe 的镜像**："先摘盘（停新 IO）→ 再摘中断（停完成）→ 再删队列 → 最后关控制器"。`del_gendisk` 放第一步是因为它内部会等所有在途 IO 完成——之后摘中断才安全（不会有"中断处理到一半 handler 被 free"的竞态）。

### 13.4 模块注册（1075-1082 行）

```c
static struct pci_driver pcie_nvme_driver = {
	.name     = "pcie_nvme",
	.id_table = pcie_nvme_id_table,  // vendor/device ID 匹配表
	.probe    = pcie_nvme_probe,
	.remove   = pcie_nvme_remove,
};
module_pci_driver(pcie_nvme_driver);
```

`module_pci_driver` 是宏，展开成 module_init/module_exit 包装——**PCI 驱动没有 open/read，只有 probe/remove**。insmod 时内核扫 PCIe 总线，匹配 id_table 就调 probe；rmmod 时调 remove。

**口述自测**：add_disk 之后发生了什么，让 request_irq 必须在它之前？remove 为什么先 del_gendisk 再 free_irq？

---

## 第 14 讲：一条读请求的完整旅程（串讲所有模块）

以用户 `dd if=/dev/nvme0n1 bs=64K count=1` 为例（假设 2 页数据）：

```
1. VFS 收到 read → page cache 未命中 → 生成 bio
2. bio 进入 blk-mq 的软件队列 → 分配 tag（比如 tag=7）
3. blk-mq 调度到某 hctx → 调 nvme_queue_rq            [775行]
4. blk_mq_start_request（超时计时开始）
5. nvme_map_and_submit                               [739行]
     - slba = 扇区号 >> (lba_shift-9)
     - nvme_setup_prps：bio→sg 表→dma_map_sg          [506行]
       · 64K=16 页：prp1=页0，dma_pool 借一页放页1..15
       · sqe.prp2 = PRP list 页地址
     - sqe.command_id = 7（= tag）
6. nvme_submit_sqe：SQ[tail]=sqe, tail+1, 敲 SQ tail 门铃 [573行]
7. queue_rq 返回 BLK_STS_OK（不等结果）
   —— 设备侧：看到门铃 → DMA 读 SQE → 按 PRP DMA 把 16 页数据写进内存
8. 设备写 CQ[head]={command_id=7, P=phase} → 发 MSI-X 中断
9. 中断 → nvme_irq → nvme_process_cq                   [695/671行]
     - 读到 P 匹配的 CQE，nvme_complete_rq               [627行]
       · blk_mq_tag_to_rq(tags, 7) → 找回 request
       · cmd->status 记录，blk_mq_complete_request
     - 敲 CQ head 门铃（释放位置）
10. 块层软中断 → nvme_mq_complete                      [660行]
      - nvme_error_status：status→BLK_STS_OK
      - nvme_unmap_data：还 PRP 页、unmap sg
      - blk_mq_end_request：回收 tag=7、bio 结束
11. page cache 填充完毕 → dd 读回数据 → 比对 MATCH
```

**如果第 8 步中断丢了**：30 秒后块层调 `nvme_timeout`（706 行）→ 主动 `nvme_process_cq` 收割 → 补完成 → `BLK_EH_DONE`。**数据没丢，只是晚到**——这就是阶段 5b 验证过的结论。

---

## 第 15 讲：真实硬件从头实现路线图（QEMU → 真机）

> 目标：拿到一块真 NVMe SSD 时，知道从哪下手、每一步怎么验证。核心认知——**QEMU 验证的是"逻辑"，真机验证的是"环境"**。驱动代码 90% 不用改，改的是"对接"部分。

### 15.1 QEMU 与真机的 5 个差异（先背这张表）

| # | 差异 | 对本驱动的影响 | 应对 |
|---|---|---|---|
| 1 | vendor/device ID 不同 | 枚举后内核不调我们的 probe | 改 id_table（38-39 行）加真盘 ID |
| 2 | 真机有 IOMMU | `dma_map_sg` 在 QEMU 上近似"直通"，真机上才真正做地址翻译 | 本项目全程标准 DMA API，**天然兼容，不用改**——这正是用标准 API 的意义 |
| 3 | 内核自带 `nvme` 驱动先抢占设备 | 我们的 probe 根本轮不到 | 屏蔽内置驱动（见 15.3） |
| 4 | 中断走真实 GIC ITS / x86 APIC | MSI-X 向量可能分配不足 | probe 14 步已有降级逻辑：向量少 → 少建队列，不失败 |
| 5 | 平台枚举走 ACPI/DT | 与 QEMU 生成的 DT 不同 | 无需改：PCIe 设备自描述（地基 2），驱动层无感 |

### 15.2 从头实现 8 步（对着一块真盘从零写，每步带验证点）

| 步 | 做什么 | 怎么算这步通过 |
|---|---|---|
| 1 | 下载 NVMe 规范，建"寄存器速查表"（抄内核 nvme.h，见 1.6） | 能不看代码说出每个寄存器偏移和位域含义 |
| 2 | 真机跑 `lspci -nn`，确认盘被枚举，记下 ID | 输出里有 "Non-Volatile memory controller" |
| 3 | 改 id_table 加真盘 ID（同时保留 QEMU 的 ID） | 编译通过 |
| 4 | 屏蔽内核自带 nvme 驱动（方法见 15.3） | `lspci -k` 里这块盘不再显示 driver=nvme |
| 5 | insmod 我们的驱动，观察 probe 16 步走到哪 | dmesg 打点：BAR0 映射成功 → Identify 打印出真盘型号/序列号 |
| 6 | 逐阶段验证（对应本项目阶段 1~5c 的验证手段） | 每阶段一个可观测结果：读写 MATCH、fio 出 IOPS、irq_loss=1 能自恢复 |
| 7 | fio 压测 + 故障注入 | IOPS 数字合理；拔中断演练超时恢复 |
| 8 | 准备面试话术（见 15.5） | 能 3 分钟讲完 QEMU→真机全路径 |

### 15.3 屏蔽内核自带 nvme 驱动的三种方法（按推荐排序）

**方法一：sysfs unbind（不用重编内核，实验室首选）**

```bash
echo 0000:01:00.0 > /sys/bus/pci/drivers/nvme/unbind   # BDF 换成 lspci 里看到的
insmod pcie_nvme.ko
echo 0000:01:00.0 > /sys/bus/pci/drivers/pcie_nvme/bind # 若 id_table 匹配则自动 probe
```

**方法二：内核命令行黑名单**

```
initcall_blacklist=nvme_init     # 加进 /boot/grub 或 U-Boot bootargs
```

**方法三：重编内核去掉 CONFIG_BLK_DEV_NVME**（嵌入式场景最常用，烧录进去就是干净的）

> 注意：方法一里 unbind 的瞬间，内置驱动会做完整 shutdown（删队列、关控制器），之后我们的驱动从"设备已复位"状态重新初始化——这正好锻炼你读内核 nvme 驱动的 remove 流程。

### 15.4 真机调试命令清单（直接复制）

```bash
lspci -nnv                       # 枚举 + 谁在驱动 + 能力列表
lspci -xxx -s 0000:01:00.0       # dump 配置空间 256B（身份证全貌）
cat /proc/iomem                  # BAR0 映射到哪个物理区间
cat /proc/interrupts | grep -i nvme   # MSI-X 向量分到哪个 CPU
dmesg -w                         # 跟着看 probe 打点
setpci -s 0000:01:00.0 CAP_EXP   # 手工读配置空间（免驱动验证硬件存在）
```

### 15.5 面试话术（"为什么先 QEMU 后真机"的满分答案）

"我在 QEMU 上验证了驱动的全部逻辑——协议状态机、DMA、中断、超时恢复，而且 QEMU 设备是开源的，我能对照设备实现反向验证驱动的每一步行为，这是真机做不到的。真机的差异我列过清单：ID 匹配、IOMMU、中断分配、内置驱动冲突，每一点都有明确应对。所以 QEMU 不是'玩具替代'，是**先验证逻辑、再验证环境**的工程顺序。"

---

## 附：口述自测 23 问（全部能答上 = 源码通关）

1. CC 寄存器一次写入配置了哪 6 样东西？
2. DSTRD 是什么？doorbell 地址怎么算？
3. P 位为什么能判断 CQE 新旧？
4. 一条队列有几个 doorbell？各自敲在什么时机？
5. 使能控制器的顺序为什么是 AQA/ASQ/ACQ → CC.EN？
6. ASQ/ACQ 写的是虚拟地址还是总线地址？为什么？
7. Admin 队列为什么不配中断？
8. NN 字段为什么在 offset 516？
9. 创建 I/O 队列为什么先 CQ 后 SQ？
10. IV=qid-1 解决了什么问题？
11. queue_rq 为什么不能睡眠？证据是什么？
12. command_id 为什么能复用 rq->tag？闭环在哪？
13. PRP 单页和多页分别怎么描述？
14. max_hw_sectors 为什么设 4096？和 PRP 什么关系？
15. PRP list 页为什么用 dma_pool？
16. 为什么不把 unmap 放中断上半部？
17. 超时后第一件事为什么是 poll CQ？
18. blk_mq_request_completed 为什么不能用？
19. 错误码为什么用 status & 0x7ff 而不是裸 SC？
20. request_irq 为什么必须在 add_disk 之前？
21. CAP 寄存器偏移 0x0000 这个值，从哪份文档的哪一章能查到？抄值的最快来源是哪个文件？
22. `admin_sq` 和 `admin_sq_dma` 为什么同时存在？写 ASQ 寄存器的是哪个？为什么？
23. 真机上的盘被内核自带 nvme 驱动抢了，怎么让我们的驱动接管？（说出一种方法即可）

（答案都在上文对应讲次里，答不上就翻回去重读那一讲。）
