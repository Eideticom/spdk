/*-
 *   BSD LICENSE
 *
 *   Copyright (c) Intel Corporation.
 *   All rights reserved.
 *
 *   Redistribution and use in source and binary forms, with or without
 *   modification, are permitted provided that the following conditions
 *   are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 *   THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 *   "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 *   LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 *   A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 *   OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 *   SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 *   LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 *   DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 *   THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 *   (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 *   OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

/*
 * NVMe over PCIe transport
 */

#include "nvme_internal.h"

/* PCIe transport extensions for spdk_nvme_ctrlr */
struct nvme_pcie_ctrlr {
	struct spdk_nvme_ctrlr ctrlr;

	/** NVMe MMIO register space */
	volatile struct spdk_nvme_registers *regs;

	/* BAR mapping address which contains controller memory buffer */
	void *cmb_bar_virt_addr;

	/* BAR physical address which contains controller memory buffer */
	uint64_t cmb_bar_phys_addr;

	/* Controller memory buffer size in Bytes */
	uint64_t cmb_size;

	/* Current offset of controller memory buffer */
	uint64_t cmb_current_offset;

	/** stride in uint32_t units between doorbell registers (1 = 4 bytes, 2 = 8 bytes, ...) */
	uint32_t doorbell_stride_u32;
};
SPDK_STATIC_ASSERT(offsetof(struct nvme_pcie_ctrlr, ctrlr) == 0, "ctrlr must be first field");

static inline struct nvme_pcie_ctrlr *
nvme_pcie_ctrlr(struct spdk_nvme_ctrlr *ctrlr)
{
	assert(ctrlr->transport == &spdk_nvme_transport_pcie);
	return (struct nvme_pcie_ctrlr *)ctrlr;
}

static int
nvme_pcie_ctrlr_get_pci_id(struct spdk_nvme_ctrlr *ctrlr, struct pci_id *pci_id)
{
	struct spdk_pci_device *pci_dev;

	assert(ctrlr != NULL);
	assert(pci_id != NULL);

	pci_dev = ctrlr->devhandle;
	assert(pci_dev != NULL);

	pci_id->vendor_id = spdk_pci_device_get_vendor_id(pci_dev);
	pci_id->dev_id = spdk_pci_device_get_device_id(pci_dev);
	pci_id->sub_vendor_id = spdk_pci_device_get_subvendor_id(pci_dev);
	pci_id->sub_dev_id = spdk_pci_device_get_subdevice_id(pci_dev);

	return 0;
}

static volatile void *
nvme_pcie_reg_addr(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	return (volatile void *)((uintptr_t)pctrlr->regs + offset);
}

static int
nvme_pcie_ctrlr_set_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	spdk_mmio_write_4(nvme_pcie_reg_addr(ctrlr, offset), value);
	return 0;
}

static int
nvme_pcie_ctrlr_set_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	spdk_mmio_write_8(nvme_pcie_reg_addr(ctrlr, offset), value);
	return 0;
}

static int
nvme_pcie_ctrlr_get_reg_4(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint32_t *value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 4);
	assert(value != NULL);
	*value = spdk_mmio_read_4(nvme_pcie_reg_addr(ctrlr, offset));
	return 0;
}

static int
nvme_pcie_ctrlr_get_reg_8(struct spdk_nvme_ctrlr *ctrlr, uint32_t offset, uint64_t *value)
{
	assert(offset <= sizeof(struct spdk_nvme_registers) - 8);
	assert(value != NULL);
	*value = spdk_mmio_read_8(nvme_pcie_reg_addr(ctrlr, offset));
	return 0;
}

static int
nvme_pcie_ctrlr_get_cmbloc(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbloc_register *cmbloc)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbloc.raw),
					 &cmbloc->raw);
}

static int
nvme_pcie_ctrlr_get_cmbsz(struct nvme_pcie_ctrlr *pctrlr, union spdk_nvme_cmbsz_register *cmbsz)
{
	return nvme_pcie_ctrlr_get_reg_4(&pctrlr->ctrlr, offsetof(struct spdk_nvme_registers, cmbsz.raw),
					 &cmbsz->raw);
}

