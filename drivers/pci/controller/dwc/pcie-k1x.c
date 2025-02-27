// SPDX-License-Identifier: GPL-2.0
/*
 * Spacemit k1x PCIe rc && ep driver
 *
 * Copyright (c) 2023, spacemit Corporation.
 *
 */
#include <linux/clk.h>
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/irqdomain.h>
#include <linux/kernel.h>
#include <linux/init.h>
#include <linux/of_device.h>
#include <linux/of_gpio.h>
#include <linux/of_pci.h>
#include <linux/pci.h>
#include <linux/phy/phy.h>
#include <linux/platform_device.h>
#include <linux/pm_runtime.h>
#include <linux/resource.h>
#include <linux/types.h>
#include <linux/mfd/syscon.h>
#include <linux/regmap.h>
#include <linux/reset.h>

#include "../../pci.h"
#include "pcie-designware.h"

/* PCIe controller wrapper k1x configuration registers */

#define	K1X_PHY_AHB_IRQ_EN              0x0000
#define	IRQ_EN						BIT(0)

#define	K1X_PHY_AHB_IRQSTATUS_INTX      0x0008
#define	INTA						BIT(6)
#define	INTB						BIT(7)
#define	INTC						BIT(8)
#define	INTD						BIT(9)
#define	LEG_EP_INTERRUPTS (INTA | INTB | INTC | INTD)
#define INTX_MASK				GENMASK(9, 6)
#define INTX_SHIFT					6

#define	K1X_PHY_AHB_IRQENABLE_SET_INTX  0x000c

#define	K1X_PHY_AHB_IRQSTATUS_MSI   0x0010
#define	MSI						BIT(11)
#define	PCIE_REMOTE_INTERRUPT				BIT(31)
/* DMA write channel 0~7 irq*/
#define	EDMA_INT0					BIT(0)
#define	EDMA_INT1					BIT(1)
#define	EDMA_INT2					BIT(2)
#define	EDMA_INT3					BIT(3)
#define	EDMA_INT4					BIT(4)
#define	EDMA_INT5					BIT(5)
#define	EDMA_INT6					BIT(6)
#define	EDMA_INT7					BIT(7)
/* DMA read channel 0~7 irq*/
#define	EDMA_INT8					BIT(8)
#define	EDMA_INT9					BIT(9)
#define	EDMA_INT10					BIT(10)
#define	EDMA_INT11					BIT(11)
#define	EDMA_INT12					BIT(12)
#define	EDMA_INT13					BIT(13)
#define	EDMA_INT14					BIT(14)
#define	EDMA_INT15					BIT(15)
#define	DMA_READ_INT					GENMASK(11, 8)

#define	K1X_PHY_AHB_IRQENABLE_SET_MSI			0x0014

#define	PCIECTRL_K1X_CONF_DEVICE_CMD			0x0000
#define	LTSSM_EN					BIT(6)
/* Perst input value in ep mode */
#define	PCIE_PERST_IN					BIT(7)
/* Perst GPIO en in RC mode 1: perst# low, 0: perst# high */
#define	PCIE_RC_PERST					BIT(12)
/* Wake# GPIO in EP mode 1: Wake# low, 0: Wake# high */
#define	PCIE_EP_WAKE					BIT(13)
#define	APP_HOLD_PHY_RST				BIT(30)
/* BIT31 0: EP, 1: RC*/
#define	DEVICE_TYPE_RC					BIT(31)

#define	PCIE_CTRL_LOGIC					0x0004
#define	PCIE_IGNORE_PERSTN				BIT(2)

#define	K1X_PHY_AHB_LINK_STS				0x0004
#define	SMLH_LINK_UP					BIT(1)
#define	RDLH_LINK_UP					BIT(12)

#define ADDR_INTR_STATUS1       			0x0018
#define ADDR_INTR_ENABLE1 				0x001C
#define MSI_INT						BIT(0)
#define MSIX_INT 					GENMASK(8, 1)

#define ADDR_MSI_RECV_CTRL 				0x0080
#define MSI_MON_EN 					BIT(0)
#define MSIX_MON_EN 					GENMASK(8, 1)
#define MSIX_AFIFO_FULL 				BIT(30)
#define MSIX_AFIFO_EMPTY 				BIT(29)
#define ADDR_MSI_RECV_ADDR0 				0x0084
#define ADDR_MSIX_MON_MASK 				0x0088
#define ADDR_MSIX_MON_BASE0 				0x008c

#define ADDR_MON_FIFO_DATA0 				0x00b0
#define ADDR_MON_FIFO_DATA1 				0x00b4
#define FIFO_EMPTY 					0xFFFFFFFF
#define FIFO_LEN 					32
#define INT_VEC_MASK 					GENMASK(7, 0)

#define EXP_CAP_ID_OFFSET				0x70

#define	PCIECTRL_K1X_CONF_INTX_ASSERT			0x0124
#define	PCIECTRL_K1X_CONF_INTX_DEASSERT			0x0128

/*RC write config  0xD28 offset register which equal with ELBI offset 0x028 addr*/
#define PCIE_ELBI_EP_DMA_IRQ_STATUS	0x028
#define PC_TO_EP_INT			(0x3fffffff)

#define PCIE_ELBI_EP_DMA_IRQ_MASK	0x02c
#define PC_TO_EP_INT_MASK		(0x3fffffff)

#define PCIE_ELBI_EP_MSI_REASON         0x018

struct k1x_pcie {
	struct dw_pcie		*pci;
	void __iomem		*base;		/* DT k1x_conf */
	void __iomem		*elbi_base;
	void __iomem		*dma_base;
	void __iomem		*phy_ahb;		/* DT phy_ahb */
	void __iomem		*phy_addr;		/* DT phy_addr */
	void __iomem		*conf0_addr;		/* DT conf0_addr */
	void __iomem		*phy0_addr;		/* DT phy0_addr */
	int			phy_count;	/* DT phy-names count */
	struct phy		**phy;
	int pcie_init_before_kernel;
	int			port_id;
	int			num_lanes;
	int			link_gen;
	struct irq_domain	*irq_domain;
	enum dw_pcie_device_mode mode;
	struct page             *msi_page;
	struct page             *msix_page;
	dma_addr_t              msix_addr;
	struct	clk *clk_pcie;  /*include master slave slave_lite clk*/
	struct	clk *clk_master;
	struct	clk *clk_slave;
	struct	clk *clk_slave_lite;
	struct reset_control *reset;

	struct	gpio_desc *perst_gpio; /* for PERST# in RC mode*/
};

struct k1x_pcie_of_data {
	enum dw_pcie_device_mode mode;
};

#define to_k1x_pcie(x)	dev_get_drvdata((x)->dev)

static inline u32 k1x_pcie_readl_dma(struct k1x_pcie *pcie, u32 reg)
{
	return readl(pcie->dma_base + reg);
}

static inline void k1x_pcie_writel_dma(struct k1x_pcie *pcie, u32 reg, u32 val)
{
	writel(val, pcie->dma_base + reg);
}

static inline u32 k1x_pcie_readw_dma(struct k1x_pcie *pcie, u32 reg)
{
	return (u32)readw(pcie->dma_base + reg);
}

static inline void k1x_pcie_writew_dma(struct k1x_pcie *pcie, u32 reg, u32 val)
{
	writew((u16)val, pcie->dma_base + reg);
}

static inline u32 k1x_pcie_readl(struct k1x_pcie *pcie, u32 offset)
{
	return readl(pcie->base + offset);
}

static inline void k1x_pcie_writel(struct k1x_pcie *pcie, u32 offset,
				      u32 value)
{
	writel(value, pcie->base + offset);
}

static inline u32 k1x_pcie_readl_elbi(struct k1x_pcie *pcie, u32 reg)
{
	return readl(pcie->elbi_base + reg);
}

