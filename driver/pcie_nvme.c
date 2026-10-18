// SPDX-License-Identifier: GPL-2.0
/*
 * pcie_nvme.c - 阶段5c：NVMe Status Code 细分处理（基于阶段5b 多队列+超时）
 *
 * 阶段1：PCIe 枚举 + BAR0 映射 + MMIO 寄存器读取。
 * 阶段2：控制器初始化 + Admin Queue + Identify Controller 命令往返。
 * 阶段3：I/O 队列 + Write/Read + PRP，同步轮询打通数据通路。
 * 阶段4：MSI-X 中断 + 异步完成（blk_mq_complete_request + .complete 回调）。
 * 阶段5：多队列扩展（每 CPU 一个 I/O 队列 + 独立 MSI-X 向量 IV=qid-1）。
 * 阶段5b：I/O 命令超时（tag_set.timeout + .timeout 回调 poll CQ 恢复）。
 * 本阶段新增（Status Code 细分）：
 *   1. CQE status 的 SC 字段按 NVMe 规范细分映射为 blk_status_t
 *   2. 区分成功/介质错误/保护错误/容量不足/不支持/可重试等，非一律 IOERR
 *
 * 面试考点：
 *   - MSI-X 与 NVMe 的对应关系：每个 I/O CQ 一个中断向量（IV 字段绑定）
 *   - command_id 复用 blk-mq 的 tag，完成时 blk_mq_tag_to_rq 反向映射
 *   - 中断上半部收割 CQ + 敲 CQ head doorbell，下半部由 block 层 softirq 完成
 *   - PRP 是 NVMe 传数据的核心：单页走 PRP1，多页 PRP1+PRP2 指向 PRP list 页
 *   - 超时≠必然丢数据：先 poll CQ 区分中断丢失（可恢复）与设备真卡死
 *   - CQE status 的 SCT 区分错误类型，SC 细分到具体错误，映射 blk_status_t
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/io.h>
#include <linux/dma-mapping.h>
#include <linux/jiffies.h>
#include <linux/string.h>
#include <linux/byteorder/generic.h>
#include <linux/blkdev.h>
#include <linux/blk-mq.h>
#include <linux/scatterlist.h>
#include <linux/dmapool.h>
#include <linux/interrupt.h>
#include <linux/cpumask.h>

#define NVME_VENDOR_ID  0x1b36
#define NVME_DEVICE_ID  0x0010

/* NVMe 控制器寄存器在 BAR0 内的偏移 */
#define NVME_REG_CAP   0x0000
#define NVME_REG_VS    0x0008
#define NVME_REG_CC    0x0014
#define NVME_REG_CSTS  0x001c
#define NVME_REG_AQA   0x0024
#define NVME_REG_ASQ   0x0028
#define NVME_REG_ACQ   0x0030
#define NVME_REG_DBS   0x1000  /* Doorbell 寄存器基地址 */

/* CAP 字段 */
#define NVME_CAP_MQES(cap)  ((int)((cap) & 0xffff))
#define NVME_CAP_DSTRD(cap) (1u << (2 + (((cap) >> 32) & 0xf)))
#define NVME_CAP_TO(cap)    ((((cap) >> 24) & 0xff))

/* CC 字段 */
#define NVME_CC_ENABLE     BIT(0)
#define NVME_CC_CSS_NVM    (0 << 4)
#define NVME_CC_MPS_4K     (0 << 7)
#define NVME_CC_AMS_RR     (0 << 11)
#define NVME_CC_SHN_NONE   (0 << 14)
#define NVME_CC_IOSQES     (6 << 16)  /* 2^6 = 64B */
#define NVME_CC_IOCQES     (4 << 20)  /* 2^4 = 16B */

/* CSTS 字段 */
#define NVME_CSTS_RDY      BIT(0)
#define NVME_CSTS_CFS      BIT(1)

/* AQA 字段 */
#define NVME_AQA_ASQS_SHIFT 0
#define NVME_AQA_ACQS_SHIFT 16

/* Admin 命令 opcode */
#define NVME_ADMIN_DELETE_SQ 0x00
#define NVME_ADMIN_CREATE_SQ 0x01
#define NVME_ADMIN_DELETE_CQ 0x04
#define NVME_ADMIN_CREATE_CQ 0x05
#define NVME_ADMIN_IDENTIFY   0x06

/* I/O 命令 opcode */
#define NVME_CMD_FLUSH 0x00
#define NVME_CMD_WRITE 0x01
#define NVME_CMD_READ  0x02

#define NVME_ADMIN_Q_DEPTH  32   /* Admin SQ/CQ 深度 */
#define NVME_IO_Q_DEPTH     64   /* I/O SQ/CQ 深度 */
#define NVME_MAX_SEGS       128  /* 单请求最多 scatter-gather 段数 */

#define NVME_IO_TIMEOUT_DEFAULT 30 /* I/O 命令默认超时（秒） */

/* NVMe Status Code（status >> 1 后的完整编码 SCT<<8|SC，不含 M/DNR/CRD） */
#define NVME_SC_SUCCESS               0x0
#define NVME_SC_INVALID_OPCODE        0x1
#define NVME_SC_INVALID_FIELD         0x2
#define NVME_SC_INVALID_NS            0xb
#define NVME_SC_CMD_INTERRUPTED       0x21
#define NVME_SC_LBA_RANGE             0x80
#define NVME_SC_CAP_EXCEEDED          0x81
#define NVME_SC_NS_NOT_READY          0x82
#define NVME_SC_RESERVATION_CONFLICT  0x83
#define NVME_SC_BAD_ATTRIBUTES        0x180
#define NVME_SC_INVALID_PI            0x181
#define NVME_SC_READ_ONLY             0x182
#define NVME_SC_ONCS_NOT_SUPPORTED    0x183
#define NVME_SC_WRITE_FAULT           0x280
#define NVME_SC_READ_ERROR            0x281
#define NVME_SC_GUARD_CHECK           0x282
#define NVME_SC_APPTAG_CHECK          0x283
#define NVME_SC_REFTAG_CHECK          0x284
#define NVME_SC_COMPARE_FAILED        0x285
#define NVME_SC_ACCESS_DENIED         0x286
#define NVME_SC_UNWRITTEN_BLOCK       0x287
#define NVME_SC_HOST_PATH_ERROR       0x370
#define NVME_SC_ZONE_TOO_MANY_ACTIVE  0x1bd
#define NVME_SC_ZONE_TOO_MANY_OPEN    0x1be

