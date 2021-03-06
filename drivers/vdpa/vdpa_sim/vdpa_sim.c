// SPDX-License-Identifier: GPL-2.0-only
/*
 * VDPA networking device simulator.
 *
 * Copyright (c) 2020, Red Hat Inc. All rights reserved.
 *     Author: Jason Wang <jasowang@redhat.com>
 *
 */

#include <linux/init.h>
#include <linux/module.h>
#include <linux/device.h>
#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/poll.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/wait.h>
#include <linux/uuid.h>
#include <linux/iommu.h>
#include <linux/dma-mapping.h>
#include <linux/sysfs.h>
#include <linux/file.h>
#include <linux/etherdevice.h>
#include <linux/vringh.h>
#include <linux/vdpa.h>
#include <linux/vhost_iotlb.h>
#include <uapi/linux/virtio_config.h>
#include <uapi/linux/virtio_net.h>

#define DRV_VERSION  "0.1"
#define DRV_AUTHOR   "Jason Wang <jasowang@redhat.com>"
#define DRV_DESC     "vDPA Device Simulator"
#define DRV_LICENSE  "GPL v2"

struct vdpasim_virtqueue {
	struct vringh vring;
	struct vringh_kiov iov;
	unsigned short head;
	bool ready;
	u64 desc_addr;
	u64 device_addr;
	u64 driver_addr;
	u32 num;
	void *private;
	irqreturn_t (*cb)(void *data);
};

#define VDPASIM_QUEUE_ALIGN PAGE_SIZE
#define VDPASIM_QUEUE_MAX 256
#define VDPASIM_DEVICE_ID 0x1
#define VDPASIM_VENDOR_ID 0
#define VDPASIM_VQ_NUM 0x2
#define VDPASIM_NAME "vdpasim-netdev"

static u64 vdpasim_features = (1ULL << VIRTIO_F_ANY_LAYOUT) |
			      (1ULL << VIRTIO_F_VERSION_1)  |
			      (1ULL << VIRTIO_F_IOMMU_PLATFORM);

/* State of each vdpasim device */
struct vdpasim {
	struct vdpa_device vdpa;
	struct vdpasim_virtqueue vqs[2];
	struct work_struct work;
	/* spinlock to synchronize virtqueue state */
	spinlock_t lock;
	struct virtio_net_config config;
	struct vhost_iotlb *iommu;
	void *buffer;
	u32 status;
	u32 generation;
	u64 features;
	/* spinlock to synchronize iommu table */
	spinlock_t iommu_lock;
};

static struct vdpasim *vdpasim_dev;

static struct vdpasim *vdpa_to_sim(struct vdpa_device *vdpa)
{
	return container_of(vdpa, struct vdpasim, vdpa);
}

static struct vdpasim *dev_to_sim(struct device *dev)
{
	struct vdpa_device *vdpa = dev_to_vdpa(dev);

	return vdpa_to_sim(vdpa);
}

static void vdpasim_queue_ready(struct vdpasim *vdpasim, unsigned int idx)
{
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	vringh_init_iotlb(&vq->vring, vdpasim_features,
			  VDPASIM_QUEUE_MAX, false,
			  (struct vring_desc *)(uintptr_t)vq->desc_addr,
			  (struct vring_avail *)
			  (uintptr_t)vq->driver_addr,
			  (struct vring_used *)
			  (uintptr_t)vq->device_addr);
}

static void vdpasim_vq_reset(struct vdpasim_virtqueue *vq)
{
	vq->ready = false;
	vq->desc_addr = 0;
	vq->driver_addr = 0;
	vq->device_addr = 0;
	vq->cb = NULL;
	vq->private = NULL;
	vringh_init_iotlb(&vq->vring, vdpasim_features, VDPASIM_QUEUE_MAX,
			  false, NULL, NULL, NULL);
}