static void
nvme_pcie_ctrlr_map_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr;
	uint32_t bir;
	union spdk_nvme_cmbsz_register cmbsz;
	union spdk_nvme_cmbloc_register cmbloc;
	uint64_t size, unit_size, offset, bar_size, bar_phys_addr;

	if (nvme_pcie_ctrlr_get_cmbsz(pctrlr, &cmbsz) ||
	    nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
		SPDK_TRACELOG(SPDK_TRACE_NVME, "get registers failed\n");
		goto exit;
	}

	if (!cmbsz.bits.sz)
		goto exit;

	bir = cmbloc.bits.bir;
	/* Values 0 2 3 4 5 are valid for BAR */
	if (bir > 5 || bir == 1)
		goto exit;

	/* unit size for 4KB/64KB/1MB/16MB/256MB/4GB/64GB */
	unit_size = (uint64_t)1 << (12 + 4 * cmbsz.bits.szu);
	/* controller memory buffer size in Bytes */
	size = unit_size * cmbsz.bits.sz;
	/* controller memory buffer offset from BAR in Bytes */
	offset = unit_size * cmbloc.bits.ofst;

	rc = spdk_pci_device_map_bar(pctrlr->ctrlr.devhandle, bir, &addr,
				     &bar_phys_addr, &bar_size);
	if ((rc != 0) || addr == NULL) {
		goto exit;
	}

	if (offset > bar_size) {
		goto exit;
	}

	if (size > bar_size - offset) {
		goto exit;
	}

	pctrlr->cmb_bar_virt_addr = addr;
	pctrlr->cmb_bar_phys_addr = bar_phys_addr;
	pctrlr->cmb_size = size;
	pctrlr->cmb_current_offset = offset;

	if (!cmbsz.bits.sqs) {
		pctrlr->ctrlr.opts.use_cmb_sqs = false;
	}

	return;
exit:
	pctrlr->cmb_bar_virt_addr = NULL;
	pctrlr->ctrlr.opts.use_cmb_sqs = false;
	return;
}

static int
nvme_pcie_ctrlr_unmap_cmb(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	union spdk_nvme_cmbloc_register cmbloc;
	void *addr = pctrlr->cmb_bar_virt_addr;

	if (addr) {
		if (nvme_pcie_ctrlr_get_cmbloc(pctrlr, &cmbloc)) {
			SPDK_TRACELOG(SPDK_TRACE_NVME, "get_cmbloc() failed\n");
			return -EIO;
		}
		rc = spdk_pci_device_unmap_bar(pctrlr->ctrlr.devhandle, cmbloc.bits.bir, addr);
	}
	return rc;
}

static int
nvme_pcie_ctrlr_alloc_cmb(struct spdk_nvme_ctrlr *ctrlr, uint64_t length, uint64_t aligned,
			  uint64_t *offset)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	uint64_t round_offset;

	round_offset = pctrlr->cmb_current_offset;
	round_offset = (round_offset + (aligned - 1)) & ~(aligned - 1);

	if (round_offset + length > pctrlr->cmb_size)
		return -1;

	*offset = round_offset;
	pctrlr->cmb_current_offset = round_offset + length;

	return 0;
}

static int
nvme_pcie_ctrlr_allocate_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc;
	void *addr;
	uint64_t phys_addr, size;

	rc = spdk_pci_device_map_bar(pctrlr->ctrlr.devhandle, 0, &addr,
				     &phys_addr, &size);
	pctrlr->regs = (volatile struct spdk_nvme_registers *)addr;
	if ((pctrlr->regs == NULL) || (rc != 0)) {
		SPDK_ERRLOG("nvme_pcicfg_map_bar failed with rc %d or bar %p\n",
			    rc, pctrlr->regs);
		return -1;
	}

	nvme_pcie_ctrlr_map_cmb(pctrlr);

	return 0;
}

static int
nvme_pcie_ctrlr_free_bars(struct nvme_pcie_ctrlr *pctrlr)
{
	int rc = 0;
	void *addr = (void *)pctrlr->regs;

	rc = nvme_pcie_ctrlr_unmap_cmb(pctrlr);
	if (rc != 0) {
		SPDK_ERRLOG("nvme_ctrlr_unmap_cmb failed with error code %d\n", rc);
		return -1;
	}

	if (addr) {
		rc = spdk_pci_device_unmap_bar(pctrlr->ctrlr.devhandle, 0, addr);
	}
	return rc;
}

static int
nvme_pcie_ctrlr_construct(struct spdk_nvme_ctrlr *ctrlr, void *devhandle)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);
	union spdk_nvme_cap_register cap;
	uint32_t cmd_reg;
	int rc;

	rc = nvme_pcie_ctrlr_allocate_bars(pctrlr);
	if (rc != 0) {
		return rc;
	}

	/* Enable PCI busmaster and disable INTx */
	spdk_pci_device_cfg_read32(devhandle, &cmd_reg, 4);
	cmd_reg |= 0x404;
	spdk_pci_device_cfg_write32(devhandle, cmd_reg, 4);

	if (nvme_ctrlr_get_cap(ctrlr, &cap)) {
		SPDK_TRACELOG(SPDK_TRACE_NVME, "get_cap() failed\n");
		return -EIO;
	}

	/* Doorbell stride is 2 ^ (dstrd + 2),
	 * but we want multiples of 4, so drop the + 2 */
	pctrlr->doorbell_stride_u32 = 1 << cap.bits.dstrd;

	/* Save the PCI address */
	ctrlr->pci_addr.domain = spdk_pci_device_get_domain(devhandle);
	ctrlr->pci_addr.bus = spdk_pci_device_get_bus(devhandle);
	ctrlr->pci_addr.dev = spdk_pci_device_get_dev(devhandle);
	ctrlr->pci_addr.func = spdk_pci_device_get_func(devhandle);

	return 0;
}