/* CQE status 字段标志（相对 status >> 1 后的值） */
#define NVME_SC_MORE 0x2000  /* M：更多状态 */
#define NVME_SC_DNR  0x4000  /* DNR：不要重试 */
#define NVME_SC_CRD  0x1800  /* CRD：命令重试延迟 */

/* NVMe 提交队列项 SQE，固定 64 字节 */
struct nvme_sqe {
	u8  opcode;
	u8  flags;
	u16 command_id;
	u32 nsid;
	u64 rsvd2;
	u64 metadata;
	u64 prp1;      /* PRP 条目1：数据缓冲区物理地址 */
	u64 prp2;      /* PRP 条目2 */
	u32 cdw10;
	u32 cdw11;
	u32 cdw12;
	u32 cdw13;
	u32 cdw14;
	u32 cdw15;
};

/* NVMe 完成队列项 CQE，固定 16 字节 */
struct nvme_cqe {
	u32 result;
	u32 rsvd1;
	u16 sq_head;
	u16 sq_id;
	u16 command_id;
	u16 status;     /* P[0] SC[15:1] */
};

/* Identify Controller 数据结构开头关键字段（NVMe 1.4，共 4096 字节） */
struct nvme_id_ctrl_hdr {
	__le16 vid;
	__le16 ssvid;
	char sn[20];
	char mn[40];
	char fr[8];
};

/* per-request 私有数据（blk-mq 通过 cmd_size 分配，blk_mq_rq_to_pdu 访问） */
struct nvme_cmd {
	struct scatterlist sg[NVME_MAX_SEGS];
	int nsegs;
	void *prp_list;         /* PRP list 页虚拟地址（dma_pool 分配） */
	dma_addr_t prp_list_dma;
	u16 status;             /* CQE status 字段（中断记录，complete 回调用） */
};

struct nvme_queue {
	struct pcie_nvme_dev *dev;
	u16 qid;                /* 队列标识（0=admin，1..N=I/O） */
	u16 q_depth;
	struct nvme_sqe *sq;
	dma_addr_t sq_dma;
	struct nvme_cqe *cq;
	dma_addr_t cq_dma;
	u16 sq_tail;
	u16 cq_head;
	u8  cq_phase;
	int irq;                /* MSI-X 中断号（阶段4 异步完成） */
	struct blk_mq_tags *tags; /* command_id -> request 映射 */
};

struct pcie_nvme_dev {
	struct pci_dev *pdev;
	void __iomem *bar0;
	resource_size_t bar0_start;
	resource_size_t bar0_len;

	u32 dstrd;

	/* Admin Queue */
	struct nvme_sqe *admin_sq;
	dma_addr_t admin_sq_dma;
	struct nvme_cqe *admin_cq;
	dma_addr_t admin_cq_dma;
	u16 sq_tail;
	u16 cq_head;
	u8  cq_phase;

	/* Identify 缓冲区 */
	void *identify;
	dma_addr_t identify_dma;

	/* Namespace 信息 */
	u32 nsid;
	u64 nsze;         /* 命名空间 LBA 总数 */
	u8  lba_shift;    /* LBA 大小 = 2^lba_shift 字节 */

	/* I/O 队列 + blk-mq */
	struct nvme_queue *queues;
	int nr_queues;
	struct blk_mq_tag_set tag_set;
	struct gendisk *disk;

	/* PRP list 页池 */
	struct dma_pool *prp_pool;
};

static const struct pci_device_id pcie_nvme_id_table[] = {
	{ PCI_DEVICE(NVME_VENDOR_ID, NVME_DEVICE_ID) },
	{ 0, }
};
MODULE_DEVICE_TABLE(pci, pcie_nvme_id_table);

/* 可调参数：I/O 超时时间（秒）与中断丢失故障注入（验证用） */
static unsigned int io_timeout = NVME_IO_TIMEOUT_DEFAULT;
module_param(io_timeout, uint, 0444);
MODULE_PARM_DESC(io_timeout, "I/O 命令超时时间（秒），默认 30");

static bool irq_loss;
module_param(irq_loss, bool, 0444);
MODULE_PARM_DESC(irq_loss, "模拟中断丢失（不收割 CQ），验证超时恢复");

/* 前向声明：中断处理函数（nvme_alloc_io_queue 里 request_irq 引用） */
static irqreturn_t nvme_irq(int irq, void *data);

static void __iomem *nvme_sq_doorbell(struct pcie_nvme_dev *dev, u16 qid)
{
	return dev->bar0 + NVME_REG_DBS + (2 * qid) * dev->dstrd;
}

static void __iomem *nvme_cq_doorbell(struct pcie_nvme_dev *dev, u16 qid)
{
	return dev->bar0 + NVME_REG_DBS + (2 * qid + 1) * dev->dstrd;
}