static inline void k1x_pcie_writel_elbi(struct k1x_pcie *pcie, u32 reg, u32 val)
{
	writel(val, pcie->elbi_base + reg);
}

static inline u32 k1x_pcie_phy_ahb_readl(struct k1x_pcie *pcie, u32 offset)
{
	return readl(pcie->phy_ahb + offset);
}

static inline void k1x_pcie_phy_ahb_writel(struct k1x_pcie *pcie, u32 offset,
				      u32 value)
{
	writel(value, pcie->phy_ahb + offset);
}

static inline u32 k1x_pcie_phy_reg_readl(struct k1x_pcie *pcie, u32 offset)
{
	return readl(pcie->phy_addr + offset);
}

static inline void k1x_pcie_phy_reg_writel(struct k1x_pcie *pcie, u32 offset,
				      u32 value)
{
	writel(value, pcie->phy_addr + offset);
}

static inline u32 k1x_pcie_conf0_reg_readl(struct k1x_pcie *pcie, u32 offset)
{
	return readl(pcie->conf0_addr + offset);
}

static inline void k1x_pcie_conf0_reg_writel(struct k1x_pcie *pcie, u32 offset,
				      u32 value)
{
	writel(value, pcie->conf0_addr + offset);
}

static inline u32 k1x_pcie_phy0_reg_readl(struct k1x_pcie *pcie, u32 offset)
{
	return readl(pcie->phy0_addr + offset);
}

static inline void k1x_pcie_phy0_reg_writel(struct k1x_pcie *pcie, u32 offset,
				      u32 value)
{
	writel(value, pcie->phy0_addr + offset);
}

#define PCIE_REF_CLK_OUTPUT
static int porta_init_done = 0;
// wait porta rterm done
void porta_rterm(struct k1x_pcie *k1x)
{
	int rd_data;
	u32 val;

	//REG32(PMUA_REG_BASE + 0x3CC) = 0x4000003f;
	val = k1x_pcie_conf0_reg_readl(k1x, 0);
	val = 0x4000003f;
	k1x_pcie_conf0_reg_writel(k1x, 0 , val);

	//REG32(PMUA_REG_BASE + 0x3CC) &= 0xbfffffff; // clear hold phy reset
	val = k1x_pcie_conf0_reg_readl(k1x, 0);
	val &= 0xbfffffff;
	k1x_pcie_conf0_reg_writel(k1x, 0 , val);

	// set refclk model
	//REG32(0xC0B10000 + (0x17 << 2)) |= (0x1 << 10);
	val = k1x_pcie_phy0_reg_readl(k1x, (0x17 << 2));
	val |= (0x1 << 10);
	k1x_pcie_phy0_reg_writel(k1x, (0x17 << 2), val);

	//REG32(0xC0B10000 + (0x17 << 2)) &= ~(0x3 << 8);
	val = k1x_pcie_phy0_reg_readl(k1x, (0x17 << 2));
	val &= ~(0x3 << 8);
	k1x_pcie_phy0_reg_writel(k1x, (0x17 << 2), val);


#ifndef PCIE_REF_CLK_OUTPUT
	// receiver mode
	//REG32(0xC0B10000 + (0x17 << 2)) |= 0x2 << 8;
	val = k1x_pcie_phy0_reg_readl(k1x, (0x17 << 2));
	val |= 0x2 << 8;
	k1x_pcie_phy0_reg_writel(k1x, (0x17 << 2), val);

	//REG32(0xC0B10000 + (0x8 << 2)) &= ~(0x1 << 29);
	val = k1x_pcie_phy0_reg_readl(k1x, (0x8 << 2));
	val &= ~(0x1 << 29);
	k1x_pcie_phy0_reg_writel(k1x, (0x8 << 2), val);
#ifdef PCIE_SEL_24M_REF_CLK
	//REG32(0xC0B10000 + (0x12 << 2)) &= 0xffff0fff;
	val = k1x_pcie_phy0_reg_readl(k1x, (0x12 << 2));
	val &= 0xffff0fff;
	k1x_pcie_phy0_reg_writel(k1x, (0x12 << 2), val);

	//REG32(0xC0B10000 + (0x12 << 2)) |= 0x00002000; // select 24Mhz refclock input pll_reg1[15:13]=2
	val = k1x_pcie_phy0_reg_readl(k1x, (0x12 << 2));
	val |= 0x00002000;
	k1x_pcie_phy0_reg_writel(k1x, (0x12 << 2), val);

	//REG32(0xC0B10000 + (0x8 << 2)) |= 0x3 << 29;	   // rc_cal_reg2 0x68
	val = k1x_pcie_phy0_reg_readl(k1x, (0x8 << 2));
	val |= 0x3 << 29;
	k1x_pcie_phy0_reg_writel(k1x, (0x8 << 2), val);
#elif PCIE_SEL_100M_REF_CLK
	//REG32(0xC0B10000 + (0x8 << 2)) |= 0x1 << 30; // rc_cal_reg2 0x48
	val = k1x_pcie_phy0_reg_readl(k1x, (0x8 << 2));
	val |= 0x1 << 30;
	k1x_pcie_phy0_reg_writel(k1x, (0x8 << 2), val);
#endif
	//REG32(0xC0B10000 + (0x14 << 2)) |= (0x1 << 3); // pll_reg9[3] en_rterm,only enable in receiver mode
	val = k1x_pcie_phy0_reg_readl(k1x, (0x14 << 2));
	val |= (0x1 << 3);
	k1x_pcie_phy0_reg_writel(k1x, (0x14 << 2), val);
#else
	// driver mode
	//REG32(0xC0B10000 + (0x17 << 2)) |= 0x1 << 8;
	val = k1x_pcie_phy0_reg_readl(k1x, (0x17 << 2));
	val |= 0x1 << 8;
	k1x_pcie_phy0_reg_writel(k1x, (0x17 << 2), val);

	//REG32(0xC0B10000 + 0x400 + (0x17 << 2)) |= 0x1 << 8;
	val = k1x_pcie_phy0_reg_readl(k1x, 0x400 + (0x17 << 2));
	val |= 0x1 << 8;
	k1x_pcie_phy0_reg_writel(k1x, 0x400 + (0x17 << 2), val);

	//REG32(0xC0B10000 + (0x12 << 2)) &= 0xffff0fff;
	val = k1x_pcie_phy0_reg_readl(k1x, (0x12 << 2));
	val &= 0xffff0fff;
	k1x_pcie_phy0_reg_writel(k1x, (0x12 << 2), val);

	//REG32(0xC0B10000 + (0x12 << 2)) |= 0x00002000; // select 24Mhz refclock input pll_reg1[15:13]=2
	val = k1x_pcie_phy0_reg_readl(k1x, (0x12 << 2));
	val |= 0x00002000;
	k1x_pcie_phy0_reg_writel(k1x, (0x12 << 2), val);

	//REG32(0xC0B10000 + (0x13 << 2)) |= (0x1 << 4); // pll_reg5[4] of lane0, enable refclk_100_n/p 100Mhz output
	val = k1x_pcie_phy0_reg_readl(k1x, (0x13 << 2));
	val |= (0x1 << 4);
	k1x_pcie_phy0_reg_writel(k1x, (0x13 << 2), val);

	//// REG32(0xC0B10000+(0x14<<2)) |= (0x1<<3);//pll_reg9[3] en_rterm,only enable in receiver mode
#endif

	//REG32(0xC0B10000 + (0x12 << 2)) &= 0xfff0ffff; // pll_reg1 of lane0, disable ssc pll_reg4[3:0]=4'h0
	val = k1x_pcie_phy0_reg_readl(k1x, (0x12 << 2));
	val &= 0xfff0ffff;
	k1x_pcie_phy0_reg_writel(k1x, (0x12 << 2), val);

	//REG32(0xC0B10000 + (0x02 << 2)) = 0x00000B78; // PU_ADDR_CLK_CFG of lane0
	val = k1x_pcie_phy0_reg_readl(k1x, (0x02 << 2));
	val = 0x00000B78;
	k1x_pcie_phy0_reg_writel(k1x, (0x02 << 2), val);

	//REG32(0xC0B10000 + (0x06 << 2)) = 0x00000400; // force rcv done
	val = k1x_pcie_phy0_reg_readl(k1x, (0x06 << 2));
	val = 0x00000400;
	k1x_pcie_phy0_reg_writel(k1x, (0x06 << 2), val);
	printk("Now waiting portA resister tuning done...\n");

	// force PCIE mpu_u3/pu_rx_lfps
	//REG32(PCIE_PUPHY_REG_BASE + 0x6 * 4) |= (0x1 << 17) | (0x1 << 15);
	val = k1x_pcie_phy_reg_readl(k1x, (0x6 * 4));
	val |= ((0x1 << 17) | (0x1 << 15));
	k1x_pcie_phy_reg_writel(k1x, (0x6 * 4), val);

	// wait pm0 rterm done
	do
	{
		//rd_data = REG32(0xC0B10000 + 0x21 * 4);
		rd_data = k1x_pcie_phy0_reg_readl(k1x, (0x21 * 4));
		printk("porta redonly_reg2: %08x\n", rd_data);
	} while (((rd_data >> 10) & 0x1) == 0); // waiting PCIe portA readonly_reg2[2] r_tune_done==1
}