static void
nvme_pcie_ctrlr_destruct(struct spdk_nvme_ctrlr *ctrlr)
{
	struct nvme_pcie_ctrlr *pctrlr = nvme_pcie_ctrlr(ctrlr);

	nvme_pcie_ctrlr_free_bars(pctrlr);
}

static void
nvme_qpair_construct_tracker(struct nvme_tracker *tr, uint16_t cid, uint64_t phys_addr)
{
	tr->prp_sgl_bus_addr = phys_addr + offsetof(struct nvme_tracker, u.prp);
	tr->cid = cid;
	tr->active = false;
}

static void
nvme_pcie_qpair_reset(struct spdk_nvme_qpair *qpair)
{
	qpair->sq_tail = qpair->cq_head = 0;

	/*
	 * First time through the completion queue, HW will set phase
	 *  bit on completions to 1.  So set this to 1 here, indicating
	 *  we're looking for a 1 to know which entries have completed.
	 *  we'll toggle the bit each time when the completion queue
	 *  rolls over.
	 */
	qpair->phase = 1;

	memset(qpair->cmd, 0,
	       qpair->num_entries * sizeof(struct spdk_nvme_cmd));
	memset(qpair->cpl, 0,
	       qpair->num_entries * sizeof(struct spdk_nvme_cpl));
}