static int nvme_enable_ctrl(struct pcie_nvme_dev *dev)
{
	u64 cap = readq(dev->bar0 + NVME_REG_CAP);
	u32 csts, cc;
	unsigned long timeout;

	csts = readl(dev->bar0 + NVME_REG_CSTS);
	if (csts & NVME_CSTS_RDY) {
		dev_err(&dev->pdev->dev, "控制器已处于 RDY 状态 (CSTS=0x%x)\n", csts);
		return -EBUSY;
	}

	writel(((NVME_ADMIN_Q_DEPTH - 1) << NVME_AQA_ASQS_SHIFT) |
	       ((NVME_ADMIN_Q_DEPTH - 1) << NVME_AQA_ACQS_SHIFT),
	       dev->bar0 + NVME_REG_AQA);
	writeq(dev->admin_sq_dma, dev->bar0 + NVME_REG_ASQ);
	writeq(dev->admin_cq_dma, dev->bar0 + NVME_REG_ACQ);

	cc = NVME_CC_ENABLE | NVME_CC_CSS_NVM | NVME_CC_MPS_4K |
	     NVME_CC_AMS_RR | NVME_CC_SHN_NONE |
	     NVME_CC_IOSQES | NVME_CC_IOCQES;
	writel(cc, dev->bar0 + NVME_REG_CC);

	timeout = jiffies + msecs_to_jiffies((NVME_CAP_TO(cap) + 1) * 500);
	while (!((csts = readl(dev->bar0 + NVME_REG_CSTS)) & NVME_CSTS_RDY)) {
		if (time_after(jiffies, timeout)) {
			dev_err(&dev->pdev->dev, "等待 CSTS.RDY 超时 (CSTS=0x%x)\n", csts);
			return -ETIMEDOUT;
		}
		cpu_relax();
	}
	if (csts & NVME_CSTS_CFS) {
		dev_err(&dev->pdev->dev, "控制器致命错误 CFS=1 (CSTS=0x%x)\n", csts);
		return -EIO;
	}

	dev_info(&dev->pdev->dev, "控制器使能成功: CC=0x%08x CSTS=0x%08x (RDY=1)\n",
		 cc, csts);
	return 0;
}

static void nvme_disable_ctrl(struct pcie_nvme_dev *dev)
{
	u32 csts;

	writel(0, dev->bar0 + NVME_REG_CC);
	while ((csts = readl(dev->bar0 + NVME_REG_CSTS)) & NVME_CSTS_RDY)
		cpu_relax();
	dev_info(&dev->pdev->dev, "控制器已关闭 (CSTS=0x%x, RDY=0)\n", csts);
}

/* 轮询 admin CQ，等待一条命令完成 */
static int nvme_admin_wait_cq(struct pcie_nvme_dev *dev, u32 *result)
{
	struct nvme_cqe *cqe = &dev->admin_cq[dev->cq_head];
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);
	u16 status;

	while ((le16_to_cpu(cqe->status) & 0x1) != dev->cq_phase) {
		if (time_after(jiffies, timeout)) {
			dev_err(&dev->pdev->dev, "等待 admin CQ 完成超时\n");
			return -ETIMEDOUT;
		}
		cpu_relax();
	}

	status = le16_to_cpu(cqe->status) >> 1;
	if (status) {
		dev_err(&dev->pdev->dev, "admin 命令失败: status=0x%x\n", status);
		return -EIO;
	}
	if (result)
		*result = le32_to_cpu(cqe->result);

	dev->cq_head = (dev->cq_head + 1) % NVME_ADMIN_Q_DEPTH;
	if (dev->cq_head == 0)
		dev->cq_phase = !dev->cq_phase;
	writel(dev->cq_head, nvme_cq_doorbell(dev, 0));

	return 0;
}

/* 通用 admin 命令提交 */
static int nvme_admin_cmd(struct pcie_nvme_dev *dev, const struct nvme_sqe *sqe,
			  u32 *result)
{
	memcpy(&dev->admin_sq[dev->sq_tail], sqe, sizeof(*sqe));
	dev->sq_tail = (dev->sq_tail + 1) % NVME_ADMIN_Q_DEPTH;
	writel(dev->sq_tail, nvme_sq_doorbell(dev, 0));

	return nvme_admin_wait_cq(dev, result);
}

static void nvme_print_field(struct pcie_nvme_dev *dev, const char *name,
			     const char *buf, int len)
{
	char tmp[64];
	int end = len - 1;

	while (end >= 0 && (buf[end] == ' ' || buf[end] == '\0'))
		end--;
	memcpy(tmp, buf, end + 1);
	tmp[end + 1] = '\0';
	dev_info(&dev->pdev->dev, "  %-4s = %s\n", name, tmp);
}

/* Identify Controller (CNS=1)，验证阶段2链路 */
static int nvme_identify_ctrl(struct pcie_nvme_dev *dev)
{
	struct nvme_sqe sqe = { 0 };

	sqe.opcode = NVME_ADMIN_IDENTIFY;
	sqe.command_id = 1;
	sqe.nsid = 0;
	sqe.prp1 = cpu_to_le64(dev->identify_dma);
	sqe.cdw10 = cpu_to_le32(0x1);

	return nvme_admin_cmd(dev, &sqe, NULL);
}

static void nvme_dump_identify_ctrl(struct pcie_nvme_dev *dev)
{
	struct nvme_id_ctrl_hdr *id = dev->identify;
	/* NN(命名空间数量) 在 offset 516，见内核 include/linux/nvme.h */
	u32 nn = le32_to_cpu(*(__le32 *)(dev->identify + 516));

	dev_info(&dev->pdev->dev, "Identify Controller: VID=0x%04x",
		 le16_to_cpu(id->vid));
	nvme_print_field(dev, "MN", id->mn, sizeof(id->mn));
	nvme_print_field(dev, "SN", id->sn, sizeof(id->sn));
	nvme_print_field(dev, "FR", id->fr, sizeof(id->fr));
	dev_info(&dev->pdev->dev, "  NN   = %u\n", nn);
}

/* Identify Namespace (CNS=0)，读 NSZE 与 LBA 格式 */
static int nvme_identify_ns(struct pcie_nvme_dev *dev)
{
	struct nvme_sqe sqe = { 0 };

	sqe.opcode = NVME_ADMIN_IDENTIFY;
	sqe.command_id = 2;
	sqe.nsid = cpu_to_le32(dev->nsid);
	sqe.prp1 = cpu_to_le64(dev->identify_dma);
	sqe.cdw10 = cpu_to_le32(0x0);

	return nvme_admin_cmd(dev, &sqe, NULL);
}

