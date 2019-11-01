#include <linux/scatterlist.h>
#include <linux/list.h>
#include <linux/dma-mapping.h>

#include "core.h"
#include "hcd.h"
#include "ep.h"

void aotg_set_ring_linkaddr(struct aotg_ring *ring, u32 addr);

int aotg_set_trb_as_ring_linkaddr(struct aotg_ring *ring, struct aotg_trb *trb);

u32 ring_trb_virt_to_dma(struct aotg_ring *ring, struct aotg_trb *trb_vaddr);

void clear_ring_irq(struct caninos_hcd *myhcd, unsigned int irq_mask);

void inc_dequeue_safe(struct aotg_ring *ring)
{
	atomic_inc(&ring->num_trbs_free);
	
	if (ring->dequeue_trb == ring->ring_trb) {
		ring->dequeue_trb = ring->first_trb;
	}
	else {
		ring->dequeue_trb++;
	}
}

struct aotg_ring *aotg_alloc_ring
	(struct caninos_hcd *myhcd, struct aotg_hcep *ep,
	 unsigned int num_trbs, gfp_t mem_flags)
{
	dma_addr_t	dma;
	struct device *dev = caninos_to_hcd(myhcd)->self.controller;
	struct aotg_ring *ring;
	
	ring = kmalloc(sizeof(struct aotg_ring), mem_flags);
	
	if (!ring) {
		return NULL;
	}
	
	ring->num_trbs = num_trbs;
	
	if (num_trbs == 0) {
		return ring;
	}
	
	ring->first_trb = (struct aotg_trb *)
		dma_alloc_coherent(dev,	num_trbs * sizeof(struct aotg_trb),
		                   &dma, mem_flags);
	
	memset(ring->first_trb, 0, num_trbs * sizeof(struct aotg_trb));
	
	ring->trb_dma = (u32)dma;
	ring->last_trb = ring->first_trb + num_trbs - 1;
	ring->ring_trb = ring->last_trb;
	
	atomic_set(&ring->num_trbs_free, num_trbs);
	
	ring->enqueue_trb = ring->first_trb;
	ring->dequeue_trb = ring->first_trb;
	
	ring->is_running = 0;
	ring->is_out = ep->is_out ? 1 : 0;
	ring->intr_inited = 0;
	ring->intr_started = 0;
	ring->priv = ep;
	ring->mask = ep->mask;
	ring->type = ep->type;
	ring->enring_cnt = 0;
	ring->dering_cnt = 0;
	ring->ring_stopped = 0;
	
	ring->reg_dmalinkaddr =
		GET_DMALINKADDR_REG(ring->is_out, myhcd->ctrl->base + HCOUT1DMALINKADDR,
		myhcd->ctrl->base + HCIN1DMALINKADDR, ep->index);
		
	ring->reg_curaddr =
		GET_CURADDR_REG(ring->is_out, myhcd->ctrl->base + HCOUT1DMACURADDR,
		myhcd->ctrl->base + HCIN1DMACURADDR, ep->index);
		
	ring->reg_dmactrl =
		GET_DMACTRL_REG(ring->is_out, myhcd->ctrl->base + HCOUT1DMACTRL,
		myhcd->ctrl->base + HCIN1DMACTRL, ep->index);
		
	ring->reg_dmacomplete_cnt =
		GET_DMACOMPLETE_CNT_REG(ring->is_out,
		myhcd->ctrl->base + HCOUT1DMACOMPLETECNT,
		myhcd->ctrl->base + HCIN1DMACOMPLETECNT, ep->index);
	
	return ring;
}

void aotg_free_ring(struct caninos_hcd *myhcd, struct aotg_ring *ring)
{
	struct device *dev = caninos_to_hcd(myhcd)->self.controller;
	
	if (!ring) {
		return;
	}
	
	dma_free_coherent(dev, ring->num_trbs * sizeof(struct aotg_trb),
		ring->first_trb, ring->trb_dma);
	
	kfree(ring);
}

struct aotg_td *aotg_alloc_td(gfp_t mem_flags)
{
	struct aotg_td *td;
	
	td = kmem_cache_alloc(td_cache, GFP_ATOMIC);
	
	if (!td) {
		return NULL;
	}
	
	memset(td, 0, sizeof(struct aotg_td));
	
	td->cross_ring = 0;
	td->err_count = 0;
	td->urb = NULL;
	INIT_LIST_HEAD(&td->queue_list);
	INIT_LIST_HEAD(&td->enring_list);
	INIT_LIST_HEAD(&td->dering_list);
	
	return td;
}

void aotg_release_td(struct aotg_td *td)
{
	if (!td) {
		return;
	}
	kmem_cache_free(td_cache, td);
}

void enable_overflow_irq(struct caninos_hcd *myhcd, struct aotg_hcep *ep)
{
	u8 mask = ep->mask;
	u8 is_out = mask & USB_HCD_OUT_MASK;
	u8 ep_num = mask & 0x0f;
	
	if (is_out) {
		usb_setbitsl(1 << (ep_num + 16), myhcd->ctrl->base + HCDMAxOVERFLOWIEN);
	}
	else {
		usb_setbitsl(1 << ep_num, myhcd->ctrl->base + HCDMAxOVERFLOWIEN);
	}
}