static int
nvme_pcie_qpair_construct(struct spdk_nvme_qpair *qpair)
{
	struct spdk_nvme_ctrlr	*ctrlr = qpair->ctrlr;
	struct nvme_pcie_ctrlr	*pctrlr = nvme_pcie_ctrlr(ctrlr);
	struct nvme_tracker	*tr;
	uint16_t		i;
	volatile uint32_t	*doorbell_base;
	uint64_t		phys_addr = 0;
	uint64_t		offset;
	uint16_t		num_trackers;

	if (qpair->id == 0) {
		num_trackers = NVME_ADMIN_TRACKERS;
	} else {
		/*
		 * No need to have more trackers than entries in the submit queue.
		 *  Note also that for a queue size of N, we can only have (N-1)
		 *  commands outstanding, hence the "-1" here.
		 */
		num_trackers = nvme_min(NVME_IO_TRACKERS, qpair->num_entries - 1);
	}

	assert(num_trackers != 0);

	qpair->sq_in_cmb = false;

	/* cmd and cpl rings must be aligned on 4KB boundaries. */
	if (ctrlr->opts.use_cmb_sqs) {
		if (nvme_pcie_ctrlr_alloc_cmb(ctrlr, qpair->num_entries * sizeof(struct spdk_nvme_cmd),
					      0x1000, &offset) == 0) {
			qpair->cmd = pctrlr->cmb_bar_virt_addr + offset;
			qpair->cmd_bus_addr = pctrlr->cmb_bar_phys_addr + offset;
			qpair->sq_in_cmb = true;
		}
	}
	if (qpair->sq_in_cmb == false) {
		qpair->cmd = spdk_zmalloc(qpair->num_entries * sizeof(struct spdk_nvme_cmd),
					  0x1000,
					  &qpair->cmd_bus_addr);
		if (qpair->cmd == NULL) {
			SPDK_ERRLOG("alloc qpair_cmd failed\n");
			return -ENOMEM;
		}
	}

	qpair->cpl = spdk_zmalloc(qpair->num_entries * sizeof(struct spdk_nvme_cpl),
				  0x1000,
				  &qpair->cpl_bus_addr);
	if (qpair->cpl == NULL) {
		SPDK_ERRLOG("alloc qpair_cpl failed\n");
		return -ENOMEM;
	}

	doorbell_base = &pctrlr->regs->doorbell[0].sq_tdbl;
	qpair->sq_tdbl = doorbell_base + (2 * qpair->id + 0) * pctrlr->doorbell_stride_u32;
	qpair->cq_hdbl = doorbell_base + (2 * qpair->id + 1) * pctrlr->doorbell_stride_u32;

	/*
	 * Reserve space for all of the trackers in a single allocation.
	 *   struct nvme_tracker must be padded so that its size is already a power of 2.
	 *   This ensures the PRP list embedded in the nvme_tracker object will not span a
	 *   4KB boundary, while allowing access to trackers in tr[] via normal array indexing.
	 */
	qpair->tr = spdk_zmalloc(num_trackers * sizeof(*tr), sizeof(*tr), &phys_addr);
	if (qpair->tr == NULL) {
		SPDK_ERRLOG("nvme_tr failed\n");
		return -ENOMEM;
	}

	for (i = 0; i < num_trackers; i++) {
		tr = &qpair->tr[i];
		nvme_qpair_construct_tracker(tr, i, phys_addr);
		LIST_INSERT_HEAD(&qpair->free_tr, tr, list);
		phys_addr += sizeof(struct nvme_tracker);
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

static inline void
nvme_pcie_copy_command(struct spdk_nvme_cmd *dst, const struct spdk_nvme_cmd *src)
{
	/* dst and src are known to be non-overlapping and 64-byte aligned. */
#if defined(__AVX__)
	__m256i *d256 = (__m256i *)dst;
	const __m256i *s256 = (const __m256i *)src;

	_mm256_store_si256(&d256[0], _mm256_load_si256(&s256[0]));
	_mm256_store_si256(&d256[1], _mm256_load_si256(&s256[1]));
#elif defined(__SSE2__)
	__m128i *d128 = (__m128i *)dst;
	const __m128i *s128 = (const __m128i *)src;

	_mm_store_si128(&d128[0], _mm_load_si128(&s128[0]));
	_mm_store_si128(&d128[1], _mm_load_si128(&s128[1]));
	_mm_store_si128(&d128[2], _mm_load_si128(&s128[2]));
	_mm_store_si128(&d128[3], _mm_load_si128(&s128[3]));
#else
	*dst = *src;
#endif
}

static void
nvme_pcie_qpair_submit_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	struct nvme_request	*req;

	req = tr->req;
	qpair->tr[tr->cid].active = true;

	/* Copy the command from the tracker to the submission queue. */
	nvme_pcie_copy_command(&qpair->cmd[qpair->sq_tail], &req->cmd);

	if (++qpair->sq_tail == qpair->num_entries) {
		qpair->sq_tail = 0;
	}

	spdk_wmb();
	spdk_mmio_write_4(qpair->sq_tdbl, qpair->sq_tail);
}

static void
nvme_pcie_qpair_complete_tracker(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr,
				 struct spdk_nvme_cpl *cpl, bool print_on_error)
{
	struct nvme_request	*req;
	bool			retry, error, was_active;

	req = tr->req;

	assert(req != NULL);

	error = spdk_nvme_cpl_is_error(cpl);
	retry = error && nvme_completion_is_retry(cpl) &&
		req->retries < spdk_nvme_retry_count;

	if (error && print_on_error) {
		nvme_qpair_print_command(qpair, &req->cmd);
		nvme_qpair_print_completion(qpair, cpl);
	}

	was_active = qpair->tr[cpl->cid].active;
	qpair->tr[cpl->cid].active = false;

	assert(cpl->cid == req->cmd.cid);

	if (retry) {
		req->retries++;
		nvme_pcie_qpair_submit_tracker(qpair, tr);
	} else {
		if (was_active && req->cb_fn) {
			req->cb_fn(req->cb_arg, cpl);
		}

		nvme_free_request(req);
		tr->req = NULL;

		LIST_REMOVE(tr, list);
		LIST_INSERT_HEAD(&qpair->free_tr, tr, list);

		/*
		 * If the controller is in the middle of resetting, don't
		 *  try to submit queued requests here - let the reset logic
		 *  handle that instead.
		 */
		if (!STAILQ_EMPTY(&qpair->queued_req) &&
		    !qpair->ctrlr->is_resetting) {
			req = STAILQ_FIRST(&qpair->queued_req);
			STAILQ_REMOVE_HEAD(&qpair->queued_req, stailq);
			nvme_qpair_submit_request(qpair, req);
		}
	}
}

static void
nvme_pcie_qpair_manual_complete_tracker(struct spdk_nvme_qpair *qpair,
					struct nvme_tracker *tr, uint32_t sct, uint32_t sc, uint32_t dnr,
					bool print_on_error)
{
	struct spdk_nvme_cpl	cpl;

	memset(&cpl, 0, sizeof(cpl));
	cpl.sqid = qpair->id;
	cpl.cid = tr->cid;
	cpl.status.sct = sct;
	cpl.status.sc = sc;
	cpl.status.dnr = dnr;
	nvme_pcie_qpair_complete_tracker(qpair, tr, &cpl, print_on_error);
}

static void
nvme_pcie_admin_qpair_abort_aers(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tracker	*tr;

	tr = LIST_FIRST(&qpair->outstanding_tr);
	while (tr != NULL) {
		assert(tr->req != NULL);
		if (tr->req->cmd.opc == SPDK_NVME_OPC_ASYNC_EVENT_REQUEST) {
			nvme_pcie_qpair_manual_complete_tracker(qpair, tr,
								SPDK_NVME_SCT_GENERIC, SPDK_NVME_SC_ABORTED_SQ_DELETION, 0,
								false);
			tr = LIST_FIRST(&qpair->outstanding_tr);
		} else {
			tr = LIST_NEXT(tr, list);
		}
	}
}

static void
nvme_pcie_admin_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

static void
nvme_pcie_qpair_destroy(struct spdk_nvme_qpair *qpair)
{
	if (nvme_qpair_is_admin_queue(qpair)) {
		nvme_pcie_admin_qpair_destroy(qpair);
	}
	if (qpair->cmd && !qpair->sq_in_cmb) {
		spdk_free(qpair->cmd);
		qpair->cmd = NULL;
	}
	if (qpair->cpl) {
		spdk_free(qpair->cpl);
		qpair->cpl = NULL;
	}
	if (qpair->tr) {
		spdk_free(qpair->tr);
		qpair->tr = NULL;
	}
}

static void
nvme_pcie_admin_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tracker *tr, *temp;

	/*
	 * Manually abort each outstanding admin command.  Do not retry
	 *  admin commands found here, since they will be left over from
	 *  a controller reset and its likely the context in which the
	 *  command was issued no longer applies.
	 */
	LIST_FOREACH_SAFE(tr, &qpair->outstanding_tr, list, temp) {
		SPDK_ERRLOG("aborting outstanding admin command\n");
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, 1 /* do not retry */, true);
	}
}