static void nvme_parse_ns(struct pcie_nvme_dev *dev)
{
	u64 nsze = le64_to_cpu(*(__le64 *)(dev->identify + 0));
	u8 flbas = *(u8 *)(dev->identify + 26);
	u8 lbaf_idx = flbas & 0xf;
	u32 lbaf = le32_to_cpu(*(__le32 *)(dev->identify + 128 + lbaf_idx * 4));
	u8 lbads = (lbaf >> 16) & 0xff;

	dev->nsze = nsze;
	dev->lba_shift = lbads;
	dev_info(&dev->pdev->dev, "Namespace: NSZE=%llu LBADS=%u (LBA=%uB)\n",
		 nsze, lbads, 1u << lbads);
}

/* 创建 I/O Completion Queue（Admin opcode 0x05） */
static int nvme_create_io_cq(struct pcie_nvme_dev *dev, struct nvme_queue *q)
{
	struct nvme_sqe sqe = { 0 };

	sqe.opcode = NVME_ADMIN_CREATE_CQ;
	sqe.command_id = 3;
	sqe.prp1 = cpu_to_le64(q->cq_dma);
	sqe.cdw10 = cpu_to_le32((q->qid & 0xffff) |
				((q->q_depth - 1) << 16));
	/* PC=1(物理连续), IEN=1(使能中断), IV=qid-1(每个 CQ 绑定自己的 MSI-X 向量) */
	sqe.cdw11 = cpu_to_le32(0x3 | ((q->qid - 1) << 16));

	return nvme_admin_cmd(dev, &sqe, NULL);
}

/* 创建 I/O Submission Queue（Admin opcode 0x01） */
static int nvme_create_io_sq(struct pcie_nvme_dev *dev, struct nvme_queue *q)
{
	struct nvme_sqe sqe = { 0 };

	sqe.opcode = NVME_ADMIN_CREATE_SQ;
	sqe.command_id = 4;
	sqe.prp1 = cpu_to_le64(q->sq_dma);
	sqe.cdw10 = cpu_to_le32((q->qid & 0xffff) |
				((q->q_depth - 1) << 16));
	/* PC=1, CQID=qid */
	sqe.cdw11 = cpu_to_le32(0x1 | (q->qid << 16));

	return nvme_admin_cmd(dev, &sqe, NULL);
}

/* 删除 I/O 队列（Delete SQ/CQ） */
static void nvme_delete_io_queue(struct pcie_nvme_dev *dev, struct nvme_queue *q)
{
	struct nvme_sqe sqe = { 0 };

	sqe.opcode = NVME_ADMIN_DELETE_SQ;
	sqe.command_id = 5;
	sqe.cdw10 = cpu_to_le32(q->qid);
	nvme_admin_cmd(dev, &sqe, NULL);

	sqe.opcode = NVME_ADMIN_DELETE_CQ;
	sqe.command_id = 6;
	sqe.cdw10 = cpu_to_le32(q->qid);
	nvme_admin_cmd(dev, &sqe, NULL);
}

/* 分配并初始化一个 I/O 队列对 */
static int nvme_alloc_io_queue(struct pcie_nvme_dev *dev, struct nvme_queue *q,
			       u16 qid)
{
	q->dev = dev;
	q->qid = qid;
	q->q_depth = NVME_IO_Q_DEPTH;
	q->sq_tail = 0;
	q->cq_head = 0;
	q->cq_phase = 1;
	/* I/O 队列 qid=1..N 对应 MSI-X 向量 index=qid-1，tags 同 index */
	q->irq = pci_irq_vector(dev->pdev, qid - 1);
	q->tags = dev->tag_set.tags[qid - 1];

	q->sq = dmam_alloc_coherent(&dev->pdev->dev,
			q->q_depth * sizeof(struct nvme_sqe), &q->sq_dma, GFP_KERNEL);
	if (!q->sq)
		return -ENOMEM;
	q->cq = dmam_alloc_coherent(&dev->pdev->dev,
			q->q_depth * sizeof(struct nvme_cqe), &q->cq_dma, GFP_KERNEL);
	if (!q->cq)
		return -ENOMEM;

	if ((q->sq_dma & 0xfff) || (q->cq_dma & 0xfff)) {
		dev_err(&dev->pdev->dev, "I/O 队列 qid=%u 未 4KB 对齐\n", qid);
		return -EIO;
	}

	if (nvme_create_io_cq(dev, q))
		return -EIO;
	if (nvme_create_io_sq(dev, q))
		return -EIO;

	/* 绑定 MSI-X 中断（必须在 add_disk 前，避免有 IO 却无中断处理） */
	if (request_irq(q->irq, nvme_irq, 0, "nvme-io", q)) {
		dev_err(&dev->pdev->dev, "request_irq qid=%u irq=%d failed\n",
			qid, q->irq);
		nvme_delete_io_queue(dev, q);
		return -EIO;
	}

	dev_info(&dev->pdev->dev, "I/O 队列 qid=%u 创建成功 irq=%d (sq_dma=0x%llx cq_dma=0x%llx)\n",
		 qid, q->irq, (u64)q->sq_dma, (u64)q->cq_dma);
	return 0;
}

/* ---- blk-mq 数据通路 ---- */

/* 构造 PRP：单页走 PRP1，多页 PRP1+PRP2(指向 PRP list 页) */
static blk_status_t nvme_setup_prps(struct nvme_queue *q, struct nvme_cmd *cmd,
				    struct request *rq, struct nvme_sqe *sqe)
{
	struct pcie_nvme_dev *dev = q->dev;
	u32 length = blk_rq_bytes(rq);
	dma_addr_t *prp_list;
	int i, nprps, first = 1;
	struct scatterlist *sg;

	cmd->nsegs = blk_rq_map_sg(rq->q, rq, cmd->sg);
	if (cmd->nsegs <= 0)
		return BLK_STS_IOERR;
	cmd->nsegs = dma_map_sg(&dev->pdev->dev, cmd->sg, cmd->nsegs,
				rq_data_dir(rq));
	if (cmd->nsegs <= 0)
		return BLK_STS_IOERR;