// force rterm value to porta/b/c
void rterm_force(struct k1x_pcie *k1x, u32 pcie_rcal)
{
	int i, lane;
	u32 val = 0;

	lane = k1x->num_lanes;
	printk("pcie_rcal = 0x%08x\n", pcie_rcal);
	printk("pcie port id = %d, lane num = %d\n", k1x->port_id, lane);

	// 2.write pma0 rterm value LSB[3:0](read0nly1[3:0]) to lane0/1 rx_reg1
	for (i = 0; i < lane; i++)
	{
		val = k1x_pcie_phy_reg_readl(k1x, ((0x14 << 2) + 0x400 * i));
		val |= ((pcie_rcal & 0xf) << 8);
		k1x_pcie_phy_reg_writel(k1x, ((0x14 << 2) + 0x400 * i), val);
	}
	// 3.set lane0/1 rx_reg4 bit5=0
	for (i = 0; i < lane; i++)
	{
		val = k1x_pcie_phy_reg_readl(k1x, ((0x15 << 2) + 0x400 * i));
		val &= ~(1 << 5);
		k1x_pcie_phy_reg_writel(k1x, ((0x15 << 2) + 0x400 * i), val);
	}

	// 4.write pma0 rterm value MSB[7:4](readonly1[7:4]) to lane0/1 tx_reg1[7:4]
	for (i = 0; i < lane; i++)
	{
		val = k1x_pcie_phy_reg_readl(k1x, ((0x19 << 2) + 0x400 * i));
		val |= ((pcie_rcal >> 4) & 0xf) << 12;
		k1x_pcie_phy_reg_writel(k1x, ((0x19 << 2) + 0x400 * i), val);
	}

	// 5.set lane0/1 tx_reg3 bit1=1
	for (i = 0; i < lane; i++)
	{
		val = k1x_pcie_phy_reg_readl(k1x, ((0x19 << 2) + 0x400 * i));
		val |= (1 << 25);
		k1x_pcie_phy_reg_writel(k1x, ((0x19 << 2) + 0x400 * i), val);
	}

	// 6.adjust rc calrefclk freq
#ifndef PCIE_REF_CLK_OUTPUT
	//REG32(PCIE_PUPHY_REG_BASE + (0x8 << 2)) &= ~(0x1 << 29);
	val = k1x_pcie_phy_reg_readl(k1x,  (0x8 << 2));
	val &= ~(0x1 << 29);
	k1x_pcie_phy_reg_writel(k1x,  (0x8 << 2), val);
#ifdef PCIE_SEL_24M_REF_CLK
	//REG32(PCIE_PUPHY_REG_BASE + (0x8 << 2)) |= 0x3 << 29; // rc_cal_reg2 0x68
	val = k1x_pcie_phy_reg_readl(k1x,  (0x8 << 2));
	val |= 0x3 << 29;
	k1x_pcie_phy_reg_writel(k1x,  (0x8 << 2), val);
#elif PCIE_SEL_100M_REF_CLK
	//REG32(PCIE_PUPHY_REG_BASE + (0x8 << 2)) |= 0x1 << 30; // rc_cal_reg2 0x48
	val = k1x_pcie_phy_reg_readl(k1x,  (0x8 << 2));
	val |= 0x1 << 30;
	k1x_pcie_phy_reg_writel(k1x,  (0x8 << 2), val);
#endif
#else
	//REG32(PCIE_PUPHY_REG_BASE + (0x8 << 2)) |= 0x3 << 29;
	val = k1x_pcie_phy_reg_readl(k1x,  (0x8 << 2));
	val |= 0x3 << 29;
	k1x_pcie_phy_reg_writel(k1x,  (0x8 << 2), val);
#endif

	// 7.set lane0/1 rc_cal_reg1[6]=1
	for (i = 0; i < lane; i++)
	{
		val = k1x_pcie_phy_reg_readl(k1x, ((0x8 << 2) + 0x400 * i));
		val &= ~(1 << 22);
		k1x_pcie_phy_reg_writel(k1x, ((0x8 << 2) + 0x400 * i), val);
	}
	for (i = 0; i < lane; i++)
	{
		val = k1x_pcie_phy_reg_readl(k1x, ((0x8 << 2) + 0x400 * i));
		val |= (1 << 22);
		k1x_pcie_phy_reg_writel(k1x, ((0x8 << 2) + 0x400 * i), val);
	}

	// release forc PCIE mpu_u3/pu_rx_lfps
	val = k1x_pcie_phy_reg_readl(k1x, 0x6 * 4);
	val &= 0xFFFD7FFF;
	k1x_pcie_phy_reg_writel(k1x, 0x6 * 4, val);
}