static void
nvme_pcie_io_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tracker *tr, *temp;

	/* Manually abort each outstanding I/O. */
	LIST_FOREACH_SAFE(tr, &qpair->outstanding_tr, list, temp) {
		SPDK_ERRLOG("aborting outstanding i/o\n");
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, 0, true);
	}
}

static void
nvme_pcie_qpair_enable(struct spdk_nvme_qpair *qpair)
{
	if (nvme_qpair_is_io_queue(qpair)) {
		nvme_pcie_io_qpair_enable(qpair);
	} else {
		nvme_pcie_admin_qpair_enable(qpair);
	}
}

static void
nvme_pcie_admin_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	nvme_pcie_admin_qpair_abort_aers(qpair);
}

static void
nvme_pcie_io_qpair_disable(struct spdk_nvme_qpair *qpair)
{
}

static void
nvme_pcie_qpair_disable(struct spdk_nvme_qpair *qpair)
{
	qpair->is_enabled = false;
	if (nvme_qpair_is_io_queue(qpair)) {
		nvme_pcie_io_qpair_disable(qpair);
	} else {
		nvme_pcie_admin_qpair_disable(qpair);
	}
}


static void
nvme_pcie_qpair_fail(struct spdk_nvme_qpair *qpair)
{
	struct nvme_tracker		*tr;

	/* Manually abort each outstanding I/O. */
	while (!LIST_EMPTY(&qpair->outstanding_tr)) {
		tr = LIST_FIRST(&qpair->outstanding_tr);
		/*
		 * Do not remove the tracker.  The abort_tracker path will
		 *  do that for us.
		 */
		SPDK_ERRLOG("failing outstanding i/o\n");
		nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
							SPDK_NVME_SC_ABORTED_BY_REQUEST, 1 /* do not retry */, true);
	}
}

static int
nvme_pcie_ctrlr_cmd_create_io_cq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn,
				 void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_CQ;

	/*
	 * TODO: create a create io completion queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/*
	 * 0x2 = interrupts enabled
	 * 0x1 = physically contiguous
	 */
	cmd->cdw11 = 0x1;
	cmd->dptr.prp.prp1 = io_que->cpl_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_create_io_sq(struct spdk_nvme_ctrlr *ctrlr,
				 struct spdk_nvme_qpair *io_que, spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_CREATE_IO_SQ;

	/*
	 * TODO: create a create io submission queue command data
	 *  structure.
	 */
	cmd->cdw10 = ((io_que->num_entries - 1) << 16) | io_que->id;
	/* 0x1 = physically contiguous */
	cmd->cdw11 = (io_que->id << 16) | (io_que->qprio << 1) | 0x1;
	cmd->dptr.prp.prp1 = io_que->cmd_bus_addr;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_delete_io_cq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_CQ;
	cmd->cdw10 = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_cmd_delete_io_sq(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair,
				 spdk_nvme_cmd_cb cb_fn, void *cb_arg)
{
	struct nvme_request *req;
	struct spdk_nvme_cmd *cmd;

	req = nvme_allocate_request_null(cb_fn, cb_arg);
	if (req == NULL) {
		return -ENOMEM;
	}

	cmd = &req->cmd;
	cmd->opc = SPDK_NVME_OPC_DELETE_IO_SQ;
	cmd->cdw10 = qpair->id;

	return nvme_ctrlr_submit_admin_request(ctrlr, req);
}

static int
nvme_pcie_ctrlr_create_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status	status;
	int rc;

	assert(ctrlr != NULL);
	assert(qpair != NULL);

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_create_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_create_io_cq failed!\n");
		return -1;
	}

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_create_io_sq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}

	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		SPDK_ERRLOG("nvme_create_io_sq failed!\n");
		/* Attempt to delete the completion queue */
		status.done = false;
		rc = nvme_pcie_ctrlr_cmd_delete_io_cq(qpair->ctrlr, qpair, nvme_completion_poll_cb, &status);
		if (rc != 0) {
			return -1;
		}
		while (status.done == false) {
			spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
		}
		return -1;
	}

	nvme_pcie_qpair_reset(qpair);

	return 0;
}