	/* 单页数据：PRP1 直接指向，无需 PRP list */
	if (length <= PAGE_SIZE) {
		sqe->prp1 = cpu_to_le64(sg_dma_address(&cmd->sg[0]));
		sqe->prp2 = 0;
		return BLK_STS_OK;
	}

	/* 多页：PRP1 = 第一页，PRP2 = PRP list 页 */
	sqe->prp1 = cpu_to_le64(sg_dma_address(&cmd->sg[0]));

	cmd->prp_list = dma_pool_alloc(dev->prp_pool, GFP_ATOMIC,
				       &cmd->prp_list_dma);
	if (!cmd->prp_list)
		return BLK_STS_RESOURCE;

	/* 逐页填充 PRP list（跳过 PRP1 已指向的第一页）。
	 * 块设备 IO 的 sg 页对齐且长度为页整数倍，此处按页拆分 */
	prp_list = cmd->prp_list;
	nprps = 0;
	for_each_sg(cmd->sg, sg, cmd->nsegs, i) {
		dma_addr_t dma = sg_dma_address(sg);
		int npages = DIV_ROUND_UP(sg_dma_len(sg), PAGE_SIZE);
		int j;

		for (j = 0; j < npages; j++) {
			if (first) {
				first = 0;
				continue;
			}
			prp_list[nprps++] = cpu_to_le64(dma + j * PAGE_SIZE);
		}
	}

	sqe->prp2 = cpu_to_le64(cmd->prp_list_dma);
	return BLK_STS_OK;
}

/* 释放 PRP list 并 unmap sg（complete 回调在软中断上下文调用） */
static void nvme_unmap_data(struct pcie_nvme_dev *dev, struct nvme_cmd *cmd,
			    struct request *rq)
{
	if (cmd->prp_list) {
		dma_pool_free(dev->prp_pool, cmd->prp_list, cmd->prp_list_dma);
		cmd->prp_list = NULL;
	}
	if (cmd->nsegs > 0)
		dma_unmap_sg(&dev->pdev->dev, cmd->sg, cmd->nsegs,
			     rq_data_dir(rq));
}

static void nvme_submit_sqe(struct nvme_queue *q, struct nvme_sqe *sqe)
{
	memcpy(&q->sq[q->sq_tail], sqe, sizeof(*sqe));
	q->sq_tail = (q->sq_tail + 1) % q->q_depth;
	writel(q->sq_tail, nvme_sq_doorbell(q->dev, q->qid));
}

/* 把 NVMe CQE status（SC+SCT 编码）映射为 blk-mq 通用返回码。
 * status 为 le16_to_cpu(cqe->status) >> 1（已去掉 P bit），
 * bit10:8 = SCT(Status Code Type)，bit7:0 = SC(Status Code)。
 * 对标内核 drivers/nvme/host/core.c 的 nvme_error_status。 */
static blk_status_t nvme_error_status(u16 status)
{
	switch (status & 0x7ff) {  /* 屏蔽 M/DNR/CRD，保留 SCT+SC */
	case NVME_SC_SUCCESS:
		return BLK_STS_OK;
	case NVME_SC_CAP_EXCEEDED:
		return BLK_STS_NOSPC;
	case NVME_SC_LBA_RANGE:
	case NVME_SC_CMD_INTERRUPTED:
	case NVME_SC_NS_NOT_READY:
		return BLK_STS_TARGET;
	case NVME_SC_BAD_ATTRIBUTES:
	case NVME_SC_ONCS_NOT_SUPPORTED:
	case NVME_SC_INVALID_OPCODE:
	case NVME_SC_INVALID_FIELD:
	case NVME_SC_INVALID_NS:
		return BLK_STS_NOTSUPP;
	case NVME_SC_WRITE_FAULT:
	case NVME_SC_READ_ERROR:
	case NVME_SC_UNWRITTEN_BLOCK:
	case NVME_SC_ACCESS_DENIED:
	case NVME_SC_READ_ONLY:
	case NVME_SC_COMPARE_FAILED:
		return BLK_STS_MEDIUM;
	case NVME_SC_GUARD_CHECK:
	case NVME_SC_APPTAG_CHECK:
	case NVME_SC_REFTAG_CHECK:
	case NVME_SC_INVALID_PI:
		return BLK_STS_PROTECTION;
	case NVME_SC_RESERVATION_CONFLICT:
		return BLK_STS_RESV_CONFLICT;
	case NVME_SC_HOST_PATH_ERROR:
		return BLK_STS_TRANSPORT;
	case NVME_SC_ZONE_TOO_MANY_ACTIVE:
		return BLK_STS_ZONE_ACTIVE_RESOURCE;
	case NVME_SC_ZONE_TOO_MANY_OPEN:
		return BLK_STS_ZONE_OPEN_RESOURCE;
	default:
		return BLK_STS_IOERR;
	}
}

/* 中断上半部：按 command_id 映射回 request，记录状态并触发异步完成 */
static void nvme_complete_rq(struct nvme_queue *q, struct nvme_cqe *cqe)
{
	struct pcie_nvme_dev *dev = q->dev;
	struct request *rq;
	struct nvme_cmd *cmd;
	u16 command_id = le16_to_cpu(cqe->command_id);
	u16 status = le16_to_cpu(cqe->status) >> 1;

	/* command_id 即提交时的 rq->tag，blk-mq 反向查 request */
	rq = blk_mq_tag_to_rq(q->tags, command_id);
	if (unlikely(!rq)) {
		dev_err(&dev->pdev->dev, "未找到 command_id=%u 的 request (qid=%u)\n",
			command_id, q->qid);
		return;
	}

	cmd = blk_mq_rq_to_pdu(rq);
	cmd->status = status;

	if (status) {
		blk_status_t bst = nvme_error_status(status);

		dev_err(&dev->pdev->dev,
			"I/O 命令失败: qid=%u tag=%u sct=%u sc=0x%02x dnr=%u -> blk_sts=%d\n",
			q->qid, command_id, (status >> 8) & 0x7,
			status & 0xff, !!(status & NVME_SC_DNR), (int)bst);
	}

	/* 触发异步完成：block 层软中断 -> nvme_mq_complete -> unmap + end_request */
	blk_mq_complete_request(rq);
}