static int init_phy(struct k1x_pcie *k1x)
{
	u32 rd_data, pcie_rcal;
	u32 val = 0;

	printk("Now init Rterm...\n");
	printk("pcie prot id = %d, porta_init_done = %d\n", k1x->port_id, porta_init_done);
	if (k1x->port_id != 0) {
	    if (porta_init_done == 0) {
			porta_rterm(k1x);
			//pcie_rcal = REG32(0xC0B10000 + (0x21 << 2));
			pcie_rcal = k1x_pcie_phy0_reg_readl(k1x,  (0x21 << 2));

			//REG32(PMUA_REG_BASE + 0x3CC) &= ~0x4000003f;
			val = k1x_pcie_conf0_reg_readl(k1x, 0);
			val &= ~0x4000003f;
			k1x_pcie_conf0_reg_writel(k1x, 0, val);
		} else {
			//pcie_rcal = REG32(0xC0B10000 + (0x21 << 2));
			pcie_rcal = k1x_pcie_phy0_reg_readl(k1x,  (0x21 << 2));
		}
	} else {
		do {
			//rd_data = REG32(0xC0B10000 + 0x21 * 4);
			rd_data = k1x_pcie_phy0_reg_readl(k1x,  (0x21 * 4));
		} while (((rd_data >> 10) & 0x1) == 0);
		//pcie_rcal = REG32(0xC0B10000 + (0x21 << 2));
		pcie_rcal = k1x_pcie_phy0_reg_readl(k1x,  (0x21 << 2));
	}

	rterm_force(k1x, pcie_rcal);

	printk("Now int init_puphy...\n");
	val = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
	val &= 0xbfffffff;
	k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, val);

	// set refclk model
	val = k1x_pcie_phy_reg_readl(k1x, (0x17 << 2));
	val |= (0x1 << 10);
	k1x_pcie_phy_reg_writel(k1x, (0x17 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, (0x17 << 2));
	val &= ~(0x3 << 8);
	k1x_pcie_phy_reg_writel(k1x, (0x17 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, 0x400 + (0x17 << 2));
	val |= (0x1 << 10);
	k1x_pcie_phy_reg_writel(k1x, 0x400 + (0x17 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, 0x400 + (0x17 << 2));
	val &= ~(0x3 << 8);
	k1x_pcie_phy_reg_writel(k1x, 0x400+ (0x17 << 2), val);
#ifndef PCIE_REF_CLK_OUTPUT
	// receiver mode
	REG32(PCIE_PUPHY_REG_BASE + (0x17 << 2)) |= 0x2 << 8;
	REG32(PCIE_PUPHY_REG_BASE + 0x400 + (0x17 << 2)) |= 0x2 << 8;
#ifdef PCIE_SEL_24M_REF_CLK
	val = k1x_pcie_phy_reg_readl(k1x, (0x12 << 2));
	val &= 0xffff0fff;
	k1x_pcie_phy_reg_writel(k1x, (0x12 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, (0x12 << 2));
	val |= 0x00002000;
	k1x_pcie_phy_reg_writel(k1x, (0x12 << 2), val);
#endif
#else
	// driver mode
	val = k1x_pcie_phy_reg_readl(k1x, (0x17 << 2));
	val |= 0x1 << 8;
	k1x_pcie_phy_reg_writel(k1x, (0x17 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, 0x400 + (0x17 << 2));
	val |= 0x1 << 8;
	k1x_pcie_phy_reg_writel(k1x, 0x400 + (0x17 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, (0x12 << 2));
	val &= 0xffff0fff;
	k1x_pcie_phy_reg_writel(k1x, (0x12 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, (0x12 << 2));
	val |= 0x00002000;
	k1x_pcie_phy_reg_writel(k1x, (0x12 << 2), val);

	val = k1x_pcie_phy_reg_readl(k1x, (0x13 << 2));
	val |= (0x1 << 4);
	k1x_pcie_phy_reg_writel(k1x, (0x13 << 2), val);

	if (k1x->port_id == 0x0) {
		//REG32(0xC0B10000+(0x14<<2)) |= (0x1<<3);//pll_reg9[3] en_rterm,only enable in receiver mode
		val = k1x_pcie_phy0_reg_readl(k1x,  (0x14 << 2));
		val |= (0x1 << 3);
		k1x_pcie_phy0_reg_writel(k1x,  (0x14 << 2), val);
	}
#endif

	// pll_reg1 of lane0, disable ssc pll_reg4[3:0]=4'h0
	val = k1x_pcie_phy_reg_readl(k1x, (0x12 << 2));
	val &= 0xfff0ffff;
	k1x_pcie_phy_reg_writel(k1x, (0x12 << 2), val);

	// PU_ADDR_CLK_CFG of lane0
	val = k1x_pcie_phy_reg_readl(k1x, (0x02 << 2));
	val = 0x00000B78;
	k1x_pcie_phy_reg_writel(k1x, (0x02 << 2), val);

	 // PU_ADDR_CLK_CFG of lane1
	val = k1x_pcie_phy_reg_readl(k1x, 0x400 + (0x02 << 2));
	val = 0x00000B78;
	k1x_pcie_phy_reg_writel(k1x, 0x400 + (0x02 << 2), val);

	// force rcv done
	val = k1x_pcie_phy_reg_readl(k1x, (0x06 << 2));
	val = 0x00000400;
	k1x_pcie_phy_reg_writel(k1x, (0x06 << 2), val);

	// force rcv done
	val = k1x_pcie_phy_reg_readl(k1x, 0x400 + (0x06 << 2));
	val = 0x00000400;
	k1x_pcie_phy_reg_writel(k1x, 0x400 + (0x06 << 2), val);

	// waiting pll lock
	printk("waiting pll lock...\n");
	do
	{
		rd_data = k1x_pcie_phy_reg_readl(k1x, 0x8);
	} while ((rd_data & 0x1) == 0);

	if (k1x->port_id == 0)
		porta_init_done = 0x1;
	printk("Now finish init_puphy....\n");
	return 0;
}

int is_pcie_init = 1;
static int __init pcie_already_init(char *str)
{
	is_pcie_init = 1;
	return 0;
}
__setup("pcie_init", pcie_already_init);

static int k1x_pcie_link_up(struct dw_pcie *pci)
{
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	u32 reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_LINK_STS);

	return (reg & RDLH_LINK_UP) && (reg & SMLH_LINK_UP);
}

static void k1x_pcie_stop_link(struct dw_pcie *pci)
{
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	u32 reg;

	reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg &= ~LTSSM_EN;
	k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);
}

static int k1x_pcie_establish_link(struct dw_pcie *pci)
{
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	struct device *dev = pci->dev;
	u32 reg;

	if (k1x->mode == DW_PCIE_EP_TYPE) {
		u32 cnt =0;
		do {
			reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
			if((reg & (1<<7)) == (1<<7))
				break;
			udelay(10);
			cnt += 1;
		}while(1);
	}

	if (dw_pcie_link_up(pci)) {
		dev_err(dev, "link is already up\n");
		return 0;
	}

	reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg |= LTSSM_EN;
	reg &= ~APP_HOLD_PHY_RST;
	k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	printk("ltssm enable\n");
	return 0;
}

/*
 * start of a new interrupt
 * we don't need operate the h/w register here,
 * but we must implement a function here, handle_edge_irq will call this func
 *
 */
static void k1x_irq_ack(struct irq_data *data)
{
        return;
}

static void k1x_pci_msi_mask_irq(struct irq_data *data)
{
	struct msi_desc * desc = irq_data_get_msi_desc(data);

	if(desc)
		pci_msi_mask_irq(data);
}

static void k1x_pci_msi_unmask_irq(struct irq_data *data)
{
	struct msi_desc * desc = irq_data_get_msi_desc(data);

	if(desc)
		pci_msi_unmask_irq(data);
}

static struct irq_chip k1x_msi_irq_chip = {
	.name = "PCI-MSI",
	.irq_ack = k1x_irq_ack,
	.irq_enable = k1x_pci_msi_unmask_irq,
	.irq_disable = k1x_pci_msi_mask_irq,
	.irq_mask = k1x_pci_msi_mask_irq,
	.irq_unmask = k1x_pci_msi_unmask_irq,
};

static struct msi_domain_info k1x_pcie_msi_domain_info = {
	.flags	= (MSI_FLAG_USE_DEF_DOM_OPS | MSI_FLAG_USE_DEF_CHIP_OPS |
		   MSI_FLAG_PCI_MSIX | MSI_FLAG_MULTI_PCI_MSI),
	.chip	= &k1x_msi_irq_chip,
};

/* MSI int handler */
irqreturn_t k1x_handle_msi_irq(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	u32 val, addr;
	int i;
	irqreturn_t ret = IRQ_NONE;

	val = k1x_pcie_phy_ahb_readl(k1x, ADDR_MSI_RECV_CTRL);
	if(val & MSIX_AFIFO_FULL)
		pr_err("AXI monitor FIFO FULL.\n");

	for (i = 0; i < FIFO_LEN; i++) {

		addr = k1x_pcie_phy_ahb_readl(k1x, ADDR_MON_FIFO_DATA0);
		if (addr == FIFO_EMPTY)
			break;
		val = k1x_pcie_phy_ahb_readl(k1x, ADDR_MON_FIFO_DATA1);
		/* in fact, val is the hwirq which equals with msi_data + msi vector */
		val &= INT_VEC_MASK;

		ret = IRQ_HANDLED;
		generic_handle_domain_irq(pp->irq_domain, val);
	}

	return ret;
}

static void k1x_pcie_setup_msi_msg(struct irq_data *d, struct msi_msg *msg)
{
	struct dw_pcie_rp *pp = irq_data_get_irq_chip_data(d);
	u64 msi_target;

	msi_target = (u64)pp->msi_data;

	msg->address_lo = lower_32_bits(msi_target);
	msg->address_hi = upper_32_bits(msi_target);

	msg->data = d->hwirq;

	pr_debug("msi#%d address_hi %#x address_lo %#x\n",
		(int)d->hwirq, msg->address_hi, msg->address_lo);
}

static int k1x_pcie_msi_set_affinity(struct irq_data *d,
				   const struct cpumask *mask, bool force)
{
	return -EINVAL;
}

static struct irq_chip k1x_pcie_msi_bottom_irq_chip = {
	.name = "K1X-PCI-MSI",
	.irq_compose_msi_msg = k1x_pcie_setup_msi_msg,
	.irq_set_affinity = k1x_pcie_msi_set_affinity,
};

static int k1x_pcie_irq_domain_alloc(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs,
				    void *args)
{
	struct dw_pcie_rp *pp = domain->host_data;
	unsigned long flags;
	u32 i;
	int bit;

	raw_spin_lock_irqsave(&pp->lock, flags);

	bit = bitmap_find_free_region(pp->msi_irq_in_use, MAX_MSI_IRQS,
				      order_base_2(nr_irqs));

	raw_spin_unlock_irqrestore(&pp->lock, flags);

	if (bit < 0)
		return -ENOSPC;

	for (i = 0; i < nr_irqs; i++)
		irq_domain_set_info(domain, virq + i, bit + i,
				    pp->msi_irq_chip,
				    pp, handle_edge_irq,
				    NULL, NULL);

	return 0;
}

static void k1x_pcie_irq_domain_free(struct irq_domain *domain,
				    unsigned int virq, unsigned int nr_irqs)
{
	struct irq_data *d = irq_domain_get_irq_data(domain, virq);
	struct dw_pcie_rp *pp = domain->host_data;
	unsigned long flags;

	raw_spin_lock_irqsave(&pp->lock, flags);

	bitmap_release_region(pp->msi_irq_in_use, d->hwirq,
			      order_base_2(nr_irqs));

	raw_spin_unlock_irqrestore(&pp->lock, flags);
}

static const struct irq_domain_ops k1x_pcie_msi_domain_ops = {
	.alloc	= k1x_pcie_irq_domain_alloc,
	.free	= k1x_pcie_irq_domain_free,
};

int k1x_pcie_allocate_domains(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pcie = to_dw_pcie_from_pp(pp);
	struct fwnode_handle *fwnode = of_node_to_fwnode(pcie->dev->of_node);

	pp->irq_domain = irq_domain_create_linear(fwnode, MAX_MSI_IRQS,
					       &k1x_pcie_msi_domain_ops, pp);
	if (!pp->irq_domain) {
		dev_err(pcie->dev, "Failed to create IRQ domain\n");
		return -ENOMEM;
	}

	irq_domain_update_bus_token(pp->irq_domain, DOMAIN_BUS_NEXUS);
	pp->msi_domain = pci_msi_create_irq_domain(fwnode,
						   &k1x_pcie_msi_domain_info,
						   pp->irq_domain);
	if (!pp->msi_domain) {
		dev_err(pcie->dev, "Failed to create MSI domain\n");
		irq_domain_remove(pp->irq_domain);
		return -ENOMEM;
	}

	return 0;
}

void k1x_pcie_msix_addr_alloc(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	struct device *dev = pci->dev;
	u64 msi_target;
	u32 reg;

	k1x->msix_page = alloc_page(GFP_KERNEL);
	k1x->msix_addr = dma_map_page(dev, k1x->msix_page, 0, PAGE_SIZE,
				    DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, k1x->msix_addr)) {
		dev_err(dev, "Failed to map MSIX address\n");
		__free_page(k1x->msix_page);
		k1x->msix_page = NULL;
		return;
	}
	msi_target = (u64)k1x->msix_addr;

	pr_info("(u64)pp->msix_addr =%llx\n", (u64)k1x->msix_addr);
	reg = k1x_pcie_phy_ahb_readl(k1x, ADDR_MSI_RECV_CTRL);
	reg |= MSIX_MON_EN;
	k1x_pcie_phy_ahb_writel(k1x, ADDR_MSI_RECV_CTRL, reg);
	reg = k1x_pcie_phy_ahb_readl(k1x, ADDR_MSIX_MON_MASK);
	reg |= 0xA;
	k1x_pcie_phy_ahb_writel(k1x, ADDR_MSIX_MON_MASK, reg);
	k1x_pcie_phy_ahb_writel(k1x, ADDR_MSIX_MON_BASE0, (lower_32_bits(msi_target) >> 2));
}

void k1x_pcie_msi_addr_alloc(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	struct device *dev = pci->dev;
	u64 msi_target;
	u32 reg;

	k1x->msi_page = alloc_page(GFP_KERNEL);
	pp->msi_data = dma_map_page(dev, k1x->msi_page, 0, PAGE_SIZE,
				    DMA_FROM_DEVICE);
	if (dma_mapping_error(dev, pp->msi_data)) {
		dev_err(dev, "Failed to map MSI data\n");
		__free_page(k1x->msi_page);
		k1x->msi_page = NULL;
		return;
	}
	msi_target = (u64)pp->msi_data;

	pr_info("(u64)pp->msi_data =%llx\n", (u64)pp->msi_data);
	/* Program the msi_data */
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_LO, lower_32_bits(msi_target));
	dw_pcie_writel_dbi(pci, PCIE_MSI_ADDR_HI, upper_32_bits(msi_target));

	reg = k1x_pcie_phy_ahb_readl(k1x, ADDR_MSI_RECV_CTRL);
	reg |= MSI_MON_EN;
	k1x_pcie_phy_ahb_writel(k1x, ADDR_MSI_RECV_CTRL, reg);
	k1x_pcie_phy_ahb_writel(k1x, ADDR_MSI_RECV_ADDR0, (lower_32_bits(msi_target) >> 2));
}

static void k1x_pcie_enable_msi_interrupts(struct k1x_pcie *k1x)
{
	u32 reg;

	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQENABLE_SET_MSI);
	reg |= MSI;
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQENABLE_SET_MSI, reg);

	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQENABLE_SET_INTX);
	reg |= LEG_EP_INTERRUPTS;
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQENABLE_SET_INTX, reg);

	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQ_EN);
	reg |= IRQ_EN;
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQ_EN, reg);

	reg = k1x_pcie_phy_ahb_readl(k1x, ADDR_INTR_ENABLE1);
	reg |= (MSI_INT | MSIX_INT);
	k1x_pcie_phy_ahb_writel(k1x, ADDR_INTR_ENABLE1, reg);
}