static int
nvme_pcie_ctrlr_delete_io_qpair(struct spdk_nvme_ctrlr *ctrlr, struct spdk_nvme_qpair *qpair)
{
	struct nvme_completion_poll_status status;
	int rc;

	assert(ctrlr != NULL);
	assert(qpair != NULL);

	/* Delete the I/O submission queue and then the completion queue */

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_delete_io_sq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

	status.done = false;
	rc = nvme_pcie_ctrlr_cmd_delete_io_cq(ctrlr, qpair, nvme_completion_poll_cb, &status);
	if (rc != 0) {
		return rc;
	}
	while (status.done == false) {
		spdk_nvme_qpair_process_completions(&ctrlr->adminq, 0);
	}
	if (spdk_nvme_cpl_is_error(&status.cpl)) {
		return -1;
	}

	return 0;
}

static void
nvme_pcie_fail_request_bad_vtophys(struct spdk_nvme_qpair *qpair, struct nvme_tracker *tr)
{
	/*
	 * Bad vtophys translation, so abort this request and return
	 *  immediately.
	 */
	nvme_pcie_qpair_manual_complete_tracker(qpair, tr, SPDK_NVME_SCT_GENERIC,
						SPDK_NVME_SC_INVALID_FIELD,
						1 /* do not retry */, true);
}

/**
 * Build PRP list describing physically contiguous payload buffer.
 */
static int
nvme_pcie_qpair_build_contig_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr)
{
	uint64_t phys_addr;
	void *seg_addr;
	uint32_t nseg, cur_nseg, modulo, unaligned;
	void *md_payload;
	void *payload = req->payload.u.contig + req->payload_offset;

	phys_addr = spdk_vtophys(payload);
	if (phys_addr == SPDK_VTOPHYS_ERROR) {
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
		return -1;
	}
	nseg = req->payload_size >> nvme_u32log2(PAGE_SIZE);
	modulo = req->payload_size & (PAGE_SIZE - 1);
	unaligned = phys_addr & (PAGE_SIZE - 1);
	if (modulo || unaligned) {
		nseg += 1 + ((modulo + unaligned - 1) >> nvme_u32log2(PAGE_SIZE));
	}

	if (req->payload.md) {
		md_payload = req->payload.md + req->md_offset;
		tr->req->cmd.mptr = spdk_vtophys(md_payload);
		if (tr->req->cmd.mptr == SPDK_VTOPHYS_ERROR) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}
	}

	tr->req->cmd.psdt = SPDK_NVME_PSDT_PRP;
	tr->req->cmd.dptr.prp.prp1 = phys_addr;
	if (nseg == 2) {
		seg_addr = payload + PAGE_SIZE - unaligned;
		tr->req->cmd.dptr.prp.prp2 = spdk_vtophys(seg_addr);
	} else if (nseg > 2) {
		cur_nseg = 1;
		tr->req->cmd.dptr.prp.prp2 = (uint64_t)tr->prp_sgl_bus_addr;
		while (cur_nseg < nseg) {
			seg_addr = payload + cur_nseg * PAGE_SIZE - unaligned;
			phys_addr = spdk_vtophys(seg_addr);
			if (phys_addr == SPDK_VTOPHYS_ERROR) {
				nvme_pcie_fail_request_bad_vtophys(qpair, tr);
				return -1;
			}
			tr->u.prp[cur_nseg - 1] = phys_addr;
			cur_nseg++;
		}
	}

	return 0;
}

/**
 * Build SGL list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_hw_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				     struct nvme_tracker *tr)
{
	int rc;
	uint64_t phys_addr;
	uint32_t remaining_transfer_len, length;
	struct spdk_nvme_sgl_descriptor *sgl;
	uint32_t nseg = 0;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload_size != 0);
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	assert(req->payload.u.sgl.next_sge_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	sgl = tr->u.sgl;
	req->cmd.psdt = SPDK_NVME_PSDT_SGL_MPTR_SGL;
	req->cmd.dptr.sgl1.unkeyed.subtype = 0;

	remaining_transfer_len = req->payload_size;

	while (remaining_transfer_len > 0) {
		if (nseg >= NVME_MAX_SGL_DESCRIPTORS) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &phys_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		length = nvme_min(remaining_transfer_len, length);
		remaining_transfer_len -= length;

		sgl->unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		sgl->unkeyed.length = length;
		sgl->address = phys_addr;
		sgl->unkeyed.subtype = 0;

		sgl++;
		nseg++;
	}

	if (nseg == 1) {
		/*
		 * The whole transfer can be described by a single SGL descriptor.
		 *  Use the special case described by the spec where SGL1's type is Data Block.
		 *  This means the SGL in the tracker is not used at all, so copy the first (and only)
		 *  SGL element into SGL1.
		 */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_DATA_BLOCK;
		req->cmd.dptr.sgl1.address = tr->u.sgl[0].address;
		req->cmd.dptr.sgl1.unkeyed.length = tr->u.sgl[0].unkeyed.length;
	} else {
		/* For now we can only support 1 SGL segment in NVMe controller */
		req->cmd.dptr.sgl1.unkeyed.type = SPDK_NVME_SGL_TYPE_LAST_SEGMENT;
		req->cmd.dptr.sgl1.address = tr->prp_sgl_bus_addr;
		req->cmd.dptr.sgl1.unkeyed.length = nseg * sizeof(struct spdk_nvme_sgl_descriptor);
	}

	return 0;
}