/* blk-mq .complete 回调（软中断上下文）：unmap 数据 + 真正结束 request */
static void nvme_mq_complete(struct request *rq)
{
	struct pcie_nvme_dev *dev = rq->q->queuedata;
	struct nvme_cmd *cmd = blk_mq_rq_to_pdu(rq);
	blk_status_t ret = nvme_error_status(cmd->status);

	nvme_unmap_data(dev, cmd, rq);
	blk_mq_end_request(rq, ret);
}

/* 收割 CQ 里所有 phase 匹配的 CQE，返回收割条数（成功完成的命令数） */
static int nvme_process_cq(struct nvme_queue *q)
{
	struct nvme_cqe *cqe;
	int handled = 0;

	for (;;) {
		cqe = &q->cq[q->cq_head];
		if ((le16_to_cpu(cqe->status) & 0x1) != q->cq_phase)
			break;

		handled++;
		nvme_complete_rq(q, cqe);

		q->cq_head = (q->cq_head + 1) % q->q_depth;
		if (q->cq_head == 0)
			q->cq_phase = !q->cq_phase;
	}

	if (handled)
		writel(q->cq_head, nvme_cq_doorbell(q->dev, q->qid));
	return handled;
}

/* MSI-X 中断处理（irq_loss=1 时模拟丢中断，直接返回不收割） */
static irqreturn_t nvme_irq(int irq, void *data)
{
	struct nvme_queue *q = data;

	if (irq_loss)
		return IRQ_NONE;

	return nvme_process_cq(q) ? IRQ_HANDLED : IRQ_NONE;
}

/* blk-mq .timeout 回调（workqueue 上下文）：命令超时未完成时被 block 层调用 */
static enum blk_eh_timer_return nvme_timeout(struct request *rq)
{
	struct nvme_queue *q = rq->mq_hctx->driver_data;
	struct pcie_nvme_dev *dev = q->dev;

	dev_warn(&dev->pdev->dev,
		 "I/O 命令超时: qid=%u tag=%u dir=%s，主动轮询 CQ 恢复\n",
		 q->qid, rq->tag, rq_data_dir(rq) == WRITE ? "WRITE" : "READ");

	/*
	 * 超时最常见原因是「中断丢失」：设备已写 CQE 但中断未被处理。
	 * 主动收割 CQ 把已就绪的命令补完成；若恰好是本请求则随之结束。
	 * 竞态说明：中断正常时不会触发超时；本简单版不做中断屏蔽，
	 * 生产级需 per-queue 锁或改用 Abort/复位控制器。
	 */
	nvme_process_cq(q);

	/*
	 * 判断本请求是否已结束不能用 blk_mq_request_completed()：本驱动多队列
	 * 下 blk_mq_complete_request 是同步完成（非 IPI 路径直接调 .complete ->
	 * blk_mq_end_request -> blk_mq_free_request），完成后 state 被重置为
	 * MQ_RQ_IDLE 而非 MQ_RQ_COMPLETE。故用「不在 IN_FLIGHT」判断：已
	 * COMPLETE 或已 IDLE 都算完成。
	 */
	if (blk_mq_rq_state(rq) != MQ_RQ_IN_FLIGHT)
		return BLK_EH_DONE;

	dev_err(&dev->pdev->dev,
		"命令仍卡住: qid=%u tag=%u 无 CQE，重置计时器继续等待（进阶：发 Abort 或复位）\n",
		q->qid, rq->tag);
	return BLK_EH_RESET_TIMER;
}

static blk_status_t nvme_map_and_submit(struct nvme_queue *q,
					struct request *rq, struct nvme_cmd *cmd)
{
	struct pcie_nvme_dev *dev = q->dev;
	struct nvme_sqe sqe = { 0 };
	blk_status_t ret;

	cmd->prp_list = NULL;
	cmd->nsegs = 0;

	if (blk_rq_bytes(rq) == 0) {
		/* flush 等零数据命令 */
		sqe.opcode = NVME_CMD_FLUSH;
		sqe.nsid = cpu_to_le32(dev->nsid);
	} else {
		u64 slba = blk_rq_pos(rq) >> (dev->lba_shift - 9);

		ret = nvme_setup_prps(q, cmd, rq, &sqe);
		if (ret != BLK_STS_OK)
			return ret;

		sqe.opcode = (rq_data_dir(rq) == WRITE) ?
			     NVME_CMD_WRITE : NVME_CMD_READ;
		sqe.nsid = cpu_to_le32(dev->nsid);
		/* SLBA / NLB（LBA 与 512B 扇区按 lba_shift 换算） */
		sqe.cdw10 = cpu_to_le32(slba & 0xffffffff);
		sqe.cdw11 = cpu_to_le32(slba >> 32);
		sqe.cdw12 = cpu_to_le32(
			(blk_rq_sectors(rq) >> (dev->lba_shift - 9)) - 1);
	}
	sqe.command_id = rq->tag;

	nvme_submit_sqe(q, &sqe);
	return BLK_STS_OK;
}

static blk_status_t nvme_queue_rq(struct blk_mq_hw_ctx *hctx,
				  const struct blk_mq_queue_data *bd)
{
	struct nvme_queue *q = hctx->driver_data;
	struct request *rq = bd->rq;
	struct nvme_cmd *cmd = blk_mq_rq_to_pdu(rq);
	blk_status_t ret;

	blk_mq_start_request(rq);

	/* 阶段4 异步完成：提交后立即返回，中断里 blk_mq_complete_request */
	ret = nvme_map_and_submit(q, rq, cmd);
	if (ret != BLK_STS_OK)
		nvme_unmap_data(q->dev, cmd, rq);

	return ret;
}