static void k1x_pcie_enable_wrapper_interrupts(struct k1x_pcie *k1x)
{
	u32 reg;

	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQENABLE_SET_MSI);
	reg |= PCIE_REMOTE_INTERRUPT | DMA_READ_INT;
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQENABLE_SET_MSI, reg);

	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQ_EN);
	reg |= IRQ_EN;
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQ_EN, reg);

	reg = k1x_pcie_readl_elbi(k1x, PCIE_ELBI_EP_DMA_IRQ_MASK);
	reg |= PC_TO_EP_INT_MASK;
	k1x_pcie_writel_elbi(k1x, PCIE_ELBI_EP_DMA_IRQ_MASK, reg);
}

int k1x_pcie_enable_clocks(struct k1x_pcie *k1x)
{
	struct device *dev = k1x->pci->dev;
	int err;

	err = clk_prepare_enable(k1x->clk_master);
	if (err) {
	        dev_err(dev, "unable to enable k1x->clk_master clock\n");
	        return err;
	}

	err = clk_prepare_enable(k1x->clk_slave);
	if (err) {
	        dev_err(dev, "unable to enable k1x->clk_slave clock\n");
	        goto err_clk_master_pcie;
	}

	err = clk_prepare_enable(k1x->clk_slave_lite);
	if (err) {
	        dev_err(dev, "unable to enable k1x->clk_slave_lite clock\n");
	        goto err_clk_slave_pcie;
	}

	return 0;

	err_clk_slave_pcie:
	clk_disable_unprepare(k1x->clk_slave);
	err_clk_master_pcie:
	clk_disable_unprepare(k1x->clk_master);
	return err;
}
EXPORT_SYMBOL_GPL(k1x_pcie_enable_clocks);