void disable_overflow_irq(struct caninos_hcd *myhcd, struct aotg_hcep *ep)
{
	u8 mask = ep->mask;
	u8 is_out = mask & USB_HCD_OUT_MASK;
	u8 ep_num = mask & 0x0f;
	
	if (is_out) {
		usb_clearbitsl(1<<(ep_num + 16), myhcd->ctrl->base + HCDMAxOVERFLOWIEN);
	}
	else {
		usb_clearbitsl(1 << ep_num, myhcd->ctrl->base + HCDMAxOVERFLOWIEN);
	}
}

void clear_overflow_irq(struct caninos_hcd *myhcd, struct aotg_hcep *ep)
{
	u8 mask = ep->mask;
	u8 is_out = mask & USB_HCD_OUT_MASK;
	u8 ep_num = mask & 0x0f;

	if (is_out) {
		usb_clearbitsl(1<<(ep_num + 16), myhcd->ctrl->base + HCDMAxOVERFLOWIRQ);
	}
	else {
		usb_clearbitsl(1 << ep_num, myhcd->ctrl->base + HCDMAxOVERFLOWIRQ);
	}
}

void overflow_irq_handler(struct caninos_hcd *myhcd, struct aotg_hcep *ep)
{
	struct aotg_ring *ring;
	
	if (!ep) {
		return;
	}
	ring = ep->ring;
}

int is_ring_running(struct aotg_ring *ring)
{
	return (readl(ring->reg_dmactrl) & 0x1) ? 1 : 0;
}

void aotg_start_ring(struct aotg_ring *ring, u32 addr)
{
	struct aotg_trb *temp_trb = ring->dequeue_trb;
	int i;
	
	if ((ring->type == PIPE_BULK) && ((temp_trb->hw_token & TRB_OF) == 0))
	{
		for (i = 0; i < NUM_TRBS; i++)
		{
			if (temp_trb->hw_token == 0xaa) { /*deal dequeue urb*/
				inc_dequeue_safe(ring);
				memset(temp_trb, 0, sizeof(struct aotg_trb));
			}
			else if (temp_trb->hw_token & TRB_OF) {
				break;
			}
			if (temp_trb == ring->last_trb) {
				temp_trb = ring->first_trb;
			}
			else {
				temp_trb++;
			}
		}
		addr = ring_trb_virt_to_dma(ring, temp_trb);
	}
	aotg_set_ring_linkaddr(ring, addr);
	mb();
	writel(DMACTRL_DMACS, ring->reg_dmactrl);
}

void aotg_stop_ring(struct aotg_ring *ring)
{
	writel(DMACTRL_DMACC, ring->reg_dmactrl);
}

void aotg_pause_ring(struct aotg_ring *ring)
{
	usb_setbitsl(DMACTRL_DMACC, (void __iomem *)ring->reg_dmactrl);
}

u32 ring_trb_virt_to_dma(struct aotg_ring *ring,
	struct aotg_trb *trb_vaddr)
{
	u32 addr;
	unsigned long offset;

	if (!ring || !trb_vaddr)
		return 0;

	if (trb_vaddr > ring->last_trb)
		return 0;

	offset = trb_vaddr - ring->first_trb;
	addr = ring->trb_dma + (offset * sizeof(*trb_vaddr));
	return addr;
}

void aotg_set_ring_linkaddr(struct aotg_ring *ring, u32 addr)
{
	if (!ring) {
		
		return;
	}
	writel(addr, ring->reg_dmalinkaddr);
}

int aotg_set_trb_as_ring_linkaddr(struct aotg_ring *ring, struct aotg_trb *trb)
{
	u32 addr;

	addr = (u32)ring_trb_virt_to_dma(ring, trb);
	if (!addr) {
		
		return -1;
	}

	aotg_set_ring_linkaddr(ring, addr);
	return 0;
}

int ring_empty(struct aotg_ring *ring)
{
	return (atomic_read(&ring->num_trbs_free) == NUM_TRBS) ? 1 : 0;
}

int ring_full(struct aotg_ring *ring)
{
	return (atomic_read(&ring->num_trbs_free) == 0) ? 1 : 0;
}

inline int is_room_on_ring(struct aotg_ring *ring, unsigned int num_trbs)
{
	return (num_trbs > atomic_read(&ring->num_trbs_free)) ? 0 : 1;
}

inline unsigned int	count_urb_need_trbs(struct urb *urb)
{
	int num_trbs;

	num_trbs = 1;
	if (usb_pipeout(urb->pipe) && (urb->transfer_flags & URB_ZERO_PACKET))
		num_trbs++;

	return num_trbs;
}

/**
 * scatter-gather transfer trbs count.
 * according to num_sgs and urb->transfer_buffer_length.
 */
inline unsigned int	count_sg_urb_need_trbs(struct urb *urb)
{
	int num_sgs, num_trbs, temp, i;
	struct scatterlist *sg;

	sg = NULL;
	num_sgs = urb->num_mapped_sgs;
	temp = urb->transfer_buffer_length;

	num_trbs = 0;
	for_each_sg(urb->sg, sg, num_sgs, i) {
		unsigned int len = sg_dma_len(sg);

		if (len != 0)
			num_trbs++;

		len = min_t(int, len, temp);
		temp -= len;
		if (temp == 0)
			break;
	}

	if (usb_pipeout(urb->pipe) && (urb->transfer_flags & URB_ZERO_PACKET))
		num_trbs++;

	return num_trbs;
}