static void vdpasim_reset(struct vdpasim *vdpasim)
{
	int i;

	for (i = 0; i < VDPASIM_VQ_NUM; i++)
		vdpasim_vq_reset(&vdpasim->vqs[i]);

	spin_lock(&vdpasim->iommu_lock);
	vhost_iotlb_reset(vdpasim->iommu);
	spin_unlock(&vdpasim->iommu_lock);

	vdpasim->features = 0;
	vdpasim->status = 0;
	++vdpasim->generation;
}

static void vdpasim_work(struct work_struct *work)
{
	struct vdpasim *vdpasim = container_of(work, struct
						 vdpasim, work);
	struct vdpasim_virtqueue *txq = &vdpasim->vqs[1];
	struct vdpasim_virtqueue *rxq = &vdpasim->vqs[0];
	ssize_t read, write;
	size_t total_write;
	int pkts = 0;
	int err;

	spin_lock(&vdpasim->lock);

	if (!(vdpasim->status & VIRTIO_CONFIG_S_DRIVER_OK))
		goto out;

	if (!txq->ready || !rxq->ready)
		goto out;

	while (true) {
		total_write = 0;
		err = vringh_getdesc_iotlb(&txq->vring, &txq->iov, NULL,
					   &txq->head, GFP_ATOMIC);
		if (err <= 0)
			break;

		err = vringh_getdesc_iotlb(&rxq->vring, NULL, &rxq->iov,
					   &rxq->head, GFP_ATOMIC);
		if (err <= 0) {
			vringh_complete_iotlb(&txq->vring, txq->head, 0);
			break;
		}

		while (true) {
			read = vringh_iov_pull_iotlb(&txq->vring, &txq->iov,
						     vdpasim->buffer,
						     PAGE_SIZE);
			if (read <= 0)
				break;

			write = vringh_iov_push_iotlb(&rxq->vring, &rxq->iov,
						      vdpasim->buffer, read);
			if (write <= 0)
				break;

			total_write += write;
		}

		/* Make sure data is wrote before advancing index */
		smp_wmb();

		vringh_complete_iotlb(&txq->vring, txq->head, 0);
		vringh_complete_iotlb(&rxq->vring, rxq->head, total_write);

		/* Make sure used is visible before rasing the interrupt. */
		smp_wmb();

		local_bh_disable();
		if (txq->cb)
			txq->cb(txq->private);
		if (rxq->cb)
			rxq->cb(rxq->private);
		local_bh_enable();

		if (++pkts > 4) {
			schedule_work(&vdpasim->work);
			goto out;
		}
	}

out:
	spin_unlock(&vdpasim->lock);
}

static int dir_to_perm(enum dma_data_direction dir)
{
	int perm = -EFAULT;

	switch (dir) {
	case DMA_FROM_DEVICE:
		perm = VHOST_MAP_WO;
		break;
	case DMA_TO_DEVICE:
		perm = VHOST_MAP_RO;
		break;
	case DMA_BIDIRECTIONAL:
		perm = VHOST_MAP_RW;
		break;
	default:
		break;
	}

	return perm;
}

static dma_addr_t vdpasim_map_page(struct device *dev, struct page *page,
				   unsigned long offset, size_t size,
				   enum dma_data_direction dir,
				   unsigned long attrs)
{
	struct vdpasim *vdpasim = dev_to_sim(dev);
	struct vhost_iotlb *iommu = vdpasim->iommu;
	u64 pa = (page_to_pfn(page) << PAGE_SHIFT) + offset;
	int ret, perm = dir_to_perm(dir);

	if (perm < 0)
		return DMA_MAPPING_ERROR;

	/* For simplicity, use identical mapping to avoid e.g iova
	 * allocator.
	 */
	spin_lock(&vdpasim->iommu_lock);
	ret = vhost_iotlb_add_range(iommu, pa, pa + size - 1,
				    pa, dir_to_perm(dir));
	spin_unlock(&vdpasim->iommu_lock);
	if (ret)
		return DMA_MAPPING_ERROR;

	return (dma_addr_t)(pa);
}