static int nvme_init_hctx(struct blk_mq_hw_ctx *hctx, void *data,
			  unsigned int hctx_idx)
{
	struct pcie_nvme_dev *dev = data;

	hctx->driver_data = &dev->queues[hctx_idx];
	return 0;
}

static const struct blk_mq_ops nvme_mq_ops = {
	.queue_rq = nvme_queue_rq,
	.complete = nvme_mq_complete,
	.init_hctx = nvme_init_hctx,
	.timeout  = nvme_timeout,
};

static const struct block_device_operations nvme_fops = {
	.owner = THIS_MODULE,
};

static int nvme_setup_disk(struct pcie_nvme_dev *dev)
{
	struct gendisk *disk;
	int ret;

	dev->queues = kcalloc(dev->nr_queues, sizeof(*dev->queues), GFP_KERNEL);
	if (!dev->queues)
		return -ENOMEM;

	memset(&dev->tag_set, 0, sizeof(dev->tag_set));
	dev->tag_set.ops = &nvme_mq_ops;
	dev->tag_set.nr_hw_queues = dev->nr_queues;
	dev->tag_set.queue_depth = NVME_IO_Q_DEPTH;
	dev->tag_set.numa_node = NUMA_NO_NODE;
	/* 阶段4 异步：queue_rq 运行在原子上下文，去掉 BLK_MQ_F_BLOCKING */
	dev->tag_set.flags = BLK_MQ_F_SHOULD_MERGE;
	dev->tag_set.cmd_size = sizeof(struct nvme_cmd);
	dev->tag_set.driver_data = dev;
	/* I/O 命令超时（jiffies），超时后 block 层调 nvme_timeout */
	dev->tag_set.timeout = io_timeout * HZ;

	ret = blk_mq_alloc_tag_set(&dev->tag_set);
	if (ret) {
		kfree(dev->queues);
		return ret;
	}

	disk = blk_mq_alloc_disk(&dev->tag_set, dev);
	if (IS_ERR(disk)) {
		ret = PTR_ERR(disk);
		goto out_free_tagset;
	}

	disk->major = 0;      /* 0 = 动态分配 major（ext devt），此时 minors 必须为 0 */
	disk->fops = &nvme_fops;
	disk->private_data = dev;
	snprintf(disk->disk_name, DISK_NAME_LEN, "nvme%dn%d", 0, dev->nsid);

	set_capacity(disk, dev->nsze << (dev->lba_shift - 9));
	blk_queue_logical_block_size(disk->queue, 1u << dev->lba_shift);
	blk_queue_physical_block_size(disk->queue, 1u << dev->lba_shift);
	blk_queue_max_hw_sectors(disk->queue, 4096); /* 2MB，单 PRP list 页上限 */

	dev->disk = disk;
	return 0;

out_free_tagset:
	blk_mq_free_tag_set(&dev->tag_set);
	kfree(dev->queues);
	return ret;
}