void aotg_fill_trb(struct aotg_trb *trb, u32 dma_addr, u32 len, u32 token)
{
	trb->hw_buf_ptr = dma_addr;
	trb->hw_buf_len = len;
	trb->hw_token = token;
	return;
}

int aotg_sg_map_trb(struct aotg_trb *trb,
	struct scatterlist *sg, int len, u32 token)
{
	int this_trb_len;

	if (NULL == sg) {
		aotg_fill_trb(trb, 0, 0, token);
		return 0;
	}
	this_trb_len = min_t(int, sg_dma_len(sg), len);
	aotg_fill_trb(trb, (u32)sg_dma_address(sg), this_trb_len, token);
	return this_trb_len;
}

void enqueue_trb(struct aotg_ring *ring, u32 buf_ptr, u32 buf_len,
	u32 token)
{
	struct aotg_trb *trb;
	trb = ring->enqueue_trb;

	atomic_dec(&ring->num_trbs_free);
	if (trb == ring->last_trb) {
		if (ring->type == PIPE_BULK) {
			token &= ~TRB_CHN;
			if (ring->is_out)
				token |= TRB_ITE | TRB_LT;
			else
				token |= TRB_ICE | TRB_LT;
			ring->enqueue_trb = ring->first_trb;
		} else {
			token |= TRB_COF;
			ring->enqueue_trb = ring->first_trb;
		}
	} else {
		ring->enqueue_trb += 1;
	}

	trb->hw_buf_ptr = buf_ptr;
	trb->hw_buf_len = buf_len;
	trb->hw_buf_remain = 0;
	wmb();
	trb->hw_token = token;
}

/*
 * ensure ring has enough room for this td
 * before call this function.
 */
int ring_enqueue_sg_td(struct caninos_hcd *myhcd,
	struct aotg_ring *ring, struct aotg_td *td)
{
	u8 is_out;
	int num_sgs, num_trbs;
	int len, this_trb_len;
	u32 addr, token;
	struct urb *urb = td->urb;
	struct scatterlist *sg;

	is_out = usb_pipeout(urb->pipe);
	len = urb->transfer_buffer_length;
	num_sgs = urb->num_mapped_sgs;
	num_trbs = count_sg_urb_need_trbs(urb);
	if (!is_room_on_ring(ring, num_trbs))
		return -1;

	td->num_trbs = num_trbs;
	td->trb_vaddr = ring->enqueue_trb;
	td->trb_dma = ring_trb_virt_to_dma(ring, ring->enqueue_trb);

	if (td->trb_vaddr + (num_trbs - 1) > ring->last_trb)
		td->cross_ring = 1;

	sg = urb->sg;
	addr = (u32)sg_dma_address(sg);
	this_trb_len = (u32)min_t(int, sg_dma_len(sg), len);

	if (is_out)
		token = TRB_OF;
	else
		token = TRB_CSP | TRB_OF;

	do {
		if (num_trbs == 1) {
			token &= ~TRB_CHN;
			if (is_out)
				token |= TRB_ITE;
			else
				token |= TRB_ICE;

			if (is_out && (urb->transfer_flags & URB_ZERO_PACKET))
				enqueue_trb(ring, 0, 0, token);
			else
				enqueue_trb(ring, addr, this_trb_len, token);
			break;
		}
		token |= TRB_CHN;
		enqueue_trb(ring, addr, this_trb_len, token);
		len -= this_trb_len;
		num_trbs--;
		num_sgs--;

		if (num_sgs) {
			sg = sg_next(sg);
			addr = (u32)sg_dma_address(sg);
			this_trb_len = (u32)min_t(int, sg_dma_len(sg), len);
		}
	} while (num_trbs);

	return 0;
}

int aotg_ring_enqueue_td(struct caninos_hcd *myhcd,
	struct aotg_ring *ring, struct aotg_td *td)
{
	u8 is_out;
	u32 addr, token, this_trb_len;
	int num_trbs;
	struct urb *urb = td->urb;

	if (!myhcd || !td || !ring || !urb) {
		
		return -1;
	}

	if (urb->num_sgs)
		return ring_enqueue_sg_td(myhcd, ring, td);

	num_trbs = count_urb_need_trbs(urb);
	if (!is_room_on_ring(ring, num_trbs))
		return -1;

	is_out = usb_pipeout(urb->pipe);

	td->num_trbs = num_trbs;
	td->trb_vaddr = ring->enqueue_trb;
	td->trb_dma = ring_trb_virt_to_dma(ring, ring->enqueue_trb);

	addr = (u32)urb->transfer_dma;
	this_trb_len = (u32)urb->transfer_buffer_length;

	if (is_out)
		token = TRB_OF;
	else
		token = TRB_CSP | TRB_OF;

	/*
	 * Finish bulk OUT with short packet
	 */
	if (num_trbs > 1) {
		token |= TRB_CHN;
		enqueue_trb(ring, addr, this_trb_len, token);
		addr = 0;
		this_trb_len = 0;
	}

	token &= ~TRB_CHN;

		token |= TRB_LT; /*8723bu,release cpu for interrupt transfer*/
		
	if (is_out)
		token |= TRB_ITE;
	else
		token |= TRB_ICE;