/**
 * Build PRP list describing scattered payload buffer.
 */
static int
nvme_pcie_qpair_build_prps_sgl_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req,
				       struct nvme_tracker *tr)
{
	int rc;
	uint64_t phys_addr;
	uint32_t data_transferred, remaining_transfer_len, length;
	uint32_t nseg, cur_nseg, total_nseg, last_nseg, modulo, unaligned;
	uint32_t sge_count = 0;
	uint64_t prp2 = 0;

	/*
	 * Build scattered payloads.
	 */
	assert(req->payload.type == NVME_PAYLOAD_TYPE_SGL);
	assert(req->payload.u.sgl.reset_sgl_fn != NULL);
	req->payload.u.sgl.reset_sgl_fn(req->payload.u.sgl.cb_arg, req->payload_offset);

	remaining_transfer_len = req->payload_size;
	total_nseg = 0;
	last_nseg = 0;

	while (remaining_transfer_len > 0) {
		assert(req->payload.u.sgl.next_sge_fn != NULL);
		rc = req->payload.u.sgl.next_sge_fn(req->payload.u.sgl.cb_arg, &phys_addr, &length);
		if (rc) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		/* Confirm that this sge is prp compatible. */
		if (phys_addr & 0x3 ||
		    (length < remaining_transfer_len && ((phys_addr + length) & (PAGE_SIZE - 1)))) {
			nvme_pcie_fail_request_bad_vtophys(qpair, tr);
			return -1;
		}

		data_transferred = nvme_min(remaining_transfer_len, length);

		nseg = data_transferred >> nvme_u32log2(PAGE_SIZE);
		modulo = data_transferred & (PAGE_SIZE - 1);
		unaligned = phys_addr & (PAGE_SIZE - 1);
		if (modulo || unaligned) {
			nseg += 1 + ((modulo + unaligned - 1) >> nvme_u32log2(PAGE_SIZE));
		}

		if (total_nseg == 0) {
			req->cmd.psdt = SPDK_NVME_PSDT_PRP;
			req->cmd.dptr.prp.prp1 = phys_addr;
			phys_addr -= unaligned;
		}

		total_nseg += nseg;
		sge_count++;
		remaining_transfer_len -= data_transferred;

		if (total_nseg == 2) {
			if (sge_count == 1)
				tr->req->cmd.dptr.prp.prp2 = phys_addr + PAGE_SIZE;
			else if (sge_count == 2)
				tr->req->cmd.dptr.prp.prp2 = phys_addr;
			/* save prp2 value */
			prp2 = tr->req->cmd.dptr.prp.prp2;
		} else if (total_nseg > 2) {
			if (sge_count == 1)
				cur_nseg = 1;
			else
				cur_nseg = 0;

			tr->req->cmd.dptr.prp.prp2 = (uint64_t)tr->prp_sgl_bus_addr;
			while (cur_nseg < nseg) {
				if (prp2) {
					tr->u.prp[0] = prp2;
					tr->u.prp[last_nseg + 1] = phys_addr + cur_nseg * PAGE_SIZE;
				} else
					tr->u.prp[last_nseg] = phys_addr + cur_nseg * PAGE_SIZE;

				last_nseg++;
				cur_nseg++;
			}
		}
	}

	return 0;
}

static inline bool
nvme_pcie_qpair_check_enabled(struct spdk_nvme_qpair *qpair)
{
	if (!qpair->is_enabled &&
	    !qpair->ctrlr->is_resetting) {
		nvme_qpair_enable(qpair);
	}
	return qpair->is_enabled;
}

