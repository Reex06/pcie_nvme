// SPDX-License-Identifier: GPL-2.0
/*
 * pcie_nvme.c - 阶段5：多队列扩展（每 CPU 一个 I/O 队列 + 独立 MSI-X 向量）
 *
 * 阶段1：PCIe 枚举 + BAR0 映射 + MMIO 寄存器读取。
 * 阶段2：控制器初始化 + Admin Queue + Identify Controller 命令往返。
 * 阶段3：I/O 队列 + Write/Read + PRP，同步轮询打通数据通路。
 * 阶段4：MSI-X 中断 + 异步完成（blk_mq_complete_request + .complete 回调）。
 * 本阶段新增（单队列 -> 每 CPU 一队列）：
 *   1. nr_queues = min(num_online_cpus(), MQES+1)，每队列独立 SQ/CQ
 *   2. 每队列绑定独立 MSI-X 向量（Create I/O CQ 的 IV=qid-1）
 *   3. blk-mq nr_hw_queues=nr_queues，init_hctx 按 hctx_idx 分发到队列
 *   4. command_id=rq->tag 在多 hctx 下仍 per-hctx 唯一
 *
 * 面试考点：
 *   - MSI-X 与 NVMe 的对应关系：每个 I/O CQ 一个中断向量（IV 字段绑定）
 *   - command_id 复用 blk-mq 的 tag，完成时 blk_mq_tag_to_rq 反向映射
 *   - 中断上半部收割 CQ + 敲 CQ head doorbell，下半部由 block 层 softirq 完成
 *   - PRP 是 NVMe 传数据的核心：单页走 PRP1，多页 PRP1+PRP2 指向 PRP list 页
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

	if (status)
		dev_err(&dev->pdev->dev, "I/O 命令失败: qid=%u status=0x%x\n",
			q->qid, status);

	/* 触发异步完成：block 层软中断 -> nvme_mq_complete -> unmap + end_request */
	blk_mq_complete_request(rq);
}

/* blk-mq .complete 回调（软中断上下文）：unmap 数据 + 真正结束 request */
static void nvme_mq_complete(struct request *rq)
{
	struct pcie_nvme_dev *dev = rq->q->queuedata;
	struct nvme_cmd *cmd = blk_mq_rq_to_pdu(rq);
	blk_status_t ret = cmd->status ? BLK_STS_IOERR : BLK_STS_OK;

	nvme_unmap_data(dev, cmd, rq);
	blk_mq_end_request(rq, ret);
}

/* MSI-X 中断处理：循环收割 CQ 直到 phase 不匹配，最后一次性敲 CQ head doorbell */
static irqreturn_t nvme_irq(int irq, void *data)
{
	struct nvme_queue *q = data;
	struct nvme_cqe *cqe;
	int handled = 0;

	for (;;) {
		cqe = &q->cq[q->cq_head];
		if ((le16_to_cpu(cqe->status) & 0x1) != q->cq_phase)
			break;

		handled = 1;
		nvme_complete_rq(q, cqe);

		q->cq_head = (q->cq_head + 1) % q->q_depth;
		if (q->cq_head == 0)
			q->cq_phase = !q->cq_phase;
	}

	if (handled) {
		writel(q->cq_head, nvme_cq_doorbell(q->dev, q->qid));
		return IRQ_HANDLED;
	}
	return IRQ_NONE;
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
MODULE_DESCRIPTION("阶段5: 多队列 NVMe 块设备驱动 (blk-mq + MSI-X)");
