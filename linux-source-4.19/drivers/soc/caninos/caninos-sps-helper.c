
#include <linux/delay.h>
#include <linux/io.h>

#define SPS_PG_CTL	0x0

int caninos_sps_set_pg
	(void __iomem *base, u32 pwr_mask, u32 ack_mask, bool enable)
{
	u32 val;
	bool ack;
	int timeout;

	val = readl(base + SPS_PG_CTL);
	ack = val & ack_mask;
	if (ack == enable)
		return 0;

	if (enable)
		val |= pwr_mask;
	else
		val &= ~pwr_mask;

	writel(val, base + SPS_PG_CTL);

	for (timeout = 5000; timeout > 0; timeout -= 50) {
		val = readl(base + SPS_PG_CTL);
		if ((val & ack_mask) == (enable ? ack_mask : 0))
			break;
		udelay(50);
	}
	if (timeout <= 0)
		return -ETIMEDOUT;

	udelay(10);

	return 0;
}
EXPORT_SYMBOL_GPL(caninos_sps_set_pg);