void k1x_pcie_disable_clocks(struct k1x_pcie *k1x)
{

	clk_disable_unprepare(k1x->clk_slave_lite);
	clk_disable_unprepare(k1x->clk_slave);
	clk_disable_unprepare(k1x->clk_master);
}
EXPORT_SYMBOL_GPL(k1x_pcie_disable_clocks);

int k1x_pcie_wait_for_speed_change(struct dw_pcie *pci)
{
	struct device *dev = pci->dev;
	u32 tmp;
	unsigned int retries;

	for (retries = 0; retries < 200; retries++) {
		tmp = dw_pcie_readl_dbi(pci, PCIE_LINK_WIDTH_SPEED_CONTROL);
		/* Test if the speed change finished. */
		if (!(tmp & PORT_LOGIC_SPEED_CHANGE))
			return 0;
		usleep_range(100, 1000);
	}

	dev_err(dev, "Speed change timeout\n");
	return -ETIMEDOUT;
}


static int k1x_pcie_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	u32 reg;

	mdelay(100);
	/* set Perst# gpio high state*/
	reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg &= ~PCIE_RC_PERST;
	k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	/* read the link status register, get the current speed */
	reg = dw_pcie_readw_dbi(pci, EXP_CAP_ID_OFFSET + PCI_EXP_LNKSTA);
	pr_info("Link up, Gen%i\n", reg & PCI_EXP_LNKSTA_CLS);

	k1x_pcie_enable_msi_interrupts(k1x);

	return 0;
}

static int k1x_pcie_intx_map(struct irq_domain *domain, unsigned int irq,
				irq_hw_number_t hwirq)
{
	irq_set_chip_and_handler(irq, &dummy_irq_chip, handle_simple_irq);
	irq_set_chip_data(irq, domain->host_data);

	return 0;
}

static const struct irq_domain_ops intx_domain_ops = {
	.map = k1x_pcie_intx_map,
	.xlate = pci_irqd_intx_xlate,
};

static int k1x_pcie_init_irq_domain(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	struct device_node *node = dev->of_node;
	struct device_node *pcie_intc_node =  of_get_next_child(node, NULL);

	if (!pcie_intc_node) {
		dev_err(dev, "No PCIe Intc node found\n");
		return -ENODEV;
	}

	k1x->irq_domain = irq_domain_add_linear(pcie_intc_node, PCI_NUM_INTX,
						   &intx_domain_ops, pp);
	if (!k1x->irq_domain) {
		dev_err(dev, "Failed to get a INTx IRQ domain\n");
		return -ENODEV;
	}

	return 0;
}

static void k1x_pcie_msi_irq_handler(struct irq_desc *desc)
{
	struct irq_chip *chip = irq_desc_get_chip(desc);
	struct k1x_pcie *k1x;
	struct dw_pcie *pci;
	struct dw_pcie_rp *pp;
	u32 reg;
	u32 hwirq;
	u32 virq;

	chained_irq_enter(chip, desc);

	pp = irq_desc_get_handler_data(desc);
	pci = to_dw_pcie_from_pp(pp);
	k1x = to_k1x_pcie(pci);

	reg = k1x_pcie_phy_ahb_readl(k1x, ADDR_INTR_STATUS1);
	k1x_pcie_phy_ahb_writel(k1x, ADDR_INTR_STATUS1, reg);
	if ((reg & MSI_INT) | (reg & MSIX_INT)) {
			k1x_handle_msi_irq(pp);
	}

	/* legacy intx*/
	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQSTATUS_INTX);
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQSTATUS_INTX, reg);
	reg = (reg & INTX_MASK) >> INTX_SHIFT;
	if(reg)
		pr_debug("legacy INTx interrupt received\n");

	while(reg) {
		hwirq = ffs(reg) - 1;
		reg &= ~BIT(hwirq);
		virq = irq_find_mapping(k1x->irq_domain, hwirq);
		if(virq)
			generic_handle_irq(virq);
		else
			pr_err("unexpected IRQ,INT%d\n", hwirq);
	}

	chained_irq_exit(chip, desc);
}

int k1x_pcie_msi_host_init(struct dw_pcie_rp *pp)
{
	struct dw_pcie *pci = to_dw_pcie_from_pp(pp);
	struct device *dev = pci->dev;
	int ret;

	if (!pci_msi_enabled()) 
		return -EINVAL;

	pp->msi_irq_chip = &k1x_pcie_msi_bottom_irq_chip;

	ret = k1x_pcie_allocate_domains(pp);
	if (ret) {
		dev_err(dev, "irq domain init failed\n");
		return ret;
	}

	irq_set_chained_handler_and_data(pp->irq, k1x_pcie_msi_irq_handler, pp);
	k1x_pcie_msi_addr_alloc(pp);
	k1x_pcie_msix_addr_alloc(pp);

	return ret;
}

static const struct dw_pcie_host_ops k1x_pcie_host_ops = {
	.init = k1x_pcie_host_init,
	.msi_init = k1x_pcie_msi_host_init,
};

static void (*k1x_pcie_irq_callback)(int);

void k1x_pcie_set_irq_callback(void (*fn)(int))
{
	k1x_pcie_irq_callback = fn;
}

/* local cpu interrupt, vendor specific*/
static irqreturn_t k1x_pcie_irq_handler(int irq, void *arg)
{
	struct k1x_pcie *k1x = arg;
	int num;
	u32 reg, reg_ahb;
	__maybe_unused u8 chan;
#if 0
	DMA_DIRC dirc = DMA_READ;
	DMA_INT_TYPE type;
	BOOLEAN hasErr;
	DMA_ERR errType;
#endif

	reg = k1x_pcie_readl_elbi(k1x, PCIE_ELBI_EP_DMA_IRQ_STATUS);
	/* write 0 to clear the irq*/
	k1x_pcie_writel_elbi(k1x, PCIE_ELBI_EP_DMA_IRQ_STATUS, 0);
	reg_ahb = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQSTATUS_MSI);