static void vdpasim_unmap_page(struct device *dev, dma_addr_t dma_addr,
			       size_t size, enum dma_data_direction dir,
			       unsigned long attrs)
{
	struct vdpasim *vdpasim = dev_to_sim(dev);
	struct vhost_iotlb *iommu = vdpasim->iommu;

	spin_lock(&vdpasim->iommu_lock);
	vhost_iotlb_del_range(iommu, (u64)dma_addr,
			      (u64)dma_addr + size - 1);
	spin_unlock(&vdpasim->iommu_lock);
}

static void *vdpasim_alloc_coherent(struct device *dev, size_t size,
				    dma_addr_t *dma_addr, gfp_t flag,
				    unsigned long attrs)
{
	struct vdpasim *vdpasim = dev_to_sim(dev);
	struct vhost_iotlb *iommu = vdpasim->iommu;
	void *addr = kmalloc(size, flag);
	int ret;

	spin_lock(&vdpasim->iommu_lock);
	if (!addr) {
		*dma_addr = DMA_MAPPING_ERROR;
	} else {
		u64 pa = virt_to_phys(addr);

		ret = vhost_iotlb_add_range(iommu, (u64)pa,
					    (u64)pa + size - 1,
					    pa, VHOST_MAP_RW);
		if (ret) {
			*dma_addr = DMA_MAPPING_ERROR;
			kfree(addr);
			addr = NULL;
		} else
			*dma_addr = (dma_addr_t)pa;
	}
	spin_unlock(&vdpasim->iommu_lock);

	return addr;
}

static void vdpasim_free_coherent(struct device *dev, size_t size,
				  void *vaddr, dma_addr_t dma_addr,
				  unsigned long attrs)
{
	struct vdpasim *vdpasim = dev_to_sim(dev);
	struct vhost_iotlb *iommu = vdpasim->iommu;

	spin_lock(&vdpasim->iommu_lock);
	vhost_iotlb_del_range(iommu, (u64)dma_addr,
			      (u64)dma_addr + size - 1);
	spin_unlock(&vdpasim->iommu_lock);

	kfree(phys_to_virt((uintptr_t)dma_addr));
}

static const struct dma_map_ops vdpasim_dma_ops = {
	.map_page = vdpasim_map_page,
	.unmap_page = vdpasim_unmap_page,
	.alloc = vdpasim_alloc_coherent,
	.free = vdpasim_free_coherent,
};

static const struct vdpa_config_ops vdpasim_net_config_ops;

static struct vdpasim *vdpasim_create(void)
{
	struct virtio_net_config *config;
	struct vdpasim *vdpasim;
	struct device *dev;
	int ret = -ENOMEM;

	vdpasim = vdpa_alloc_device(struct vdpasim, vdpa, NULL,
				    &vdpasim_net_config_ops);
	if (!vdpasim)
		goto err_alloc;

	INIT_WORK(&vdpasim->work, vdpasim_work);
	spin_lock_init(&vdpasim->lock);
	spin_lock_init(&vdpasim->iommu_lock);

	dev = &vdpasim->vdpa.dev;
	dev->dma_mask = &dev->coherent_dma_mask;
	if (dma_set_mask_and_coherent(dev, DMA_BIT_MASK(64)))
		goto err_iommu;
	set_dma_ops(dev, &vdpasim_dma_ops);

	vdpasim->iommu = vhost_iotlb_alloc(2048, 0);
	if (!vdpasim->iommu)
		goto err_iommu;

	vdpasim->buffer = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!vdpasim->buffer)
		goto err_iommu;

	config = &vdpasim->config;
	config->mtu = 1500;
	config->status = VIRTIO_NET_S_LINK_UP;
	eth_random_addr(config->mac);

	vringh_set_iotlb(&vdpasim->vqs[0].vring, vdpasim->iommu);
	vringh_set_iotlb(&vdpasim->vqs[1].vring, vdpasim->iommu);

	vdpasim->vdpa.dma_dev = dev;
	ret = vdpa_register_device(&vdpasim->vdpa);
	if (ret)
		goto err_iommu;

	return vdpasim;

err_iommu:
	put_device(dev);
err_alloc:
	return ERR_PTR(ret);
}

