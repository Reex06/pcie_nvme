# 项目答辩话术（面试用）

> 本文档是面试准备材料，核心是「高频追问 + 标准答案」，可直接背。
> 技术细节、踩坑完整版见 [README.md](./README.md)，项目管理见 [ROADMAP.md](./ROADMAP.md)。

---

## 一、项目定位（先立人设，避免被问倒）

**一句话**：从零实现了一个精简版 NVMe 块设备驱动，打通 PCIe → NVMe → blk-mq 全链路，目的是深入理解驱动栈，不是替代内核驱动。

**简历条目**：

> **基于 QEMU 的 ARM64 NVMe 驱动开发**（个人项目）
> 从零实现精简 NVMe 块设备驱动（约 1000 行 C），打通 PCIe→NVMe→blk-mq 全链路。涵盖 PCIe 枚举/BAR/MMIO、Admin/I/O 命令队列、PRP 多页 DMA、MSI-X 异步完成、多队列（每 CPU 一队列 + 独立中断向量）、I/O 超时恢复、NVMe 错误码细分。fio 实测 numjobs 1→2 IOPS 翻倍。全程对标内核 `drivers/nvme/host/pci.c`，记录 9 个「现象→根因→解决」案例。

**禁止的表述**：重新实现了 NVMe 驱动 / 替代内核驱动 / 高性能驱动 / 量产级。

---

## 二、技术链路怎么讲（2 分钟版本）

按数据流向讲，别背术语：

```
1. PCIe 枚举：pci_register_driver 匹配 vendor/device → probe → 读 BAR0 → devm_ioremap 映射 MMIO
2. 初始化：写 AQA/ASQ/ACQ → CC.EN → 等 CSTS.RDY=1 → 提交 Identify 命令读控制器/namespace 信息
3. I/O 队列：Admin 命令创建 I/O SQ/CQ → blk_mq_alloc_disk 注册 gendisk
4. 数据通路：queue_rq 构造 Write/Read 命令 + PRP 页 → dma_map_sg 得总线地址 → 敲 SQ tail doorbell
5. 完成：控制器写 CQE 触发 MSI-X → 中断上半部收割 CQ → blk_mq_tag_to_rq 反查 request → blk_mq_complete_request → .complete 回调 unmap + blk_mq_end_request
```

讲完这句收尾：**"每个环节都能对标内核 nvme pci.c 的实现。"**

---

## 三、高频追问 + 标准答案

### Q1：内核已经有 NVMe 驱动了，为什么重复做？

> 目标不是替代内核，是用最短路径证明我理解这套链路。内核 nvme 几万行、支持 multipath/fabrics/failover，我写的是 1000 行的核心子集。写一遍比读一万行源码理解深，而且写完回头读内核 pci.c，能讲清它为什么那么设计。

（类比：手写 mini 内核 / mini TCP/IP 栈，目的是理解底层，没人觉得是造轮子。）

### Q2：QEMU 虚拟的和真实 NVMe 有什么区别？

| 维度 | QEMU | 真实 NVMe |
|---|---|---|
| 时序/性能 | 软件模拟，几百 IOPS | 硬件 DMA 引擎，几十万 IOPS |
| 错误场景 | 几乎不出错，超时靠 `irq_loss` 注入 | media error/超时/固件问题自然发生 |
| DMA/一致性 | host 内存拷贝，无真实 IOMMU | 真 IOMMU/SMMU + cache coherency |
| 中断 | GICv3 ITS 虚拟 MSI-X | 物理 MSI-X table + 硬件 GIC |
| 协议完整性 | NVMe 子集（SGL/CMB/zoned 简化） | 厂商固件，特性各异 |

答完主动补一句：**"所以我专门做了 5b 超时、5c 错误码，就是补真实设备才会遇到的场景，QEMU 上只能人为注入来验证。"**——把劣势翻成优势。

### Q3：怎么保证虚拟能用、真实硬件就能用？

> 不能 100% 保证，但风险可控。我代码里没有一行 QEMU 专属 hack，全走标准接口——NVMe 寄存器/SQ/CQ/PRP 是协议规范（不随厂商变），pci_register_driver/dma_map_sg/blk_mq/request_irq 是内核标准 API，真实设备也走这套。
>
> 真实硬件可能的差异：timeout 值、初始化时序需按设备调；真实 arm64 有 SMMU/IOMMU，dma_map 路径更复杂；真实会出错（所以有错误码细分）。

