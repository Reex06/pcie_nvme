# 开发进度与后续计划

> 本文档记录「已执行方案」与「待执行方案」，用于项目管理与后续续接。
> 面向面试官的成品展示见 [README.md](./README.md)。

---

## 一、今日已完成（2026-10-17）

| 阶段 | 内容 | 验证结果 | 状态 |
|---|---|---|---|
| 0 环境 | arm64 内核 + busybox rootfs + QEMU virt | 内核启动、内核自带 nvme 驱动曾识别 `/dev/nvme0n1` | ✅ |
| 1 | PCIe 枚举 + BAR0 映射 + 读 CAP/VS/CSTS | BDF=`0000:00:02.0`, BAR0=`0x8000004000`/16KB, MQES=2048 | ✅ |
| 2 | 控制器初始化 + Admin Queue + Identify | `CC=0x00460001`, `CSTS.RDY=1`, VID/MN/SN 正确 | ✅ |
| 3 | I/O 队列 + blk-mq + Write/Read + PRP | `/dev/nvme0n1` 注册，dd 4K/64K 写读 MATCH | ✅ |
| 4 | MSI-X 中断 + 异步完成 | 中断计数 2→10，dd MATCH，`msix_enabled=1` | ✅ |
| 文档 | README.md 定稿（选题/环境/四阶段/8 个踩坑/技术链路） | — | ✅ |

**今日关键环境调整**
- QEMU `-M virt,gic-version=3`：MSI-X 依赖 GICv3 ITS（GICv2 只有 MSI）。
- 内核 `CONFIG_BLK_DEV_NVME=n`：让自写驱动独占 NVMe 设备。

**代码现状**（`driver/pcie_nvme.c`）
- 单 I/O 队列（`nr_queues=1`），一个 hctx，一个 MSI-X 向量。
- Admin 命令同步轮询完成；I/O 命令 MSI-X 中断异步完成。
- 无 I/O 超时处理、无 NVMe status code 细分处理。

---

## 二、待执行方案

### 阶段5：多队列扩展（每核一队列，对标真实 NVMe 性能模型）

**目标**：`nr_queues` 从 1 扩到 `min(num_online_cpus(), MQES+1)`，每个队列独立 SQ/CQ + 独立 MSI-X 向量 + 独立 hctx，让 blk-mq 按 CPU 分发请求。

**改动点**：
1. `nvme_setup_disk`：`nr_queues = min_t(int, num_online_cpus(), NVME_CAP_MQES(cap)+1)`；`tag_set.nr_hw_queues = nr_queues`。
2. `pci_alloc_irq_vectors(pdev, 1, nr_queues, PCI_IRQ_MSIX | PCI_IRQ_MSI)`（max_vecs 从 1 改 nr_queues）。
3. probe 循环 `for (i=0; i<nr_queues; i++) nvme_alloc_io_queue(dev, &queues[i], i+1)`。
   - qid = i+1，IV = qid-1 = i，`tags = tag_set.tags[i]`（`nvme_alloc_io_queue` 已按 `qid-1` 索引，无需改）。
4. remove/错误路径循环 `free_irq` + `nvme_delete_io_queue` + `pci_free_irq_vectors`。
5. `command_id = rq->tag` 在多 hctx 下仍 per-hctx 唯一，`blk_mq_tag_to_rq(q->tags, command_id)` 逻辑不变。

**验证**：`-smp 4` 下看到多个 `nvme-io` 中断行；`fio --numjobs=4` 吞吐高于单队列。

### 阶段5b：I/O 命令超时处理

**目标**：设备不响应（不产生中断）时，request 不能永久挂起。

**改动点**：
1. `nvme_mq_ops` 加 `.timeout = nvme_timeout`。
2. `queue_rq` 里 `blk_mq_start_request` 后 `blk_mq_rq_timeout(rq, NVME_IO_TIMEOUT)`（如 30s）。
3. `nvme_timeout` 回调：打印卡住的 request（tag/opcode），记录超时计数；简单版可直接 `blk_mq_complete_request(rq)` 返回错误，进阶版发 NVMe Abort 命令或 `nvme_disable_ctrl` 复位控制器。
4. 涉及「在途命令追踪」：需要 per-command 状态记录（pdu 已可存），便于 timeout 时定位。

**验证**：临时在中断处理里 `return IRQ_NONE` 模拟丢中断，观察 timeout 触发、request 被回收、不 hang。

### 阶段5c：NVMe Status Code 细分处理

**目标**：CQE status 的 SC 字段（bit 15:1）细分，而非一律 `BLK_STS_IOERR`。

**改动点**：
1. 在 `nvme_complete_rq`/`nvme_mq_complete` 里按 SC 映射：
   - `0x0` 成功；`0x70~0x7f` 媒体错误；`0x80~0xbf` 命令特定错误；重试类 SC 判 `BLK_STS_RESOURCE`。
2. 可选：对可重试错误实现 retry（需 per-request 重试计数，放 pdu）。

### 阶段6（可选）：性能与亲和优化

- **中断亲和**：`irq_set_affinity_hint(q->irq, cpumask_of(hctx->type->cpu))` 把队列中断绑到对应 CPU。
- **中断合并**：Set Features（Interrupt Coalescing）命令减少中断频率，提升吞吐。
- **SGL 支持**：NVMe 1.2 后的 Scatter-Gather List 替代 PRP（可选，PRP 已够用）。

### 收尾：版本管理

- `git init` + 首次提交（**不加任何 AI/Claude 签名行**，遵守全局规则）。
- `.gitignore` 排除 `*.ko *.o *.mod *.mod.c Module.symvers modules.order` 等编译产物。

---

## 三、优先级建议

1. **阶段5 多队列**（含金量最高，补齐性能模型，面试最能讲）。
2. **阶段5b 超时**（补健壮性，面试常问「命令不完成怎么办」）。
3. 收尾 git 提交（随时可做）。
4. 阶段5c / 阶段6 按时间取舍。