	enqueue_trb(ring, addr, this_trb_len, token);
	return 0;
}

void aotg_reorder_intr_td(struct aotg_hcep *ep)
{
	struct aotg_td *td, *next, *entry_td = NULL;
	struct urb *urb;
	struct aotg_ring *ring;
	unsigned long td_temp = ULONG_MAX;
	ring = ep->ring;

	ring->dequeue_trb = ring->first_trb;
	ring->enqueue_trb = ring->first_trb;
	list_for_each_entry(td, &ep->enring_td_list, enring_list) {
		if (td_temp > (unsigned long)(td->intr_mem_vaddr)) {
			td_temp = (unsigned long)(td->intr_mem_vaddr);
			entry_td = td;
		}
	}

	list_for_each_entry_safe(td, next, &ep->enring_td_list, enring_list) {
		if ((unsigned long)(td->intr_mem_vaddr) > td_temp) {
			if (td->urb) {
				urb = td->urb;
				td->urb = NULL;
				entry_td->urb = urb;
				entry_td = list_entry(entry_td->enring_list.next, struct aotg_td, enring_list);
			}
			list_del(&td->enring_list);
			list_add_tail(&td->enring_list, &ep->enring_td_list);
		} else {
			break;
		}
	}
}

void aotg_reorder_iso_td(struct caninos_hcd *myhcd, struct aotg_ring *ring)
{
	struct aotg_td *td, *next;
	struct aotg_trb *new_trb_q, *prev_trb_q, *trb;
	int i;
	dma_addr_t dma, prev_dma;
	struct device *dev = caninos_to_hcd(myhcd)->self.controller;
	struct aotg_hcep *ep = (struct aotg_hcep *)ring->priv;

	new_trb_q = (struct aotg_trb *)
		dma_alloc_coherent(dev,	NUM_TRBS * sizeof(struct aotg_trb),
		&dma, GFP_ATOMIC);
	if (!new_trb_q) {
		pr_err("dma_alloc_coherent trb error!!!\n");
		return;
	}
	memset(new_trb_q, 0, NUM_TRBS * sizeof(struct aotg_trb));
	prev_trb_q = ring->first_trb;
	ring->first_trb = new_trb_q;
	prev_dma = ring->trb_dma;
	ring->trb_dma = (u32)dma;
	ring->last_trb = ring->first_trb + NUM_TRBS - 1;
	ring->ring_trb = ring->last_trb;
	atomic_set(&ring->num_trbs_free, NUM_TRBS);
	ring->enqueue_trb = ring->first_trb;
	ring->dequeue_trb = ring->first_trb;

	list_for_each_entry_safe(td, next, &ep->enring_td_list, enring_list) {
		if (td) {
			trb = td->trb_vaddr;
			td->trb_vaddr = ring->enqueue_trb;
			td->trb_dma = ring_trb_virt_to_dma(ring, ring->enqueue_trb);
			for (i = 0; i < td->num_trbs; i++) {
				enqueue_trb(ring, trb->hw_buf_ptr, trb->hw_buf_len, trb->hw_token & (~TRB_COF));
			}
		}
	}
	dma_free_coherent(dev, NUM_TRBS * sizeof(struct aotg_trb), prev_trb_q, prev_dma);
}

int aotg_ring_enqueue_intr_td(struct caninos_hcd *myhcd, struct aotg_ring *ring,
	struct aotg_hcep *ep, struct urb *urb, gfp_t mem_flags)
{
	u8 is_out;
	u32 addr, token, this_trb_len;
	int i;
	int mem_size;
	dma_addr_t	dma;
	struct aotg_td *td, *next;
	struct device *dev = caninos_to_hcd(myhcd)->self.controller;

	if (!is_room_on_ring(ring, INTR_TRBS)) {
		pr_warn("%s err, check it!\n", __func__);
		return -1;
	}

	is_out = usb_pipeout(urb->pipe);
	mem_size = urb->transfer_buffer_length;
	ring->intr_mem_size = mem_size;
	ring->intr_dma_buf_vaddr = (u8 *)dma_alloc_coherent(dev, mem_size * INTR_TRBS,
		&dma, mem_flags);
	if (!ring->intr_dma_buf_vaddr) {
		pr_err("%s err, alloc dma buf for intr fail!\n", __func__);
		return -1;
	}
	ring->intr_dma_buf_phyaddr = dma;

	for (i = 0; i < INTR_TRBS; i++) {
		td = aotg_alloc_td(mem_flags);
		if (!td) {
			pr_err("%s err, alloc td fail\n", __func__);
			goto fail;
		}
		td->intr_mem_vaddr = ring->intr_dma_buf_vaddr + mem_size * i;
		td->intr_men_phyaddr = ring->intr_dma_buf_phyaddr + mem_size * i;
		memset(td->intr_mem_vaddr, 0, mem_size);

		td->num_trbs = 1;
		td->trb_vaddr = ring->enqueue_trb;
		td->trb_dma = ring_trb_virt_to_dma(ring, ring->enqueue_trb);

		this_trb_len = mem_size;
		if (is_out) {
			token = TRB_OF | TRB_ITE | TRB_LT;
			enqueue_trb(ring, urb->transfer_dma, this_trb_len, token);
		}	else {
			token = TRB_OF | TRB_ICE | TRB_CSP;
			addr = (u32)td->intr_men_phyaddr;
			enqueue_trb(ring, addr, this_trb_len, token);
		}

		list_add_tail(&td->enring_list, &ep->enring_td_list);
		ring->enring_cnt++;
	}