static int vdpasim_set_vq_address(struct vdpa_device *vdpa, u16 idx,
				  u64 desc_area, u64 driver_area,
				  u64 device_area)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	vq->desc_addr = desc_area;
	vq->driver_addr = driver_area;
	vq->device_addr = device_area;

	return 0;
}

static void vdpasim_set_vq_num(struct vdpa_device *vdpa, u16 idx, u32 num)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	vq->num = num;
}

static void vdpasim_kick_vq(struct vdpa_device *vdpa, u16 idx)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	if (vq->ready)
		schedule_work(&vdpasim->work);
}

static void vdpasim_set_vq_cb(struct vdpa_device *vdpa, u16 idx,
			      struct vdpa_callback *cb)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	vq->cb = cb->callback;
	vq->private = cb->private;
}

static void vdpasim_set_vq_ready(struct vdpa_device *vdpa, u16 idx, bool ready)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	spin_lock(&vdpasim->lock);
	vq->ready = ready;
	if (vq->ready)
		vdpasim_queue_ready(vdpasim, idx);
	spin_unlock(&vdpasim->lock);
}

static bool vdpasim_get_vq_ready(struct vdpa_device *vdpa, u16 idx)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];

	return vq->ready;
}

static int vdpasim_set_vq_state(struct vdpa_device *vdpa, u16 idx, u64 state)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];
	struct vringh *vrh = &vq->vring;

	spin_lock(&vdpasim->lock);
	vrh->last_avail_idx = state;
	spin_unlock(&vdpasim->lock);

	return 0;
}

static u64 vdpasim_get_vq_state(struct vdpa_device *vdpa, u16 idx)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vdpasim_virtqueue *vq = &vdpasim->vqs[idx];
	struct vringh *vrh = &vq->vring;

	return vrh->last_avail_idx;
}

static u32 vdpasim_get_vq_align(struct vdpa_device *vdpa)
{
	return VDPASIM_QUEUE_ALIGN;
}

static u64 vdpasim_get_features(struct vdpa_device *vdpa)
{
	return vdpasim_features;
}

static int vdpasim_set_features(struct vdpa_device *vdpa, u64 features)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);

	/* DMA mapping must be done by driver */
	if (!(features & (1ULL << VIRTIO_F_IOMMU_PLATFORM)))
		return -EINVAL;

	vdpasim->features = features & vdpasim_features;

	return 0;
}

static void vdpasim_set_config_cb(struct vdpa_device *vdpa,
				  struct vdpa_callback *cb)
{
	/* We don't support config interrupt */
}

static u16 vdpasim_get_vq_num_max(struct vdpa_device *vdpa)
{
	return VDPASIM_QUEUE_MAX;
}

static u32 vdpasim_get_device_id(struct vdpa_device *vdpa)
{
	return VDPASIM_DEVICE_ID;
}

static u32 vdpasim_get_vendor_id(struct vdpa_device *vdpa)
{
	return VDPASIM_VENDOR_ID;
}

static u8 vdpasim_get_status(struct vdpa_device *vdpa)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	u8 status;

	spin_lock(&vdpasim->lock);
	status = vdpasim->status;
	spin_unlock(&vdpasim->lock);

	return status;
}

static void vdpasim_set_status(struct vdpa_device *vdpa, u8 status)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);

	spin_lock(&vdpasim->lock);
	vdpasim->status = status;
	if (status == 0)
		vdpasim_reset(vdpasim);
	spin_unlock(&vdpasim->lock);
}

static void vdpasim_get_config(struct vdpa_device *vdpa, unsigned int offset,
			     void *buf, unsigned int len)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);

	if (offset + len < sizeof(struct virtio_net_config))
		memcpy(buf, (u8 *)&vdpasim->config + offset, len);
}

static void vdpasim_set_config(struct vdpa_device *vdpa, unsigned int offset,
			     const void *buf, unsigned int len)
{
	/* No writable config supportted by vdpasim */
}

static u32 vdpasim_get_generation(struct vdpa_device *vdpa)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);

	return vdpasim->generation;
}