repeat:
	if (reg & PC_TO_EP_INT) {
		pr_debug( "%s: irq = %d, reg=%x\n", __func__, irq, reg);
		num = (reg & PC_TO_EP_INT);
		if (k1x_pcie_irq_callback)
			k1x_pcie_irq_callback(num);
	}
	if(reg_ahb & DMA_READ_INT) {
		pr_debug( "dma read done irq  reg=%x\n", reg);
#if 0
		/* chan: read channel 0/1/2/3 */
		while (DmaQueryReadIntSrc(pci, &chan, &type)) {
			BOOLEAN over = false;

			/* Abort and Done interrupts are handled differently. */
			if (type == DMA_INT_ABORT) {
				/* Read error status registers to see what's the source of error. */
				hasErr = DmaQueryErrSrc(pci, dirc, chan,
						&errType);
				DmaClrInt(pci, dirc, chan, DMA_INT_ABORT);
				DmaClrInt(pci, dirc, chan, DMA_INT_DONE);

				if (hasErr) {
					printk("Xfer on %s channel %d encounterred hardware error: %d",
							dirc == DMA_WRITE ? "write" : "read", chan, errType);

					if (errType == DMA_ERR_WR || errType == DMA_ERR_RD){
						/* Fatal errors. Can't recover. */
						over = true;
						printk("pcie DMA fatel error occurred...\n");
					} else {
						/* Try to request the DMA to continue processing. */
						printk("Resuming DMA transfer from non-fatal error...");
					}
				} else {
					printk("Xfer on %s channel %d aborted but no HW error found!",
							dirc == DMA_WRITE ? "write" : "read", chan);
					over = true;
				}
			} else {
				pr_debug("dirc=%d chan=%d \n", dirc, chan);
				DmaClrInt(pci, dirc, chan, DMA_INT_DONE);
				over = true;
			}

			/* Xfer is aborted/done, invoking callback if desired */
			if (over && k1x_pcie_irq_callback)
				k1x_pcie_irq_callback(chan);
		}
#endif
	}

	reg = k1x_pcie_readl_elbi(k1x, PCIE_ELBI_EP_DMA_IRQ_STATUS);
	if (reg & PC_TO_EP_INT) {
		k1x_pcie_writel_elbi(k1x, PCIE_ELBI_EP_DMA_IRQ_STATUS, 0);
		goto repeat;
	}

	return IRQ_HANDLED;
}

static void k1x_pcie_ep_init(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	enum pci_barno bar;

	if (0) {
		for (bar = BAR_0; bar <= BAR_5; bar++)
			dw_pcie_ep_reset_bar(pci, bar);
	}

	k1x_pcie_enable_wrapper_interrupts(k1x);
}

__maybe_unused static void k1x_pcie_ep_enable_irq(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);

	k1x_pcie_enable_wrapper_interrupts(k1x);
}

__maybe_unused static void k1x_pcie_ep_disable_irq(struct dw_pcie_ep *ep)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	struct k1x_pcie *k1x = to_k1x_pcie(pci);
	u32 reg;

	reg = k1x_pcie_phy_ahb_readl(k1x, K1X_PHY_AHB_IRQ_EN);
	reg &= ~IRQ_EN;
	k1x_pcie_phy_ahb_writel(k1x, K1X_PHY_AHB_IRQ_EN, reg);

}

static int k1x_pcie_raise_irq(struct dw_pcie_ep *ep, u8 func_no,
			      unsigned int type, u16 interrupt_num)
{
	struct dw_pcie *pci = to_dw_pcie_from_ep(ep);
	//struct k1x_pcie *k1x = to_k1x_pcie(pci);

	switch (type) {
	case PCI_IRQ_INTX:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
		//k1x_pcie_raise_legacy_irq(k1x);
		return -EINVAL;
	case PCI_IRQ_MSI:
		dw_pcie_ep_raise_msi_irq(ep, func_no, interrupt_num);
		break;
	case PCI_IRQ_MSIX:
		dw_pcie_ep_raise_msix_irq(ep, func_no, interrupt_num);
		break;
	default:
		dev_err(pci->dev, "UNKNOWN IRQ type\n");
	}

	return 0;
}

static const struct pci_epc_features k1x_pcie_epc_features = {
	.linkup_notifier = false,
	.msi_capable = true,
	.msix_capable = true,
};

static const struct pci_epc_features*
k1x_pcie_get_features(struct dw_pcie_ep *ep)
{
	return &k1x_pcie_epc_features;
}

static struct dw_pcie_ep_ops pcie_ep_ops = {
	.init = k1x_pcie_ep_init,
	.raise_irq = k1x_pcie_raise_irq,
	//.enable_irq = k1x_pcie_ep_enable_irq,
	//.disable_irq = k1x_pcie_ep_disable_irq,
	.get_features = k1x_pcie_get_features,
};

static int __init k1x_add_pcie_ep(struct k1x_pcie *k1x,
				     struct platform_device *pdev)
{
	int ret;
	struct dw_pcie_ep *ep;
	struct resource *res;
	struct device *dev = &pdev->dev;
	struct dw_pcie *pci = k1x->pci;

	ep = &pci->ep;
	ep->ops = &pcie_ep_ops;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "elbi");
	k1x->elbi_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!k1x->elbi_base)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dma");
	k1x->dma_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!k1x->dma_base)
		return -ENOMEM;

	ret = dw_pcie_ep_init(ep);
	if (ret) {
		dev_err(dev, "failed to initialize endpoint\n");
		return ret;
	}

	return 0;
}

static int __init k1x_add_pcie_port(struct k1x_pcie *k1x,
				       struct platform_device *pdev)
{
	int ret;
	struct dw_pcie *pci = k1x->pci;
	struct dw_pcie_rp *pp = &pci->pp;
	struct device *dev = pci->dev;
	struct resource *res;
	u32 reg;

	/* set Perst# (fundamental reset) gpio low state*/
	reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
	reg |= PCIE_RC_PERST;
	k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

	pp->irq = platform_get_irq(pdev, 0);
	if (pp->irq < 0) {
		dev_err(dev, "missing IRQ resource\n");
		return pp->irq;
	}

	ret = k1x_pcie_init_irq_domain(pp);
	if (ret < 0)
		return ret;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "dbi");
	pci->dbi_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pci->dbi_base)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "atu");
	pci->atu_base = devm_ioremap(dev, res->start, resource_size(res));
	if (!pci->atu_base)
		return -ENOMEM;

	pp->ops = &k1x_pcie_host_ops;

	pp->num_vectors = MAX_MSI_IRQS;
	ret = dw_pcie_host_init(pp);
	if (ret) {
		dev_err(dev, "failed to initialize host\n");
		return ret;
	}

	return 0;
}

static const struct dw_pcie_ops dw_pcie_ops = {
	.start_link = k1x_pcie_establish_link,
	.stop_link = k1x_pcie_stop_link,
	.link_up = k1x_pcie_link_up,
};

#ifdef CONFIG_PM_SLEEP
static void k1x_pcie_disable_phy(struct k1x_pcie *k1x)
{
	int phy_count = k1x->phy_count;

	while (phy_count--) {
		phy_power_off(k1x->phy[phy_count]);
		phy_exit(k1x->phy[phy_count]);
	}
}

static int k1x_pcie_enable_phy(struct k1x_pcie *k1x)
{
	int phy_count = k1x->phy_count;
	int ret;
	int i;

	for (i = 0; i < phy_count; i++) {
		ret = phy_init(k1x->phy[i]);
		if (ret < 0)
			goto err_phy;

		ret = phy_power_on(k1x->phy[i]);
		if (ret < 0) {
			phy_exit(k1x->phy[i]);
			goto err_phy;
		}
	}

	return 0;

err_phy:
	while (--i >= 0) {
		phy_power_off(k1x->phy[i]);
		phy_exit(k1x->phy[i]);
	}

	return ret;
}
#endif

static const struct k1x_pcie_of_data k1x_pcie_rc_of_data = {
	.mode = DW_PCIE_RC_TYPE,
};

static const struct k1x_pcie_of_data k1x_pcie_ep_of_data = {
	.mode = DW_PCIE_EP_TYPE,
};

static const struct of_device_id of_k1x_pcie_match[] = {
	{
		.compatible = "k1x,dwc-pcie",
		.data = &k1x_pcie_rc_of_data,
	},
	{
		.compatible = "k1x,dwc-pcie-ep",
		.data = &k1x_pcie_ep_of_data,
	},
	{},
};