	ring->intr_inited = 1;
	return 0;

fail:
	list_for_each_entry_safe(td, next, &ep->enring_td_list, enring_list) {
		aotg_release_td(td);
	}

	dma_free_coherent(dev, ring->intr_mem_size * INTR_TRBS,
		ring->intr_dma_buf_vaddr, ring->intr_dma_buf_phyaddr);

	return -1;
}

int aotg_intr_get_finish_trb(struct aotg_ring *ring)
{
	int i, count = 0;
	struct aotg_trb *trb = ring->first_trb;

	for (i = 0; i < INTR_TRBS; i++) {
		if ((trb->hw_token&TRB_OF) == 0)
			count++;
		trb++;
	}
	return count;
}

int aotg_intr_chg_buf_len(struct caninos_hcd *myhcd, struct aotg_ring *ring, int len)
{
	struct aotg_td *td;
	dma_addr_t	dma;
	int i = 0;
	u32 token;
	struct aotg_hcep *ep = (struct aotg_hcep *)ring->priv;
	struct device *dev = caninos_to_hcd(myhcd)->self.controller;

	writel(DMACTRL_DMACC, ring->reg_dmactrl);
	usb_clearbitsb(0x80, ep->reg_hcepcon);
	usb_settoggle(ep->udev, ep->epnum, ep->is_out, 0);
	
	caninos_hcep_reset(caninos_to_hcd(myhcd), ep->mask, ENDPRST_FIFORST);
	
	writeb(ep->epnum, ep->reg_hcepctrl);
	usb_setbitsb(0x80, ep->reg_hcepcon);

	dma_free_coherent(dev, ring->intr_mem_size * INTR_TRBS,
		ring->intr_dma_buf_vaddr, ring->intr_dma_buf_phyaddr);

	ring->intr_mem_size = len;
	ring->intr_dma_buf_vaddr = (u8 *)dma_alloc_coherent(dev, len * INTR_TRBS,
		&dma, GFP_ATOMIC);
	if (!ring->intr_dma_buf_vaddr) {
		pr_err("%s err, alloc dma buf for intr fail!\n", __func__);
		return -1;
	}
	ring->intr_dma_buf_phyaddr = dma;

	aotg_reorder_intr_td(ep);
	list_for_each_entry(td, &ep->enring_td_list, enring_list) {
		td->intr_mem_vaddr = ring->intr_dma_buf_vaddr + len * i;
		td->intr_men_phyaddr = ring->intr_dma_buf_phyaddr + len * i;
		memset(td->intr_mem_vaddr, 0, len);

		if (ring->is_out)
			token = TRB_OF | TRB_ITE | TRB_LT;
		else
			token = TRB_OF | TRB_ICE | TRB_CSP;

		enqueue_trb(ring, td->intr_men_phyaddr, len, token);
		i++;
	}
	mb();
	writel(DMACTRL_DMACS, ep->ring->reg_dmactrl);

	return 0;
}

void aotg_intr_dma_buf_free(struct caninos_hcd *myhcd, struct aotg_ring *ring)
{
	struct aotg_td *td, *next;
	struct aotg_hcep *ep = (struct aotg_hcep *)ring->priv;
	struct device *dev = caninos_to_hcd(myhcd)->self.controller;

	list_for_each_entry_safe(td, next, &ep->enring_td_list, enring_list) {
		aotg_release_td(td);
	}

	dma_free_coherent(dev, ring->intr_mem_size * INTR_TRBS,
		ring->intr_dma_buf_vaddr, ring->intr_dma_buf_phyaddr);
}

int aotg_ring_enqueue_isoc_td(struct caninos_hcd *myhcd,
	struct aotg_ring *ring, struct aotg_td *td)
{
	u8 is_out;
	u32 start_addr;
	u32 addr, token, this_trb_len;
	int i = 0;
	int start_frame;
	int num_trbs;
	struct urb *urb = td->urb;

	if (!myhcd || !td || !ring || !urb) {
		
		return -1;
	}

	num_trbs = urb->number_of_packets;
	if (unlikely(num_trbs == 0)) {
		
		return -1;
	}

	if (unlikely(!is_room_on_ring(ring, num_trbs))) {
		return -1;
	}

	is_out = usb_pipeout(urb->pipe);

	td->num_trbs = num_trbs;
	td->trb_vaddr = ring->enqueue_trb;
	td->trb_dma = ring_trb_virt_to_dma(ring, ring->enqueue_trb);

	start_frame = readw(myhcd->ctrl->base + HCFRMNRL);
	start_frame &= 0x3fff;
	urb->start_frame = start_frame;

	start_addr = (u32)urb->transfer_dma;

	if (is_out)
		token = TRB_OF;
	else
		token = TRB_CSP | TRB_OF;

	do {
		addr = start_addr + urb->iso_frame_desc[i].offset;
		this_trb_len = urb->iso_frame_desc[i].length;
		if (num_trbs == 1) {
			token &= ~TRB_CHN;
			if (is_out)
				token |= TRB_ITE;
			else
				token |= TRB_ICE;

			enqueue_trb(ring, addr, this_trb_len, token);
			break;
		}
		enqueue_trb(ring, addr, this_trb_len, token);
		i++;
		num_trbs--;
	} while (num_trbs);