static int vdpasim_set_map(struct vdpa_device *vdpa,
			   struct vhost_iotlb *iotlb)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	struct vhost_iotlb_map *map;
	u64 start = 0ULL, last = 0ULL - 1;
	int ret;

	spin_lock(&vdpasim->iommu_lock);
	vhost_iotlb_reset(vdpasim->iommu);

	for (map = vhost_iotlb_itree_first(iotlb, start, last); map;
	     map = vhost_iotlb_itree_next(map, start, last)) {
		ret = vhost_iotlb_add_range(vdpasim->iommu, map->start,
					    map->last, map->addr, map->perm);
		if (ret)
			goto err;
	}
	spin_unlock(&vdpasim->iommu_lock);
	return 0;

err:
	vhost_iotlb_reset(vdpasim->iommu);
	spin_unlock(&vdpasim->iommu_lock);
	return ret;
}

static int vdpasim_dma_map(struct vdpa_device *vdpa, u64 iova, u64 size,
			   u64 pa, u32 perm)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);
	int ret;

	spin_lock(&vdpasim->iommu_lock);
	ret = vhost_iotlb_add_range(vdpasim->iommu, iova, iova + size - 1, pa,
				    perm);
	spin_unlock(&vdpasim->iommu_lock);

	return ret;
}

static int vdpasim_dma_unmap(struct vdpa_device *vdpa, u64 iova, u64 size)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);

	spin_lock(&vdpasim->iommu_lock);
	vhost_iotlb_del_range(vdpasim->iommu, iova, iova + size - 1);
	spin_unlock(&vdpasim->iommu_lock);

	return 0;
}

static void vdpasim_free(struct vdpa_device *vdpa)
{
	struct vdpasim *vdpasim = vdpa_to_sim(vdpa);

	cancel_work_sync(&vdpasim->work);
	kfree(vdpasim->buffer);
	if (vdpasim->iommu)
		vhost_iotlb_free(vdpasim->iommu);
}

static const struct vdpa_config_ops vdpasim_net_config_ops = {
	.set_vq_address         = vdpasim_set_vq_address,
	.set_vq_num             = vdpasim_set_vq_num,
	.kick_vq                = vdpasim_kick_vq,
	.set_vq_cb              = vdpasim_set_vq_cb,
	.set_vq_ready           = vdpasim_set_vq_ready,
	.get_vq_ready           = vdpasim_get_vq_ready,
	.set_vq_state           = vdpasim_set_vq_state,
	.get_vq_state           = vdpasim_get_vq_state,
	.get_vq_align           = vdpasim_get_vq_align,
	.get_features           = vdpasim_get_features,
	.set_features           = vdpasim_set_features,
	.set_config_cb          = vdpasim_set_config_cb,
	.get_vq_num_max         = vdpasim_get_vq_num_max,
	.get_device_id          = vdpasim_get_device_id,
	.get_vendor_id          = vdpasim_get_vendor_id,
	.get_status             = vdpasim_get_status,
	.set_status             = vdpasim_set_status,
	.get_config             = vdpasim_get_config,
	.set_config             = vdpasim_set_config,
	.get_generation         = vdpasim_get_generation,
	.set_map                = vdpasim_set_map,
	.dma_map                = vdpasim_dma_map,
	.dma_unmap              = vdpasim_dma_unmap,
	.free                   = vdpasim_free,
};

static int __init vdpasim_dev_init(void)
{
	vdpasim_dev = vdpasim_create();

	if (!IS_ERR(vdpasim_dev))
		return 0;

	return PTR_ERR(vdpasim_dev);
}

static void __exit vdpasim_dev_exit(void)
{
	struct vdpa_device *vdpa = &vdpasim_dev->vdpa;

	vdpa_unregister_device(vdpa);
}

module_init(vdpasim_dev_init)
module_exit(vdpasim_dev_exit)

MODULE_VERSION(DRV_VERSION);
MODULE_LICENSE(DRV_LICENSE);
MODULE_AUTHOR(DRV_AUTHOR);
MODULE_DESCRIPTION(DRV_DESC);