static int pcie_nvme_probe(struct pci_dev *pdev, const struct pci_device_id *id)
{
	struct pcie_nvme_dev *dev;
	u64 cap;
	u32 vs, csts;
	int ret, i;

	dev_info(&pdev->dev, "probe: BDF=%04x:%02x:%02x.%d class=0x%06x\n",
		 pci_domain_nr(pdev->bus), pdev->bus->number,
		 PCI_SLOT(pdev->devfn), PCI_FUNC(pdev->devfn), pdev->class);

	ret = pci_enable_device_mem(pdev);
	if (ret) {
		dev_err(&pdev->dev, "pci_enable_device_mem failed: %d\n", ret);
		return ret;
	}
	/* NVMe 是 DMA 设备，必须使能 Bus Master，否则读不到 SQ/写不了 CQ */
	pci_set_master(pdev);

	dev = kzalloc(sizeof(*dev), GFP_KERNEL);
	if (!dev) {
		ret = -ENOMEM;
		goto out_disable_device;
	}
	dev->pdev = pdev;
	pci_set_drvdata(pdev, dev);

	dev->bar0_start = pci_resource_start(pdev, 0);
	dev->bar0_len   = pci_resource_len(pdev, 0);
	if (!dev->bar0_start || !dev->bar0_len) {
		dev_err(&pdev->dev, "BAR0 无效\n");
		ret = -ENODEV;
		goto out_free_dev;
	}

	ret = pci_request_mem_regions(pdev, "pcie_nvme");
	if (ret) {
		dev_err(&pdev->dev, "pci_request_mem_regions failed: %d\n", ret);
		goto out_free_dev;
	}

	dev->bar0 = devm_ioremap(&pdev->dev, dev->bar0_start, dev->bar0_len);
	if (!dev->bar0) {
		dev_err(&pdev->dev, "devm_ioremap BAR0 failed\n");
		ret = -ENOMEM;
		goto out_release_regions;
	}

	cap  = readq(dev->bar0 + NVME_REG_CAP);
	vs   = readl(dev->bar0 + NVME_REG_VS);
	csts = readl(dev->bar0 + NVME_REG_CSTS);
	dev->dstrd = NVME_CAP_DSTRD(cap);
	dev_info(&pdev->dev, "CAP=0x%016llx VS=0x%08x DSTRD=%u 字节\n",
		 cap, vs, dev->dstrd);

	/* 分配 Admin SQ/CQ 与 Identify 缓冲 */
	dev->admin_sq = dmam_alloc_coherent(&pdev->dev,
			NVME_ADMIN_Q_DEPTH * sizeof(struct nvme_sqe),
			&dev->admin_sq_dma, GFP_KERNEL);
	if (!dev->admin_sq) {
		ret = -ENOMEM;
		goto out_release_regions;
	}
	dev->admin_cq = dmam_alloc_coherent(&pdev->dev,
			NVME_ADMIN_Q_DEPTH * sizeof(struct nvme_cqe),
			&dev->admin_cq_dma, GFP_KERNEL);
	if (!dev->admin_cq) {
		ret = -ENOMEM;
		goto out_release_regions;
	}
	dev->identify = dmam_alloc_coherent(&pdev->dev, 4096,
			&dev->identify_dma, GFP_KERNEL);
	if (!dev->identify) {
		ret = -ENOMEM;
		goto out_release_regions;
	}
	if ((dev->admin_sq_dma & 0xfff) || (dev->admin_cq_dma & 0xfff) ||
	    (dev->identify_dma & 0xfff)) {
		dev_err(&pdev->dev, "DMA 缓冲区未 4KB 对齐\n");
		ret = -EIO;
		goto out_release_regions;
	}

	dev->sq_tail = 0;
	dev->cq_head = 0;
	dev->cq_phase = 1;

	ret = nvme_enable_ctrl(dev);
	if (ret)
		goto out_release_regions;

	/* Identify Controller（阶段2链路复验） */
	ret = nvme_identify_ctrl(dev);
	if (ret)
		goto out_disable_ctrl;
	nvme_dump_identify_ctrl(dev);

	/* Identify Namespace，读容量与 LBA 格式 */
	dev->nsid = 1;   /* QEMU 默认第一个 namespace */
	ret = nvme_identify_ns(dev);
	if (ret)
		goto out_disable_ctrl;
	nvme_parse_ns(dev);

	/* PRP list 页池（一页放 512 个 8 字节 PRP 条目） */
	dev->prp_pool = dma_pool_create("nvme_prp", &pdev->dev,
					PAGE_SIZE, PAGE_SIZE, 0);
	if (!dev->prp_pool) {
		ret = -ENOMEM;
		goto out_disable_ctrl;
	}

	/* 计算 I/O 队列数：每 CPU 一个，上限 MQES+1 */
	dev->nr_queues = min_t(int, num_online_cpus(), NVME_CAP_MQES(cap) + 1);

	/* 先分配 MSI-X 向量确定实际可用队列数（每队列一个向量，MSI-X 优先回退 MSI） */
	ret = pci_alloc_irq_vectors(pdev, 1, dev->nr_queues,
				    PCI_IRQ_MSIX | PCI_IRQ_MSI);
	if (ret < 0) {
		dev_err(&pdev->dev, "pci_alloc_irq_vectors failed: %d\n", ret);
		goto out_destroy_pool;
	}
	dev->nr_queues = min_t(int, dev->nr_queues, ret);
	dev_info(&pdev->dev, "已分配 %d 个 %s 中断向量, I/O 队列数=%d\n",
		 ret, pdev->msix_enabled ? "MSI-X" : "MSI", dev->nr_queues);

	/* blk-mq + gendisk（用最终 nr_queues 建 tag_set） */
	ret = nvme_setup_disk(dev);
	if (ret)
		goto out_free_irq_vectors;

	/* 循环创建 I/O 队列 + 绑定中断（在 add_disk 前，避免提前有 IO） */
	for (i = 0; i < dev->nr_queues; i++) {
		ret = nvme_alloc_io_queue(dev, &dev->queues[i], i + 1);
		if (ret)
			goto out_free_irq;
	}

	ret = add_disk(dev->disk);
	if (ret)
		goto out_free_irq;

	dev_info(&pdev->dev, "块设备 /dev/nvme%dn%d 注册成功, 容量=%llu LBA, I/O 队列=%d\n",
		 0, dev->nsid, dev->nsze, dev->nr_queues);
	return 0;

out_free_irq:
	/* 清理已成功创建的 I/O 队列（第 i 个失败，前 i 个已建） */
	while (i > 0) {
		i--;
		free_irq(dev->queues[i].irq, &dev->queues[i]);
		nvme_delete_io_queue(dev, &dev->queues[i]);
	}
out_free_irq_vectors:
	pci_free_irq_vectors(pdev);
	/* nvme_setup_disk 失败时内部已释放 tag_set/queues，dev->disk 为 NULL 跳过 */
	if (dev->disk) {
		put_disk(dev->disk);
		blk_mq_free_tag_set(&dev->tag_set);
		kfree(dev->queues);
	}
out_destroy_pool:
	dma_pool_destroy(dev->prp_pool);
out_disable_ctrl:
	nvme_disable_ctrl(dev);
out_release_regions:
	pci_release_mem_regions(pdev);
out_free_dev:
	kfree(dev);
out_disable_device:
	pci_disable_device(pdev);
	return ret;
}

static void pcie_nvme_remove(struct pci_dev *pdev)
{
	struct pcie_nvme_dev *dev = pci_get_drvdata(pdev);
	int i;

	dev_info(&pdev->dev, "remove\n");

	if (dev->disk)
		del_gendisk(dev->disk);

	/* del_gendisk 已 quiesce 队列、等在途 IO 完成，可安全释放中断 */
	for (i = 0; i < dev->nr_queues; i++) {
		free_irq(dev->queues[i].irq, &dev->queues[i]);
		nvme_delete_io_queue(dev, &dev->queues[i]);
	}

	pci_free_irq_vectors(pdev);

	blk_mq_free_tag_set(&dev->tag_set);

	if (dev->prp_pool)
		dma_pool_destroy(dev->prp_pool);

	if (dev->disk)
		put_disk(dev->disk);

	nvme_disable_ctrl(dev);

	kfree(dev->queues);

	/* bar0/devm_ioremap、DMA/dmam_alloc_coherent 由 devres 自动释放 */
	pci_release_mem_regions(pdev);
	pci_disable_device(pdev);
	kfree(dev);
}

static struct pci_driver pcie_nvme_driver = {
	.name     = "pcie_nvme",
	.id_table = pcie_nvme_id_table,
	.probe    = pcie_nvme_probe,
	.remove   = pcie_nvme_remove,
};

module_pci_driver(pcie_nvme_driver);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("cxr");
MODULE_DESCRIPTION("阶段5c: 多队列 NVMe 块设备驱动 (blk-mq + MSI-X + 超时 + status code 细分)");