	return 0;
}

void dequeue_td(struct aotg_ring *ring, struct aotg_td *td, int dequeue_flag)
{
	int i, num_trbs;
	struct aotg_trb *trb;

	if (!ring || !td || ((struct list_head *)(&td->enring_list)->next == LIST_POISON1)) {
		
		return;
	}
	trb = td->trb_vaddr;
	num_trbs = td->num_trbs;
	for (i = 0; i < num_trbs; i++) {
		if (dequeue_flag != TD_IN_RING) {
			inc_dequeue_safe(ring);
			memset(trb, 0, sizeof(struct aotg_trb));
		} else {
		/*
		 *perhaps the dequeue urb in the middle of queue_list,
		 *don't change ring->dequeue here
		 */
			trb->hw_token = 0xaa;
		}
		if (trb == ring->last_trb)
			trb = ring->first_trb;
		else
			trb++;
	}
	list_del(&td->enring_list);
	aotg_release_td(td);
	ring->dering_cnt++;
}

void dequeue_intr_td(struct aotg_ring *ring, struct aotg_td *td)
{
	u32 addr, token, this_trb_len;
	struct aotg_trb *trb;
	struct aotg_hcep *ep;

	if (!ring || !td) {
		
		return;
	}

	trb = td->trb_vaddr;
	inc_dequeue_safe(ring);
	list_del(&td->enring_list);
	ring->dering_cnt++;

	td->urb = NULL;
	memset(td->intr_mem_vaddr, 0, ring->intr_mem_size);

	if (ring->is_out)
		token = TRB_OF | TRB_ITE | TRB_LT;
	else
		token = TRB_OF | TRB_ICE | TRB_CSP;
	this_trb_len = ring->intr_mem_size;
	addr = (u32)td->intr_men_phyaddr;

	ep = (struct aotg_hcep *)ring->priv;
	enqueue_trb(ring, addr, this_trb_len, token);
	list_add_tail(&td->enring_list, &ep->enring_td_list);
	ring->enring_cnt++;
	return;
}


int aotg_ring_dequeue_intr_td(struct caninos_hcd *myhcd, struct aotg_hcep *ep,
		struct aotg_ring *ring,	struct aotg_td *td)
{
	u32 addr;
	struct aotg_td *td_tmp;

	/*To INTR transfer: first dequeue happens at the last step of insertion
	  while there isn't any data transmission, so we just empty the urb of
	  the first td but	don't dequeue the td.*/
	/*aotg_stop_ring(ring);*/
	if (td->trb_vaddr->hw_token & TRB_OF)
		td->urb = NULL;
	else
		dequeue_intr_td(ring, td);

	td_tmp = list_first_entry_or_null(&ep->enring_td_list, struct aotg_td, enring_list);
	if ((td_tmp) && (td_tmp->urb)) {
		pr_warn("%s, unormal circumstances, pls check it...\n", __func__);
		pr_warn("restart ep%d intr ring\n", ep->index);
		addr = ring_trb_virt_to_dma(ring, ring->dequeue_trb);
		aotg_start_ring(ring, addr);
	}
	return 0;
}


int aotg_ring_dequeue_td(struct caninos_hcd *myhcd, struct aotg_ring *ring,
	struct aotg_td *td, int dequeue_flag)
{
	int index;
	struct aotg_hcep *ep;
	struct urb *urb = td->urb;
	td->urb = NULL;

	if (dequeue_flag == TD_IN_QUEUE) {
	
		list_del(&td->queue_list);
		aotg_release_td(td);
		return 0;
	} else if (dequeue_flag == TD_IN_RING) {
		ep = (struct aotg_hcep *)urb->ep->hcpriv;
	
		writel(DMACTRL_DMACC, ep->ring->reg_dmactrl);
		usb_clearbitsb(0x80, ep->reg_hcepcon);
		usb_settoggle(ep->udev, ep->epnum, ep->is_out, 0);
		
		caninos_hcep_reset(caninos_to_hcd(myhcd), ep->mask, ENDPRST_FIFORST);
		
		writeb(ep->epnum, ep->reg_hcepctrl);
		usb_setbitsb(0x80, ep->reg_hcepcon);
		/*
		 * dequeue urb, when the urb complete in hardware contex
		 */
		index = ring->mask & 0xf;
		if ((0x1 << index) & (readw(myhcd->ctrl->base + HCINxDMAIRQ0)) ||
			(0x1 << index) & (readw(myhcd->ctrl->base + HCOUTxBUFEMPTYIRQ0))) {
			pr_warn("noticd:%s, IN%dIRQ:0x%x; OUT%dIRQ:0x%x\n",
				__func__, index, readw(myhcd->ctrl->base + HCINxDMAIRQ0),
				index, readw(myhcd->ctrl->base + HCOUTxBUFEMPTYIRQ0));
			clear_ring_irq(myhcd, ring->mask);
		}

		dequeue_td(ring, td, dequeue_flag);
	}
	return 0;
}