### Q4：做这个项目的意义？

1. **求职**：PCIe + 块设备 + 中断 + DMA + blk-mq，正是驱动岗最核心的技能栈。
2. **面试**：驱动岗最爱问"你写过最难的驱动"，这个能讲 30 分钟，每个坑都是加分项。
3. **方法论**：展示了分阶段验证 + 踩坑记录 + 对标内核的工程习惯。

---

## 四、技术深挖（面试官顺着链路追问）

### PRP 和 SGL 的区别？
PRP 是页对齐物理页指针（PRP1 单页、PRP2 指向 PRP list 页）；SGL 是更灵活的 scatter-gather 描述符。我实现的是 PRP，因为它足够覆盖页对齐的 bio 场景，且对标早期内核实现。

### 为什么用 blk-mq？
blk-mq 多队列、per-CPU 无锁、支持多硬件队列，是现在块设备的标准框架，能天然映射 NVMe 的多个 I/O 队列。

### MSI-X 和 MSI 的区别？
MSI 只有一个（或有限几个）向量；MSI-X 支持多个独立向量，每个可独立屏蔽、独立绑定 CPU。NVMe 需要每队列一个中断向量，所以必须 MSI-X（还要 GICv3 ITS）。

### doorbell 机制？
写 SQ tail doorbell 通知控制器有新命令；控制器执行完写 CQE 并递增 head，驱动收割后写 CQ head doorbell 通知控制器释放 CQ 条目。是 NVMe 不产生寄存器的「生产-消费」模型。

### DMA 一致性怎么保证？
`dma_alloc_coherent`（SQ/CQ/PRP list 用，保证一致）；`dma_map_sg`（数据 buffer 用，流式映射，unmap 时同步）。

### 为什么 unmap 放 .complete 回调而不是中断上半部？
缩短硬中断占用时间；上半部只记 status + complete_request，重活交给软中断，对标内核 nvme pci.c。

### command_id 怎么来的？
直接复用 `rq->tag`（blk-mq 队列内 tag 唯一），完成时 `blk_mq_tag_to_rq` 反查，无需额外映射表。多队列下 tag 仍 per-hctx 唯一，靠「哪个 CQ 收到 CQE」定位队列。

### 超时怎么处理？
`tag_set.timeout` 到点触发 `.timeout` 回调 → 先 poll CQ 区分「丢中断（CQE 已就绪）」和「真卡死」→ 已就绪返回 BLK_EH_DONE，否则 BLK_EH_RESET_TIMER 继续等。

---

## 五、踩坑精选（挑 2-3 个最能体现能力的讲）

面试官问"你踩过什么坑"，优先讲这几个（有「现象→根因→解决」完整链路）：

1. **Bus Master 没开**（坑2）：`pci_enable_device_mem` 只开 MEM 位，没开 Bus Master，设备 DMA 读不到 SQ → 命令永不完成。体现对 PCIe 配置空间 Command 寄存器的理解。
2. **同步完成假阴性**（坑9）：超时回调用 `blk_mq_request_completed()` 判断恒返回 false，因为多队列下 complete 是同步路径，state 被重置为 IDLE 而非 COMPLETE。体现对 blk-mq 内部状态的深挖。
3. **GICv2 不支持 MSI-X**（坑8）：QEMU 默认 GICv2 只有 MSI，需 GICv3 ITS。体现对 ARM 中断控制器的理解。
4. **arm64 ioremap 未导出**（坑1）：out-of-tree 模块用不了 ioremap，改 devm_ioremap。体现对内核符号导出的理解。

---

## 六、容易被问倒的雷区（提前准备好）

- **别说「我重新实现了 NVMe」**——要说「精简版/核心子集」。
- **别吹性能**——QEMU 上 IOPS 只有几百，真实是几十万，吹了必被拆穿。
- **别回避「虚拟和真实的区别」**——这是展示功底的机会，不是刁难。
- **PRP 的页对齐约束**要能讲清：PRP 条目必须页对齐、长度页整数倍，所以 bio 的 sg 按页拆分。
- **多队列的队列深度/向量数**：`nr_queues = min(num_online_cpus(), MQES+1)`，动态读 CAP，不是写死。
