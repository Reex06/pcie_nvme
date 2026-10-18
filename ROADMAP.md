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
| 5 | 多队列扩展（每 CPU 一队列 + 独立 MSI-X 向量） | `-smp 2` 下 2 个 `nvme-io` 中断行（irq=23/24），dd MATCH，两队列中断均增长 | ✅ |
| 5b | I/O 命令超时处理（丢中断时 poll CQ 恢复） | `irq_loss=1` 模拟丢中断，超时回调 poll CQ 恢复，dd MATCH，rmmod 干净 | ✅ |
| 5c | NVMe Status Code 细分（对标内核 nvme_error_status） | 成功路径 dd 64K MATCH，dmesg 无误报，rmmod 干净 | ✅ |
| 6 | fio 多队列吞吐对比 | randread 4K IOPS 267→398→555，read 128K 17.7→44.4→63.3 MiB/s（numjobs 1/2/4） | ✅ |
| 文档 | README.md 定稿（选题/环境/五阶段/8 个踩坑 + 多队列考点/技术链路） | — | ✅ |

**今日关键环境调整**
- QEMU `-M virt,gic-version=3`：MSI-X 依赖 GICv3 ITS（GICv2 只有 MSI）。
- 内核 `CONFIG_BLK_DEV_NVME=n`：让自写驱动独占 NVMe 设备。

**代码现状**（`driver/pcie_nvme.c`）
- 多 I/O 队列（`nr_queues=min(num_online_cpus(), MQES+1)`），多 hctx，多 MSI-X 向量（`IV=qid-1`）。
- Admin 命令同步轮询完成；I/O 命令 MSI-X 中断异步完成。
- 已有 I/O 超时处理（`tag_set.timeout` + `nvme_timeout` 回调 + `irq_loss` 模拟丢中断）与 NVMe status code 细分（`nvme_error_status` 映射 `blk_status_t`）；已用 fio 验证多队列吞吐（numjobs 1→2→4 递增）。

---

## 二、待执行方案

### 阶段5b：I/O 命令超时处理 ✅（已完成）

实现 `tag_set.timeout` + `nvme_mq_ops.timeout = nvme_timeout`：超时回调先 `nvme_process_cq` poll CQ（覆盖丢中断），request 已结束则返回 `BLK_EH_DONE`，否则 `BLK_EH_RESET_TIMER` 重置计时器继续等。模块参数 `irq_loss=1` 模拟丢中断验证 dd 不 hang。详见 README 阶段5b + 坑 9。

### 阶段5c：NVMe Status Code 细分处理 ✅（已完成）

CQE status 字段 `status>>1` 得 `SCT<<8|SC`（bit10:8=SCT 错误类型，bit7:0=SC 具体错误）。`nvme_error_status()` 对标内核 `core.c:nvme_error_status`：`switch (status & 0x7ff)` 屏蔽 M/DNR/CRD 后按 SC 细分映射 `blk_status_t`——成功/容量不足(NOSPC)/可重试(TARGET)/不支持(NOTSUPP)/介质错误(MEDIUM)/保护错误(PROTECTION)/保留冲突(RESV_CONFLICT)/传输错误(TRANSPORT)/ZNS 资源(ZONE_*)/其余 IOERR。注意规范里 `0x80~0xbf` 是 Media Error（SCT=2），`0x70~0x7f` 是 Command Specific（SCT=1）。详见 README 阶段5c。

### 阶段6：性能与亲和优化

- ✅ **fio 多队列吞吐对比**：`randread 4k` IOPS 267→398→555，`read 128k` 带宽 17.7→44.4→63.3 MiB/s（numjobs 1/2/4），证明多队列价值。
- ⬜ **中断亲和**：`irq_set_affinity_hint(q->irq, cpumask_of(hctx->type->cpu))` 把队列中断绑到对应 CPU。
- ⬜ **中断合并**：Set Features（Interrupt Coalescing）命令减少中断频率，提升吞吐。
- ⬜ **SGL 支持**：NVMe 1.2 后的 Scatter-Gather List 替代 PRP（可选，PRP 已够用）。

### 收尾：版本管理

- ✅ `git init` + 首次提交 + 阶段5 提交（**不加任何 AI/Claude 签名行**，遵守全局规则）。
- ✅ `.gitignore` 排除 `*.ko *.o *.mod *.mod.c Module.symvers modules.order` 等编译产物。
- ✅ 已推送 `github.com/Reex06/pcie_nvme.git`（main 分支，SSH 认证）。

---

## 三、优先级建议

1. 剩余可选：中断亲和 / 中断合并 / SGL（均非必需，项目主体已完整可展示）。