unsigned int get_ring_irq(struct caninos_hcd *myhcd)
{
	unsigned int data;
	unsigned int i;
	unsigned int pending = 0;

	data = readw(myhcd->ctrl->base + HCINxDMAIRQ0);

	if (data) {
		for (i = 1; i < 16; i++) {
			if (data & (0x1 << i)) {
				pending = i;
				return pending;
			}
		}
	}

	data = readw(myhcd->ctrl->base + HCOUTxBUFEMPTYIRQ0);

	if (data) {
		for (i = 1; i < 16; i++) {
			if (data & (0x1 << i)) {
				pending = i | AOTG_DMA_OUT_PREFIX;
				return pending;
			}
		}
	}

	return pending;
}

void clear_ring_irq(struct caninos_hcd *myhcd, unsigned int irq_mask)
{
	int index;
	u8 is_out = irq_mask & AOTG_DMA_OUT_PREFIX;
	index = irq_mask & 0xf;

	if (is_out)
		writew((0x1 << index), (myhcd->ctrl->base + HCOUTxBUFEMPTYIRQ0));
	else
		writew((0x1 << index), (myhcd->ctrl->base + HCINxDMAIRQ0));

	return;
}

int finish_td
	(struct caninos_hcd *myhcd, struct aotg_ring *ring, struct aotg_td *td)
{
	struct urb *urb;
	struct aotg_trb *trb;
	int num_trbs;
	int i, trb_tx_len, length = 0;
	int status;
	struct aotg_hcep *ep ;

	urb = td->urb;
	trb = td->trb_vaddr;
	num_trbs = td->num_trbs;

	if (td->cross_ring) {
		if ((ring->last_trb->hw_token & TRB_OF) != 0)
			return -1;
		td->cross_ring = 0;
		aotg_set_trb_as_ring_linkaddr(ring, ring->first_trb);
		usb_setbitsl(DMACTRL_DMACS, (void __iomem *)ring->reg_dmactrl);
		return 1;
	}

	for (i = 0; i < num_trbs; i++) {
		if (trb->hw_token & (AOTG_TRB_IOS | AOTG_TRB_IOZ)) {
			trb_tx_len = trb->hw_buf_len - trb->hw_buf_remain;
			length += trb_tx_len;
			break;
		} else if (trb->hw_token & AOTG_TRB_IOC) {
			length += trb->hw_buf_len;
		} else {
			return -1;
		}
		if (trb == ring->last_trb)
			trb = ring->first_trb;
		else
			trb += 1;
	}
	ep = (struct aotg_hcep *)urb->ep->hcpriv;
	if ((ep->error_count) && (ring->type == PIPE_BULK))
		ep->error_count = 0;

	dequeue_td(ring, td, TD_IN_FINISH);

	urb->actual_length = length;
	status = 0;

	/**
	 * bugfix: td may have been released already.
	 * fetch transfer_flags by urb instead of td->urb.
	 */
	if (urb->actual_length > urb->transfer_buffer_length) {
		urb->actual_length = 0;
		if (urb->transfer_flags & URB_SHORT_NOT_OK)
			status = -EREMOTEIO;
		else
			status = 0;
	}

	usb_hcd_unlink_urb_from_ep(bus_to_hcd(urb->dev->bus), urb);
	spin_unlock(&myhcd->lock);
	usb_hcd_giveback_urb(bus_to_hcd(urb->dev->bus), urb, status);
	spin_lock(&myhcd->lock);

	return 0;
}

int intr_finish_td(struct caninos_hcd *myhcd, struct aotg_ring *ring,
	struct aotg_td *td)
{
	int length;
	int status;
	struct urb *urb;
	struct aotg_trb *trb;

	trb = td->trb_vaddr;
	if (td->urb == NULL) {
		if ((trb->hw_token&TRB_OF) == 0)
				aotg_stop_ring(ring);
		return -1;
	}

	urb = td->urb;

	if (trb->hw_token & (AOTG_TRB_IOS | AOTG_TRB_IOZ))
		length = trb->hw_buf_len - trb->hw_buf_remain;
	else if (trb->hw_token & AOTG_TRB_IOC)
		length = trb->hw_buf_len;
	else
		return -1;

	if (!ring->is_out)
		memcpy(urb->transfer_buffer, td->intr_mem_vaddr, length);
	dequeue_intr_td(ring, td);

	urb->actual_length = length;
	status = 0;

	usb_hcd_unlink_urb_from_ep(bus_to_hcd(urb->dev->bus), urb);
	spin_unlock(&myhcd->lock);
	usb_hcd_giveback_urb(bus_to_hcd(urb->dev->bus), urb, status);
	spin_lock(&myhcd->lock);

	return 0;
}

int isoc_finish_td(struct caninos_hcd *myhcd, struct aotg_ring *ring,
	struct aotg_td *td)
{
	struct urb *urb;
	struct aotg_trb *trb;
	int num_trbs;
	int i, trb_tx_len, length = 0;
	int status;

	if (!ring || !td || ((struct list_head *)(&td->enring_list)->next == LIST_POISON1)) {
		
		return -1;
	}

	urb = td->urb;
	if (!urb || !urb->dev)
		return -1;
	trb = td->trb_vaddr;
	num_trbs = td->num_trbs;