static int __init k1x_pcie_probe(struct platform_device *pdev)
{
	u32 reg;
	int ret;
	int irq;
	void __iomem *base;
	struct resource *res;
	struct dw_pcie *pci;
	struct k1x_pcie *k1x;
	struct device *dev = &pdev->dev;
	struct device_node *np = dev->of_node;

	const struct of_device_id *match;
	const struct k1x_pcie_of_data *data;
	enum dw_pcie_device_mode mode;

	match = of_match_device(of_match_ptr(of_k1x_pcie_match), dev);
	if (!match)
		return -EINVAL;

	data = (struct k1x_pcie_of_data *)match->data;
	mode = (enum dw_pcie_device_mode)data->mode;

	k1x = devm_kzalloc(dev, sizeof(*k1x), GFP_KERNEL);
	if (!k1x)
		return -ENOMEM;

	pci = devm_kzalloc(dev, sizeof(*pci), GFP_KERNEL);
	if (!pci)
		return -ENOMEM;

	pci->dev = dev;
	pci->ops = &dw_pcie_ops;

	irq = platform_get_irq(pdev, 1);
	if (irq < 0) {
		dev_err(dev, "missing IRQ resource: %d\n", irq);
		return irq;
	}
	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "k1x_conf");
	base = devm_ioremap(dev, res->start, resource_size(res));
	if (!base)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy_ahb");
	k1x->phy_ahb = devm_ioremap(dev, res->start, resource_size(res));
	if (!k1x->phy_ahb)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy_addr");
	k1x->phy_addr = devm_ioremap(dev, res->start, resource_size(res));
	if (!k1x->phy_addr)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "conf0_addr");
	k1x->conf0_addr = devm_ioremap(dev, res->start, resource_size(res));
	if (!k1x->conf0_addr)
		return -ENOMEM;

	res = platform_get_resource_byname(pdev, IORESOURCE_MEM, "phy0_addr");
	k1x->phy0_addr = devm_ioremap(dev, res->start, resource_size(res));
	if (!k1x->phy0_addr)
		return -ENOMEM;

	if (of_property_read_u32(np, "k1x,pcie-port", &k1x->port_id)) {
		dev_err(dev, "Failed to get pcie's port id\n");
		return -EINVAL;
	}

	if (of_property_read_u32(np, "num-lanes", &k1x->num_lanes)) {
		dev_warn(dev, "Failed to get pcie's port num-lanes.\n");
		k1x->num_lanes = 1;
	}
	if((k1x->num_lanes < 1) || (k1x->num_lanes > 2)) {
		dev_warn(dev, "configuration of num-lanes is invalid.\n");
		k1x->num_lanes = 1;
	}

	/* pcie0 and usb use combo phy and reset */
	if (k1x->port_id == 0) {
		k1x->reset = devm_reset_control_array_get_shared(dev);
	} else {
		k1x->reset = devm_reset_control_get_optional(dev, NULL);
	}
	if (IS_ERR(k1x->reset)) {
		dev_err(dev, "Failed to get pcie%d's resets\n", k1x->port_id);
		return PTR_ERR(k1x->reset);
	}

	k1x->base = base;
	k1x->pci = pci;
	platform_set_drvdata(pdev, k1x);

	pm_runtime_enable(&pdev->dev);
	pm_runtime_get_sync(&pdev->dev);
	pm_runtime_get_noresume(&pdev->dev);

	reset_control_deassert(k1x->reset);

	init_phy(k1x);

	k1x->pcie_init_before_kernel = is_pcie_init;
	if (is_pcie_init == 0) {
		reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~LTSSM_EN;
		k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);
	}
	k1x->link_gen = of_pci_get_max_link_speed(np);
	if (k1x->link_gen < 0 || k1x->link_gen > 3)
		k1x->link_gen = 3;

	k1x->mode = mode;
	switch (mode) {
	case DW_PCIE_RC_TYPE:
		if(!IS_ENABLED(CONFIG_PCI_K1X_HOST))
			return -ENODEV;

		reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg |= DEVICE_TYPE_RC;
		k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		reg = k1x_pcie_readl(k1x, PCIE_CTRL_LOGIC);
		reg |= PCIE_IGNORE_PERSTN;
		k1x_pcie_writel(k1x, PCIE_CTRL_LOGIC, reg);

		ret = k1x_add_pcie_port(k1x, pdev);
		if (ret < 0)
			goto err_clk;
		break;
	case DW_PCIE_EP_TYPE:
		if(!IS_ENABLED(CONFIG_PCI_K1X_EP))
			return -ENODEV;

		reg = k1x_pcie_readl(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD);
		reg &= ~DEVICE_TYPE_RC;
		k1x_pcie_writel(k1x, PCIECTRL_K1X_CONF_DEVICE_CMD, reg);

		ret = k1x_add_pcie_ep(k1x, pdev);
		if (ret < 0)
			goto err_clk;
		break;
	default:
		dev_err(dev, "INVALID device type %d\n", mode);
	}

	ret = devm_request_irq(dev, irq, k1x_pcie_irq_handler,
			       IRQF_SHARED, "k1x-pcie", k1x);
	if (ret) {
		dev_err(dev, "failed to request k1x-pcie irq\n");
		goto err_clk;
	}

	return 0;

err_clk:
	k1x_pcie_disable_clocks(k1x);
	return ret;
}

#ifdef CONFIG_PM_SLEEP
static int k1x_pcie_suspend(struct device *dev)
{
	struct k1x_pcie *k1x = dev_get_drvdata(dev);
	struct dw_pcie *pci = k1x->pci;
	u32 val;

	if (k1x->mode != DW_PCIE_RC_TYPE)
		return 0;

	/* clear MSE */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);
	val &= ~PCI_COMMAND_MEMORY;
	dw_pcie_writel_dbi(pci, PCI_COMMAND, val);

	return 0;
}

static int k1x_pcie_resume(struct device *dev)
{
	struct k1x_pcie *k1x = dev_get_drvdata(dev);
	struct dw_pcie *pci = k1x->pci;
	u32 val;

	if (k1x->mode != DW_PCIE_RC_TYPE)
		return 0;

	/* set MSE */
	val = dw_pcie_readl_dbi(pci, PCI_COMMAND);
	val |= PCI_COMMAND_MEMORY;
	dw_pcie_writel_dbi(pci, PCI_COMMAND, val);

	return 0;
}

static int k1x_pcie_suspend_noirq(struct device *dev)
{
	struct k1x_pcie *k1x = dev_get_drvdata(dev);

	k1x_pcie_disable_phy(k1x);

	return 0;
}

static int k1x_pcie_resume_noirq(struct device *dev)
{
	struct k1x_pcie *k1x = dev_get_drvdata(dev);
	int ret;

	ret = k1x_pcie_enable_phy(k1x);
	if (ret) {
		dev_err(dev, "failed to enable phy\n");
		return ret;
	}

	return 0;
}
#endif

static const struct dev_pm_ops k1x_pcie_pm_ops = {
	SET_SYSTEM_SLEEP_PM_OPS(k1x_pcie_suspend, k1x_pcie_resume)
	SET_NOIRQ_SYSTEM_SLEEP_PM_OPS(k1x_pcie_suspend_noirq,
				      k1x_pcie_resume_noirq)
};

static struct platform_driver k1x_pcie_driver = {
	.driver = {
		.name	= "k1x-dwc-pcie",
		.of_match_table = of_k1x_pcie_match,
		.suppress_bind_attrs = true,
		.pm	= &k1x_pcie_pm_ops,
	},
};
builtin_platform_driver_probe(k1x_pcie_driver, k1x_pcie_probe);