static int
nvme_pcie_qpair_submit_request(struct spdk_nvme_qpair *qpair, struct nvme_request *req)
{
	struct nvme_tracker *tr;
	int rc;
	struct spdk_nvme_ctrlr *ctrlr = qpair->ctrlr;

	nvme_pcie_qpair_check_enabled(qpair);

	tr = LIST_FIRST(&qpair->free_tr);

	if (tr == NULL || !qpair->is_enabled) {
		/*
		 * No tracker is available, or the qpair is disabled due to
		 *  an in-progress controller-level reset.
		 *
		 * Put the request on the qpair's request queue to be
		 *  processed when a tracker frees up via a command
		 *  completion or when the controller reset is
		 *  completed.
		 */
		STAILQ_INSERT_TAIL(&qpair->queued_req, req, stailq);
		return 0;
	}

	LIST_REMOVE(tr, list); /* remove tr from free_tr */
	LIST_INSERT_HEAD(&qpair->outstanding_tr, tr, list);
	tr->req = req;
	req->cmd.cid = tr->cid;

	if (req->payload_size == 0) {
		/* Null payload - leave PRP fields zeroed */
		rc = 0;
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_CONTIG) {
		rc = nvme_pcie_qpair_build_contig_request(qpair, req, tr);
	} else if (req->payload.type == NVME_PAYLOAD_TYPE_SGL) {
		if (ctrlr->flags & SPDK_NVME_CTRLR_SGL_SUPPORTED) {
			rc = nvme_pcie_qpair_build_hw_sgl_request(qpair, req, tr);
		} else {
			rc = nvme_pcie_qpair_build_prps_sgl_request(qpair, req, tr);
		}
	} else {
		assert(0);
		nvme_pcie_fail_request_bad_vtophys(qpair, tr);
		rc = -EINVAL;
	}

	if (rc < 0) {
		return rc;
	}

	nvme_pcie_qpair_submit_tracker(qpair, tr);
	return 0;
}

static int32_t
nvme_pcie_qpair_process_completions(struct spdk_nvme_qpair *qpair, uint32_t max_completions)
{
	struct nvme_tracker	*tr;
	struct spdk_nvme_cpl	*cpl;
	uint32_t num_completions = 0;

	if (!nvme_pcie_qpair_check_enabled(qpair)) {
		/*
		 * qpair is not enabled, likely because a controller reset is
		 *  is in progress.  Ignore the interrupt - any I/O that was
		 *  associated with this interrupt will get retried when the
		 *  reset is complete.
		 */
		return 0;
	}

	if (max_completions == 0 || (max_completions > (qpair->num_entries - 1U))) {

		/*
		 * max_completions == 0 means unlimited, but complete at most one
		 * queue depth batch of I/O at a time so that the completion
		 * queue doorbells don't wrap around.
		 */
		max_completions = qpair->num_entries - 1;
	}

	while (1) {
		cpl = &qpair->cpl[qpair->cq_head];

		if (cpl->status.p != qpair->phase)
			break;

		tr = &qpair->tr[cpl->cid];

		if (tr->active) {
			nvme_pcie_qpair_complete_tracker(qpair, tr, cpl, true);
		} else {
			SPDK_ERRLOG("cpl does not map to outstanding cmd\n");
			nvme_qpair_print_completion(qpair, cpl);
			assert(0);
		}

		if (++qpair->cq_head == qpair->num_entries) {
			qpair->cq_head = 0;
			qpair->phase = !qpair->phase;
		}

		if (++num_completions == max_completions) {
			break;
		}
	}

	if (num_completions > 0) {
		spdk_mmio_write_4(qpair->cq_hdbl, qpair->cq_head);
	}

	return num_completions;
}

const struct spdk_nvme_transport spdk_nvme_transport_pcie = {
	.ctrlr_size = sizeof(struct nvme_pcie_ctrlr),

	.ctrlr_construct = nvme_pcie_ctrlr_construct,
	.ctrlr_destruct = nvme_pcie_ctrlr_destruct,

	.ctrlr_get_pci_id = nvme_pcie_ctrlr_get_pci_id,

	.ctrlr_set_reg_4 = nvme_pcie_ctrlr_set_reg_4,
	.ctrlr_set_reg_8 = nvme_pcie_ctrlr_set_reg_8,

	.ctrlr_get_reg_4 = nvme_pcie_ctrlr_get_reg_4,
	.ctrlr_get_reg_8 = nvme_pcie_ctrlr_get_reg_8,

	.ctrlr_create_io_qpair = nvme_pcie_ctrlr_create_io_qpair,
	.ctrlr_delete_io_qpair = nvme_pcie_ctrlr_delete_io_qpair,

	.qpair_construct = nvme_pcie_qpair_construct,
	.qpair_destroy = nvme_pcie_qpair_destroy,

	.qpair_enable = nvme_pcie_qpair_enable,
	.qpair_disable = nvme_pcie_qpair_disable,

	.qpair_reset = nvme_pcie_qpair_reset,
	.qpair_fail = nvme_pcie_qpair_fail,

	.qpair_submit_request = nvme_pcie_qpair_submit_request,
	.qpair_process_completions = nvme_pcie_qpair_process_completions,
};