	for (i = 0; i < num_trbs; i++) {
		if (trb->hw_token & (AOTG_TRB_IOS | AOTG_TRB_IOZ)) {
			trb_tx_len = trb->hw_buf_len - trb->hw_buf_remain;
			urb->iso_frame_desc[i].actual_length = trb_tx_len;
			urb->iso_frame_desc[i].status = 0;
			length += trb_tx_len;
		} else if (trb->hw_token & AOTG_TRB_IOC) {
			trb_tx_len = trb->hw_buf_len;
			urb->iso_frame_desc[i].actual_length = trb_tx_len;
			urb->iso_frame_desc[i].status = 0;
			length += trb->hw_buf_len;
		} else {
			return -1;
		}

		if (trb == ring->last_trb)
			trb = ring->first_trb;
		else
			trb += 1;
	}

	td->urb = NULL;
	dequeue_td(ring, td, TD_IN_FINISH);

	urb->actual_length = length;
	status = 0;

	usb_hcd_unlink_urb_from_ep(bus_to_hcd(urb->dev->bus), urb);
	spin_unlock(&myhcd->lock);
	usb_hcd_giveback_urb(bus_to_hcd(urb->dev->bus), urb, status);
	spin_lock(&myhcd->lock);

	return 0;
}

void handle_ring_dma_tx(struct caninos_hcd *myhcd, unsigned int irq_mask)
{
	int ret;
	struct aotg_td *td = NULL, *next;
	struct aotg_ring *ring;
	struct aotg_hcep *ep;

	if (AOTG_IS_DMA_OUT(irq_mask))
		ep = myhcd->outep[AOTG_GET_DMA_NUM(irq_mask)];
	else
		ep = myhcd->inep[AOTG_GET_DMA_NUM(irq_mask)];

	if (ep == NULL) {
		
		return;
	}


	ring = ep->ring;
	if (!ring) {
		
		return;
	}

	if (list_empty(&ep->enring_td_list) || ring_empty(ring))
		return;
	if ((ep->error_count) && (ring->type != PIPE_BULK))
		ep->error_count = 0;
	if (ring->type == PIPE_ISOCHRONOUS) {
		do {
			mb();
			td = list_first_entry_or_null(&ep->enring_td_list, struct aotg_td, enring_list);
			if (!td)
				break;
			ret = isoc_finish_td(myhcd, ring, td);
		} while (ret == 0);

		if (!list_empty(&ep->queue_td_list)) {
			list_for_each_entry_safe(td, next, &ep->queue_td_list, queue_list) {
				ret = aotg_ring_enqueue_isoc_td(myhcd, ring, td);
				if (ret)
					return;
				list_del(&td->queue_list);
				list_add_tail(&td->enring_list, &ep->enring_td_list);
				ring->enring_cnt++;
			}
		}

		if (!list_empty(&ep->enring_td_list) && !is_ring_running(ring)) {
			
			aotg_start_ring(ring, ring_trb_virt_to_dma(ring, ring->dequeue_trb));
		} else if (list_empty(&ep->enring_td_list) && is_ring_running(ring)) {
			aotg_stop_ring(ring);
		}
		return;
	} else if (ring->type == PIPE_INTERRUPT) {
		if (!ring->intr_started)
			return;
		do {
			mb();
			td = list_first_entry_or_null(&ep->enring_td_list, struct aotg_td, enring_list);
			if (!td)
				break;
			ret = intr_finish_td(myhcd, ring, td);
		} while (ret == 0);

		if (!is_ring_running(ring)) {
			pr_debug("%s, intr stop!\n", __func__);
		}
		return;
	}

	do {
		mb();
		td = list_first_entry_or_null(&ep->enring_td_list, struct aotg_td, enring_list);
		if (!td)
			break;
		ret = finish_td(myhcd, ring, td);
		if (ep->hep->enabled == 0)
			break;
	} while (ret == 0);

	if (ret == 1)
		return;

	if (!list_empty(&ep->queue_td_list)) {
		list_for_each_entry_safe(td, next, &ep->queue_td_list, queue_list) {
			ret = aotg_ring_enqueue_td(myhcd, ring, td);
			if (ret)
				return;
			list_del(&td->queue_list);
			list_add_tail(&td->enring_list, &ep->enring_td_list);
			ring->enring_cnt++;
		}
	}

	if (!list_empty(&ep->enring_td_list) && !is_ring_running(ring))
		aotg_start_ring(ring, ring_trb_virt_to_dma(ring, ring->dequeue_trb));
	else if (list_empty(&ep->enring_td_list) && is_ring_running(ring))
		aotg_stop_ring(ring);
}

void aotg_ring_irq_handler(struct caninos_hcd *myhcd)
{
	unsigned int irq_mask;
	unsigned long flags;
	bool dma_nest = false;
	
	spin_lock_irqsave(&myhcd->lock, flags);
	
	if (myhcd->check_trb_mutex) {
		dma_nest = true;
	}
	
	myhcd->check_trb_mutex = true;
	
	do {
		irq_mask = get_ring_irq(myhcd);
		
		if (irq_mask == 0)
		{
			myhcd->check_trb_mutex = false;
			spin_unlock_irqrestore(&myhcd->lock, flags);
			return;
		}
		
		clear_ring_irq(myhcd, irq_mask);

		if (dma_nest == true)
		{
			spin_unlock_irqrestore(&myhcd->lock, flags);
			return;
		}

		handle_ring_dma_tx(myhcd, irq_mask);
	} while (irq_mask);
	
	myhcd->check_trb_mutex = false;
	spin_unlock_irqrestore(&myhcd->lock, flags);
}
