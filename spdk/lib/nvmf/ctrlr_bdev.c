/*   SPDX-License-Identifier: BSD-3-Clause
 *   Copyright (C) 2017 Intel Corporation. All rights reserved.
 *   Copyright (c) 2019 Mellanox Technologies LTD. All rights reserved.
 *   Copyright (c) 2021 NVIDIA CORPORATION & AFFILIATES. All rights reserved.
 */

#include "spdk/stdinc.h"

#include "nvmf_internal.h"
#include "nvmf_image.h"

#include "spdk/bdev.h"
#include "spdk/endian.h"
#include "spdk/thread.h"
#include "spdk/likely.h"
#include "spdk/nvme.h"
#include "spdk/nvme_spec.h"
#include "spdk/nvmf_cmd.h"
#include "spdk/nvmf_spec.h"
#include "spdk/trace.h"
#include "spdk/scsi_spec.h"
#include "spdk/string.h"
#include "spdk/util.h"
#include "spdk/env.h"

#include "spdk/log.h"

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libavutil/imgutils.h>



struct custom_grep_ctx {
    struct spdk_nvmf_request *req;
    char *buffer;
    size_t buffer_len;
};

void dump_hex(const char *, const void *, size_t );

// static void nvmf_print_iov(const struct spdk_nvmf_request *req, uint32_t data_len)
// {
//     if (req->iovcnt == 0) {
//         return;
//     }

//     // Print first iovec info
//     fprintf(stdout, "First iovec base: %p, len: %zu\n", req->iov[0].iov_base, req->iov[0].iov_len);

//     // Print data content
//     fprintf(stdout, "Data content: ");
//     uint32_t total_copied = 0;

//     for (int i = 0; i < req->iovcnt && total_copied < data_len; i++) {
//         char *data = (char *)req->iov[i].iov_base;
//         uint32_t iov_len = req->iov[i].iov_len;
//         uint32_t copy_len = spdk_min(data_len - total_copied, iov_len);

//         for (uint32_t j = 0; j < copy_len; j++) {
//             fprintf(stdout, "%c", data[j]);
//         }
//         total_copied += copy_len;
//     }
//     fprintf(stdout, "\n");
// }

static bool
nvmf_subsystem_bdev_io_type_supported(struct spdk_nvmf_subsystem *subsystem,
				      enum spdk_bdev_io_type io_type)
{
	struct spdk_nvmf_ns *ns;

	for (ns = spdk_nvmf_subsystem_get_first_ns(subsystem); ns != NULL;
	     ns = spdk_nvmf_subsystem_get_next_ns(subsystem, ns)) {
		if (ns->bdev == NULL) {
			continue;
		}

		if (!spdk_bdev_io_type_supported(ns->bdev, io_type)) {
			SPDK_DEBUGLOG(nvmf,
				      "Subsystem %s namespace %u (%s) does not support io_type %d\n",
				      spdk_nvmf_subsystem_get_nqn(subsystem),
				      ns->opts.nsid, spdk_bdev_get_name(ns->bdev), (int)io_type);
			return false;
		}
	}

	SPDK_DEBUGLOG(nvmf, "All devices in Subsystem %s support io_type %d\n",
		      spdk_nvmf_subsystem_get_nqn(subsystem), (int)io_type);
	return true;
}

bool
nvmf_ctrlr_dsm_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_UNMAP);
}

bool
nvmf_ctrlr_write_zeroes_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_WRITE_ZEROES);
}

bool
nvmf_ctrlr_copy_supported(struct spdk_nvmf_ctrlr *ctrlr)
{
	return nvmf_subsystem_bdev_io_type_supported(ctrlr->subsys, SPDK_BDEV_IO_TYPE_COPY);
}

static void
nvmf_bdev_ctrlr_complete_cmd_custom(struct spdk_bdev_io *bdev_io, bool success,
                             void *cb_arg)
{
    struct custom_grep_ctx {
        char *keyword;
        uint32_t keyword_len;
        struct spdk_nvmf_request *req;
    };

    struct custom_grep_ctx *ctx = cb_arg;
    struct spdk_nvmf_request    *req = ctx->req;
    struct spdk_nvme_cpl        *response = &req->rsp->nvme_cpl;

    int    sc = 0, sct = 0;
    uint32_t cdw0 = 0;

    struct iovec *iovs;
    int iovcnt = 0;
    spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);

    if (iovcnt > 0) {
        // 결과 저장을 위한 버퍼
        char *result_buffer = NULL;
        size_t result_size = 0;
        size_t result_capacity = 4096;  // 초기 버퍼 크기
        result_buffer = malloc(result_capacity);

        if (!result_buffer) {
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto complete;
        }

        // 키워드의 개행문자 제거
        size_t keyword_len = strlen(ctx->keyword);
        if (keyword_len > 0 && ctx->keyword[keyword_len - 1] == '\n') {
            ctx->keyword[keyword_len - 1] = '\0';
            keyword_len--;
        }

        for (int i = 0; i < iovcnt; i++) {
            char *data = iovs[i].iov_base;
            size_t len = iovs[i].iov_len;
            size_t start = 0;

            // 각 라인 처리
            for (size_t j = 0; j < len; j++) {
                if (data[j] == '\n' || j == len - 1) {
                    size_t line_len = j - start + 1;
                    char line[line_len + 1];
                    memcpy(line, &data[start], line_len);
                    line[line_len] = '\0';

                    // 키워드 검색
                    if (strstr(line, ctx->keyword)) {
                        size_t needed_size = result_size + line_len + 1;
                        if (needed_size > result_capacity) {
                            result_capacity *= 2;
                            char *new_buffer = realloc(result_buffer, result_capacity);
                            if (!new_buffer) {
                                free(result_buffer);
                                sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
                                goto complete;
                            }
                            result_buffer = new_buffer;
                        }
                        memcpy(result_buffer + result_size, line, line_len);
                        result_buffer[result_size + line_len] = '\n';
                        result_size += line_len + 1;
                    }
                    start = j + 1;
                }
            }
        }

        if (result_size > 0) {
            // 결과가 있는 경우
            result_buffer[result_size - 1] = '\0';  // 마지막 개행문자 제거
            spdk_iov_memset(req->iov, req->iovcnt, 0);  // iov 초기화
            spdk_copy_buf_to_iovs(req->iov, req->iovcnt, result_buffer, result_size);

            req->xfer = SPDK_NVME_DATA_CONTROLLER_TO_HOST;
            req->length = 4096;
            sc = SPDK_NVME_SC_SUCCESS;
        } else {
            // 결과가 없는 경우
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            req->length = 0;
        }

        free(result_buffer);

        } else {
            sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        }

complete:
    response->cdw0 = cdw0;
    response->status.sc = sc;
    response->status.sct = sct;

    free(ctx);

    spdk_nvmf_request_complete(req);
    spdk_bdev_free_io(bdev_io);
}

static void
nvmf_bdev_ctrlr_complete_cmd(struct spdk_bdev_io *bdev_io, bool success,
			     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				sc = 0, sct = 0;
	uint32_t			cdw0 = 0;

	// iov 내에있는 데이터 확인용 코드
//        uint32_t tc = 0;
//        uint32_t data_len = 512;
//
//        struct iovec *iovs;
//        int iovcnt = 0;
//        spdk_bdev_io_get_iovec(bdev_io, &iovs, &iovcnt);
//
//        if (iovcnt > 0) {
//            fprintf(stdout, "First iovec base: %p, len: %zu\n", iovs[0].iov_base, iovs[0].iov_len);
//            // 데이터 출력
//            uint32_t tc = 0;
//            fprintf(stdout, "Data content: ");
//            for (int i = 0; i < iovcnt && tc < data_len; i++) {
//                char *data = (char *)iovs[i].iov_base;
//                uint32_t iov_len = iovs[i].iov_len;
//                uint32_t copy_len = spdk_min(data_len - tc, iov_len);
//                for (uint32_t j = 0; j < copy_len; j++) {
//                    fprintf(stdout, "%c", data[j]);
//                }
//                tc += copy_len;
//            }
//            fprintf(stdout, "\n");
//        }
        //

	if (spdk_unlikely(req->first_fused)) {
		struct spdk_nvmf_request	*first_req = req->first_fused_req;
		struct spdk_nvme_cpl		*first_response = &first_req->rsp->nvme_cpl;
		int				first_sc = 0, first_sct = 0;

		/* get status for both operations */
		spdk_bdev_io_get_nvme_fused_status(bdev_io, &cdw0, &first_sct, &first_sc, &sct, &sc);
		first_response->cdw0 = cdw0;
		first_response->status.sc = first_sc;
		first_response->status.sct = first_sct;

		/* first request should be completed */
		spdk_nvmf_request_complete(first_req);
		req->first_fused_req = NULL;
		req->first_fused = false;
	} else {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
	}

	response->cdw0 = cdw0;
	response->status.sc = sc;
	response->status.sct = sct;

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

static void
nvmf_bdev_ctrlr_complete_admin_cmd(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;

	if (req->cmd_cb_fn) {
		req->cmd_cb_fn(req);
	}

	nvmf_bdev_ctrlr_complete_cmd(bdev_io, success, req);
}

void
nvmf_bdev_ctrlr_identify_ns(struct spdk_nvmf_ns *ns, struct spdk_nvme_ns_data *nsdata,
			    bool dif_insert_or_strip)
{
	struct spdk_bdev *bdev = ns->bdev;
	uint64_t num_blocks;
	uint32_t phys_blocklen;
	uint32_t max_copy;

	num_blocks = spdk_bdev_get_num_blocks(bdev);

	nsdata->nsze = num_blocks;
	nsdata->ncap = num_blocks;
	nsdata->nuse = num_blocks;
	nsdata->nlbaf = 0;
	nsdata->flbas.format = 0;
	nsdata->flbas.msb_format = 0;
	nsdata->nacwu = spdk_bdev_get_acwu(bdev) - 1; /* nacwu is 0-based */
	if (!dif_insert_or_strip) {
		nsdata->lbaf[0].ms = spdk_bdev_get_md_size(bdev);
		nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_block_size(bdev));
		if (nsdata->lbaf[0].ms != 0) {
			nsdata->flbas.extended = 1;
			nsdata->mc.extended = 1;
			nsdata->mc.pointer = 0;
			nsdata->dps.md_start = spdk_bdev_is_dif_head_of_md(bdev);
			/* NVMf library doesn't process PRACT and PRCHK flags, we
			 * leave the use of extended LBA buffer to users.
			 */
			nsdata->dps.pit = SPDK_NVME_FMT_NVM_PROTECTION_DISABLE;
		}
	} else {
		nsdata->lbaf[0].ms = 0;
		nsdata->lbaf[0].lbads = spdk_u32log2(spdk_bdev_get_data_block_size(bdev));
	}

	phys_blocklen = spdk_bdev_get_physical_block_size(bdev);
	assert(phys_blocklen > 0);
	/* Linux driver uses min(nawupf, npwg) to set physical_block_size */
	nsdata->nsfeat.optperf = 1;
	nsdata->nsfeat.ns_atomic_write_unit = 1;
	nsdata->npwg = (phys_blocklen >> nsdata->lbaf[0].lbads) - 1;
	nsdata->nawupf = nsdata->npwg;
	nsdata->npwa = nsdata->npwg;
	nsdata->npdg = nsdata->npwg;
	nsdata->npda = nsdata->npwg;

	if (spdk_bdev_get_write_unit_size(bdev) == 1) {
		nsdata->noiob = spdk_bdev_get_optimal_io_boundary(bdev);
	}
	nsdata->nmic.can_share = 1;
	if (nvmf_ns_is_ptpl_capable(ns)) {
		nsdata->nsrescap.rescap.persist = 1;
	}
	nsdata->nsrescap.rescap.write_exclusive = 1;
	nsdata->nsrescap.rescap.exclusive_access = 1;
	nsdata->nsrescap.rescap.write_exclusive_reg_only = 1;
	nsdata->nsrescap.rescap.exclusive_access_reg_only = 1;
	nsdata->nsrescap.rescap.write_exclusive_all_reg = 1;
	nsdata->nsrescap.rescap.exclusive_access_all_reg = 1;
	nsdata->nsrescap.rescap.ignore_existing_key = 1;

	SPDK_STATIC_ASSERT(sizeof(nsdata->nguid) == sizeof(ns->opts.nguid), "size mismatch");
	memcpy(nsdata->nguid, ns->opts.nguid, sizeof(nsdata->nguid));

	SPDK_STATIC_ASSERT(sizeof(nsdata->eui64) == sizeof(ns->opts.eui64), "size mismatch");
	memcpy(&nsdata->eui64, ns->opts.eui64, sizeof(nsdata->eui64));

	/* For now we support just one source range for copy command */
	nsdata->msrc = 0;

	max_copy = spdk_bdev_get_max_copy(bdev);
	if (max_copy == 0 || max_copy > UINT16_MAX) {
		/* Zero means copy size is unlimited */
		nsdata->mcl = UINT16_MAX;
		nsdata->mssrl = UINT16_MAX;
	} else {
		nsdata->mcl = max_copy;
		nsdata->mssrl = max_copy;
	}
}

static void
nvmf_bdev_ctrlr_get_rw_params(const struct spdk_nvme_cmd *cmd, uint64_t *start_lba,
			      uint64_t *num_blocks)
{
	/* SLBA: CDW10 and CDW11 */
	*start_lba = from_le64(&cmd->cdw10);

	/* NLB: CDW12 bits 15:00, 0's based */
	*num_blocks = (from_le32(&cmd->cdw12) & 0xFFFFu) + 1;
}

static void
nvmf_bdev_ctrlr_get_rw_ext_params(const struct spdk_nvme_cmd *cmd,
				  struct spdk_bdev_ext_io_opts *opts)
{
	/* Get CDW12 values */
	opts->nvme_cdw12.raw = from_le32(&cmd->cdw12);

	/* Get CDW13 values */
	opts->nvme_cdw13.raw = from_le32(&cmd->cdw13);
}

static bool
nvmf_bdev_ctrlr_lba_in_range(uint64_t bdev_num_blocks, uint64_t io_start_lba,
			     uint64_t io_num_blocks)
{
	if (io_start_lba + io_num_blocks > bdev_num_blocks ||
	    io_start_lba + io_num_blocks < io_start_lba) {
		return false;
	}

	return true;
}

static void
nvmf_ctrlr_process_io_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	int rc;

	rc = nvmf_ctrlr_process_io_cmd(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

static void
nvmf_ctrlr_process_admin_cmd_resubmit(void *arg)
{
	struct spdk_nvmf_request *req = arg;
	int rc;

	rc = nvmf_ctrlr_process_admin_cmd(req);
	if (rc == SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE) {
		spdk_nvmf_request_complete(req);
	}
}

static void
nvmf_bdev_ctrl_queue_io(struct spdk_nvmf_request *req, struct spdk_bdev *bdev,
			struct spdk_io_channel *ch, spdk_bdev_io_wait_cb cb_fn, void *cb_arg)
{
	int rc;

	req->bdev_io_wait.bdev = bdev;
	req->bdev_io_wait.cb_fn = cb_fn;
	req->bdev_io_wait.cb_arg = cb_arg;

	rc = spdk_bdev_queue_io_wait(bdev, ch, &req->bdev_io_wait);
	if (rc != 0) {
		assert(false);
	}
	req->qpair->group->stat.pending_bdev_io++;
}

bool
nvmf_bdev_zcopy_enabled(struct spdk_bdev *bdev)
{
	return spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_ZCOPY);
}

int
nvmf_bdev_ctrlr_read_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_bdev_ext_io_opts opts = {
		.size = SPDK_SIZEOF(&opts, accel_sequence),
		.memory_domain = req->memory_domain,
		.memory_domain_ctx = req->memory_domain_ctx,
		.accel_sequence = req->accel_sequence,
	};
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	/* printing logs for debugging */
	/*
	fprintf(stdout, "Opcode: 0x%x\n", cmd->opc);
    fprintf(stdout, "req->iovcnt: %u\n", req->iovcnt);
    fprintf(stdout, "req->length: %u\n", req->length);
	*/

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	//nvmf_print_iov(req, req->length);

	// uint32_t meta_start_lba = cmd->cdw10; // 연산 메타데이터 파일의 LBA 시작 주소
    // uint32_t meta_block_count = cmd->cdw11; // 연산 메타데이터 파일의 블록 갯수
    // uint32_t target_start_lba = cmd->cdw12; // 연산 대상 파일의 LBA 시작 주소
    // uint32_t target_block_count = cmd->cdw13; // 연산 대상 파일의 블록 갯수
    // uint32_t target_block_cot = cmd->cdw14; // 연산 대상 파일의 블록 갯수

	/* printing logs for debugging */
	/*
	fprintf(stdout, " from CDW10: %u\n", meta_start_lba);
    fprintf(stdout, " from CDW11: %u\n", meta_block_count);
    fprintf(stdout, " from CDW12: %u\n", target_start_lba);
    fprintf(stdout, " from CDW13: %u\n", target_block_count);
    fprintf(stdout, " from CDW14: %u\n", target_block_cot);
	*/

//	fprintf(stdout, "start_lba: %u\n", start_lba);
//    fprintf(stdout, "num_blocks: %u\n", num_blocks);
//    fprintf(stdout, "SGL length: %u\n", req->length);

//    fprintf(stdout, "READ Opcode: 0x%x\n", cmd->opc);
//    fprintf(stdout, "req->length: %u\n", req->length);
//    fprintf(stdout, "req->iovcnt: %u\n", req->iovcnt);

//	for (int i = 0; i < req->iovcnt; i++) {
//        printf("nvmf_bdev_ctrlr_custom_grep_cmd sgl: req->iov[%d].iov_len: %zu\n", i, req->iov[i].iov_len);
//    }
//    fprintf(stdout, "req->xfer: %u\n", req->xfer);

//  uint32_t tc = 0;
//
//    if (req->iovcnt > 0) {
//        fprintf(stdout, "First iovec base: %p, len: %zu\n", req->iov[0].iov_base, req->iov[0].iov_len);
//
//        // 데이터 출력
//        uint32_t data_len = 4096;
//        uint32_t tc = 0;
//        fprintf(stdout, "Data content: ");
//        for (int i = 0; i < req->iovcnt && tc < data_len; i++) {
//            char *data = (char *)req->iov[i].iov_base;
//            uint32_t iov_len = req->iov[i].iov_len;
//            uint32_t copy_len = spdk_min(data_len - tc, iov_len);
//
//            for (uint32_t j = 0; j < copy_len; j++) {
//                fprintf(stdout, "%c", data[j]);
//            }
//
//            tc += copy_len;
//        }
//        fprintf(stdout, "\n");
//    }

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));

	rc = spdk_bdev_readv_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
					nvmf_bdev_ctrlr_complete_cmd, req, &opts);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_bdev_ext_io_opts opts = {
		.size = SPDK_SIZEOF(&opts, nvme_cdw13),
		.memory_domain = req->memory_domain,
		.memory_domain_ctx = req->memory_domain_ctx,
		.accel_sequence = req->accel_sequence,
	};
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

//	fprintf(stdout, "Opcode: 0x%x\n", cmd->opc);
//	fprintf(stdout, "req->iovcnt: %u\n", req->iovcnt);

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	nvmf_bdev_ctrlr_get_rw_ext_params(cmd, &opts);

//	fprintf(stdout, "start_lba: %u\n", req->iovcnt);
//	fprintf(stdout, "num_blocks: %u\n", num_blocks);
//	fprintf(stdout, "SGL length: %u\n", req->length);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	assert(!spdk_nvmf_request_using_zcopy(req));

	rc = spdk_bdev_writev_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
					 nvmf_bdev_ctrlr_complete_cmd, req, &opts);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}


int
nvmf_bdev_ctrlr_custom_grep_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
                                struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

    uint32_t data_len = cmd->cdw10 & 0xFFFF;  // 하위 16비트를 데이터 길이로 사용
    uint64_t start_lba = cmd->cdw11;
    uint64_t num_blocks = cmd->cdw12;

//    fprintf(stdout, "Custom grep command entered\n");
//    fprintf(stdout, "Opcode: 0x%x\n", cmd->opc);
//    fprintf(stdout, "Data length from CDW10: %u\n", data_len);
//    fprintf(stdout, "Start LBA from CDW11: %lu\n", start_lba);
//    fprintf(stdout, "Block Count from CDW12: %lu\n", num_blocks);
//    fprintf(stdout, "req->length: %u\n", req->length);
//    fprintf(stdout, "req->iovcnt: %u\n", req->iovcnt);

//	for (int i = 0; i < req->iovcnt; i++) {
//        printf("nvmf_bdev_ctrlr_custom_grep_cmd sgl: req->iov[%d].iov_len: %zu\n", i, req->iov[i].iov_len);
//    }
//    fprintf(stdout, "req->xfer: %u\n", req->xfer);

//     iov 내에있는 데이터 확인용 코드
//    uint32_t tc = 0;
//
//    if (req->iovcnt > 0) {
//        fprintf(stdout, "First iovec base: %p, len: %zu\n", req->iov[0].iov_base, req->iov[0].iov_len);
//
//        // 데이터 출력
//        uint32_t tc = 0;
//        fprintf(stdout, "Data content: ");
//        for (int i = 0; i < req->iovcnt && tc < data_len; i++) {
//            char *data = (char *)req->iov[i].iov_base;
//            uint32_t iov_len = req->iov[i].iov_len;
//            uint32_t copy_len = spdk_min(data_len - tc, iov_len);
//
//            for (uint32_t j = 0; j < copy_len; j++) {
//                fprintf(stdout, "%c", data[j]);
//            }
//
//            tc += copy_len;
//        }
//        fprintf(stdout, "\n");
//    }

    // 내부에 데이터가 없을때를 대비
    if (data_len > req->length || req->iovcnt == 0) {
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    // 새로운 버퍼 할당
    char *new_buffer = NULL;
    uint32_t total_len = 0;

    // req->iov의 총 길이 계산
    for (int i = 0; i < req->iovcnt; i++) {
        total_len += req->iov[i].iov_len;
    }

    // 새 버퍼 할당
    new_buffer = malloc(total_len);
    if (new_buffer == NULL) {
        fprintf(stderr, "Failed to allocate memory for new buffer\n");
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    // req->iov의 데이터를 새 버퍼로 복사
    uint32_t offset = 0;
    for (int i = 0; i < req->iovcnt; i++) {
        memcpy(new_buffer + offset, req->iov[i].iov_base, req->iov[i].iov_len);
        offset += req->iov[i].iov_len;
    }

    // 복사된 데이터 출력 (디버깅용)
//    fprintf(stdout, "Copied data content: ");
//    for (uint32_t i = 0; i < total_len; i++) {
//        fprintf(stdout, "%c", new_buffer[i]);
//    }
//    fprintf(stdout, "\n");

    // req->iov 초기화
    for (int i = 0; i < req->iovcnt; i++) {
        if (req->iov[i].iov_base) {
            memset(req->iov[i].iov_base, 0, req->iov[i].iov_len);
        }
    }

    struct spdk_bdev_ext_io_opts opts = {
        .size = SPDK_SIZEOF(&opts, accel_sequence),
        .memory_domain = req->memory_domain,
        .memory_domain_ctx = req->memory_domain_ctx,
        .accel_sequence = req->accel_sequence,
    };
    uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
    // uint32_t block_size = spdk_bdev_get_block_size(bdev);
    struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

    // 유효성 검사
    if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
        SPDK_ERRLOG("end of media\n");
        rsp->status.sct = SPDK_NVME_SCT_GENERIC;
        rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    struct custom_grep_ctx {
        char *buffer;
        uint32_t buffer_len;
        struct spdk_nvmf_request *req;
    };

    struct custom_grep_ctx *ctx = malloc(sizeof(struct custom_grep_ctx));
    if (ctx == NULL) {
        free(new_buffer);
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    ctx->buffer = new_buffer;
    ctx->buffer_len = total_len;
    ctx->req = req;

    int rc;
    rc = spdk_bdev_readv_blocks_ext(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
    					nvmf_bdev_ctrlr_complete_cmd_custom, ctx, &opts);
//    fprintf(stdout, "rc: %d\n", rc);

//    req->length = 4096;
//    free(ctx->buffer);
//    free(ctx);

    if (spdk_unlikely(rc)) {
        if (rc == -ENOMEM) {
            nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
            return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
        }
        rsp->status.sct = SPDK_NVME_SCT_GENERIC;
        rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct custom_ctx {
    char *first_result;
    uint32_t first_result_len;
    struct spdk_nvmf_request *req;
    struct spdk_bdev_desc *desc;
    struct spdk_io_channel *ch;
};

// 일반 데이터 출력 함수 추가
static void print_data_content(const char *data, uint32_t length) {
    fprintf(stdout, "Operation Meta Data content: ");
    for (uint32_t i = 0; i < length; i++) {
        fprintf(stdout, "%c", data[i]);
    }
    fprintf(stdout, "\n");
}

// 연산 결과를 위한 구조체
struct ndp_result {
    char *data;
    uint32_t length;
};

static struct ndp_result*
ndp_calculate(const char *ndp_meta_data, uint32_t ndp_meta_data_len,
              const char *ndp_target_data, uint32_t ndp_target_data_len)
{
    struct ndp_result *result;

    // 결과 구조체 할당
    result = spdk_malloc(sizeof(struct ndp_result),
                        0x1000, NULL,
                        SPDK_ENV_SOCKET_ID_ANY,
                        SPDK_MALLOC_DMA);
    if (!result) {
        return NULL;
    }

    // 전체 결과를 담을 버퍼 할당
    uint32_t total_length = ndp_meta_data_len + ndp_target_data_len;
    result->data = spdk_malloc(total_length,
                              0x1000, NULL,
                              SPDK_ENV_SOCKET_ID_ANY,
                              SPDK_MALLOC_DMA);
    if (!result->data) {
        spdk_free(result);
        return NULL;
    }

    // 두 결과를 연결
    memcpy(result->data, ndp_meta_data, ndp_meta_data_len);
    memcpy(result->data + ndp_meta_data_len, ndp_target_data, ndp_target_data_len);
    result->length = total_length;

    return result;
}

static void
nvmf_bdev_ctrlr_second_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct custom_ctx *ctx = cb_arg;
    struct spdk_nvmf_request *req = ctx->req;
    struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

    if (success) {
        // 두 번째 read 결과를 임시 버퍼에 복사
        char *second_result = spdk_malloc(req->length,
                                        0x1000, NULL,
                                        SPDK_ENV_SOCKET_ID_ANY,
                                        SPDK_MALLOC_DMA);
        if (!second_result) {
            response->status.sct = SPDK_NVME_SCT_GENERIC;
            response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto error;
        }

        // 두 번째 read 결과 복사
        uint32_t total_copied = 0;
        for (int i = 0; i < req->iovcnt && total_copied < req->length; i++) {
            uint32_t to_copy = spdk_min(req->length - total_copied, req->iov[i].iov_len);
            memcpy(second_result + total_copied, req->iov[i].iov_base, to_copy);
            total_copied += to_copy;
        }

        // 연산 수행
        struct ndp_result *calc_result = ndp_calculate(ctx->first_result,
                                                     ctx->first_result_len,
                                                     second_result,
                                                     total_copied);

        if (!calc_result) {
            spdk_free(second_result);
            response->status.sct = SPDK_NVME_SCT_GENERIC;
            response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
            goto error;
        }

        // 결과 출력
        fprintf(stdout, "Combined result (length: %u):\n", calc_result->length);
        print_data_content(calc_result->data, calc_result->length);

        spdk_iov_memset(req->iov, req->iovcnt, 0);

        // 결과를 req->iov에 복사하여 반환
        if (req->iovcnt > 0) {
            memcpy(req->iov[0].iov_base, calc_result->data,
                   spdk_min(calc_result->length, req->iov[0].iov_len));
        }

        // 임시 버퍼들 해제
        spdk_free(second_result);
        spdk_free(calc_result->data);
        spdk_free(calc_result);

        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_SUCCESS;
    } else {
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
    }

error:
    spdk_free(ctx->first_result);
    spdk_free(ctx);
    spdk_bdev_free_io(bdev_io);
    spdk_nvmf_request_complete(req);
}

// 첫 번째 read의 콜백 함수 - 두 번째 read 실행
static void
nvmf_bdev_ctrlr_first_read_complete(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct custom_ctx *ctx = cb_arg;
    struct spdk_nvmf_request *req = ctx->req;
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

    uint32_t target_start_lba = cmd->cdw12;
    uint32_t target_block_count = cmd->cdw13;
    int rc;

    if (!success) {
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        spdk_free(ctx->first_result);
        spdk_free(ctx);
        spdk_bdev_free_io(bdev_io);
        spdk_nvmf_request_complete(req);
        return;
    }

    // 첫 번째 read 결과 복사
    ctx->first_result_len = req->length;
    ctx->first_result = spdk_zmalloc(req->length,
                                    0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
                                    SPDK_MALLOC_DMA);
    if (!ctx->first_result) {
        fprintf(stdout, "!ctx->first_result?\n");
        spdk_free(ctx);
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        spdk_bdev_free_io(bdev_io);
        spdk_nvmf_request_complete(req);
        return;
    }

    // iov에서 데이터 복사
    uint32_t total_copied = 0;
    for (int i = 0; i < req->iovcnt && total_copied < req->length; i++) {
        uint32_t to_copy = spdk_min(req->length - total_copied, req->iov[i].iov_len);
        memcpy(ctx->first_result + total_copied, req->iov[i].iov_base, to_copy);
        total_copied += to_copy;
    }

    // 두 번째 read 실행
    struct spdk_bdev_ext_io_opts opts = {
        .size = SPDK_SIZEOF(&opts, accel_sequence),
        .memory_domain = req->memory_domain,
        .memory_domain_ctx = req->memory_domain_ctx,
        .accel_sequence = req->accel_sequence,
    };

    rc = spdk_bdev_readv_blocks_ext(ctx->desc, ctx->ch, req->iov, req->iovcnt,
                                   target_start_lba, target_block_count,
                                   nvmf_bdev_ctrlr_second_read_complete, ctx, &opts);
    fprintf(stdout, "rc complete\n");
    if (rc) {
        fprintf(stdout, "First Here?\n");
        spdk_free(ctx->first_result);
        spdk_free(ctx);
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        spdk_bdev_free_io(bdev_io);
//        spdk_nvmf_request_complete(req);
        return;
    }

    spdk_bdev_free_io(bdev_io);
}

int
nvmf_bdev_ctrlr_custom_echo_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
                                struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

    /* 연산 메타데이터 파일의 주소 범위, 연산 대상파일의 주소 범위를 받아 연산하는 드라이버 기능*/

    uint32_t meta_start_lba = cmd->cdw10; // 연산 메타데이터 파일의 LBA 시작 주소
    uint32_t meta_block_count = cmd->cdw11; // 연산 메타데이터 파일의 블록 갯수

    uint32_t target_start_lba = cmd->cdw12; // 연산 대상 파일의 LBA 시작 주소
    uint32_t target_block_count = cmd->cdw13; // 연산 대상 파일의 블록 갯수

    fprintf(stdout, "Custom read callback command entered\n");
    fprintf(stdout, "Opcode: 0x%x\n", cmd->opc);
    fprintf(stdout, "req->length: %u\n", req->length);
    fprintf(stdout, "req->iovcnt: %u\n", req->iovcnt);
    fprintf(stdout, "req->xfer: %u\n", req->xfer);

    fprintf(stdout, "meta_start_lba from CDW10: %u\n", meta_start_lba);
    fprintf(stdout, "meta_block_count from CDW11: %u\n", meta_block_count);
    fprintf(stdout, "target_start_lba from CDW12: %u\n", target_start_lba);
    fprintf(stdout, "target_block_count from CDW13: %u\n", target_block_count);

    // read 두번을 실행하는 초입부...
    // 첫 번째 read 실행
    // ctx 생성 및 초기화
    int rc;
    struct custom_ctx *ctx = spdk_zmalloc(sizeof(struct custom_ctx),
                                         0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY,
                                         SPDK_MALLOC_DMA);
    if (!ctx) {
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    ctx->req = req;
    ctx->desc = desc;
    ctx->ch = ch;
    ctx->first_result = NULL;
    ctx->first_result_len = 0;

    // 첫 번째 read 실행
    struct spdk_bdev_ext_io_opts opts = {
        .size = SPDK_SIZEOF(&opts, accel_sequence),
        .memory_domain = req->memory_domain,
        .memory_domain_ctx = req->memory_domain_ctx,
        .accel_sequence = req->accel_sequence,
    };

    rc = spdk_bdev_readv_blocks_ext(desc, ch, req->iov, req->iovcnt,
                                   meta_start_lba, meta_block_count,
                                   nvmf_bdev_ctrlr_first_read_complete, ctx, &opts);

    if (rc) {
        spdk_free(ctx);
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    response->status.sct = SPDK_NVME_SCT_GENERIC;
    response->status.sc = SPDK_NVME_SC_SUCCESS;
    return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

void dump_hex(const char *label, const void *data, size_t len)
{
    const unsigned char *p = (const unsigned char *)data;
    size_t i, j;

    fprintf(stdout, "%s:\n", label);
    for (i = 0; i < len; i += 16) {
        fprintf(stdout, "%08zx ", i);
        for (j = 0; j < 16 && i + j < len; j++) {
            fprintf(stdout, "%02x ", p[i + j]);
        }
        while (j < 16) {
            fprintf(stdout, "   ");
            j++;
        }
        fprintf(stdout, " ");
        for (j = 0; j < 16 && i + j < len; j++) {
            int c = p[i + j];
            fprintf(stdout, "%c", (c >= 32 && c <= 126) ? c : '.');
        }
        fprintf(stdout, "\n");
    }
}

int
nvmf_bdev_ctrlr_custom_heaan_cipadd_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
                                struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	//void* data = cmd->dptr.sgl1.address;
    uint32_t input_0_extents_count = cmd->cdw11; // Number of extents of inputfile_0
    uint32_t input_1_extents_count = cmd->cdw12; // Number of extents of inputfile_1
    uint32_t target_extents_count = cmd->cdw13; // Number of extents of targetfile

    //fprintf(stdout, "C_HEAAN_ADD: address : %lld\n", data);
 	//fprintf(stdout, "C_HEAAN_ADD: ext_cnt : %lld\n", extents_count);
	
	uint32_t total_data_len = req->iov->iov_len * NVMF_REQ_MAX_BUFFERS;
    uint32_t num_iovs = req->iovcnt;
	fprintf(stdout, "C_HEAAN_ADD: Total Data Transfer Length: %u bytes\n", total_data_len);
    fprintf(stdout, "C_HEAAN_ADD: Number of IOVs: %u\n", num_iovs);

    // CRITICAL: Check for a valid pointer before using it.
    // The previous crash was likely because this condition was not met.
    if (total_data_len == 0 || num_iovs == 0 || req->iov[0].iov_base == NULL) {
        SPDK_ERRLOG("Custom command 0xE0: No data buffer indicated or buffer is NULL. Cannot proceed.\n");
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    // Now, with the safety check, you can safely access the buffer.
    void *data_buf_ptr = req->iov[0].iov_base;
    size_t first_iov_len = req->iov[0].iov_len;
    
    // Your original print statements are now safe.
    fprintf(stdout, "C_HEAAN_ADD: First IOV Buffer Address: %p\n", data_buf_ptr);
    fprintf(stdout, "C_HEAAN_ADD: First IOV Length: %zu bytes\n", first_iov_len);

	fprintf(stdout, "Opcode: 0x%02x\n", cmd->opc);


    // Add more logging to confirm data is received
    SPDK_NOTICELOG("First 64 bytes of received data:\n");
	//spdk_log_dump("Received data", data_buf_ptr, spdk_min(first_iov_len, (size_t)64));

	dump_hex("Received Buffer Content (Target)", data_buf_ptr, 64);
	
	uint64_t* u64data = (uint64_t *)data_buf_ptr;
	uint32_t buf_num = 0;
	for(uint32_t i = 0; i < input_0_extents_count; i++) {
    	fprintf(stdout, "IN 0 LBA: %lu\n", u64data[2*buf_num]);
    	fprintf(stdout, "IN 0 Len: %lu\n", u64data[2*buf_num+1]);
		buf_num++;
	}
	
	for(uint32_t i = 0; i < input_1_extents_count; i++) {
    	fprintf(stdout, "IN 1 LBA: %lu\n", u64data[2*buf_num]);
    	fprintf(stdout, "IN 1 Len: %lu\n", u64data[2*buf_num+1]);
		buf_num++;
	}
	
	for(uint32_t i = 0; i < target_extents_count; i++) {
    	fprintf(stdout, "TGT LBA: %lu\n", u64data[2*buf_num]);
    	fprintf(stdout, "TGT Len: %lu\n", u64data[2*buf_num+1]);
		buf_num++;
	}

    response->status.sct = SPDK_NVME_SCT_GENERIC;
    response->status.sc = SPDK_NVME_SC_SUCCESS;
    //return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
    return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

// 0: IDLE, 1: DECODING/WRITING, 2: READY/READABLE
volatile int g_ndp_state = 0;
uint8_t *full_video_buffer = NULL;
uint32_t g_total_frames = 0;
uint16_t g_video_width = 0;
uint16_t g_video_height = 0;

struct ndp_request_ctx {
	uint64_t read_num_blocks, read_start_lba;
    struct spdk_nvmf_request *req;
    void *read_buf;
	uint32_t remaining_extents;
	uint64_t total_video_size;
};

struct buffer_data {
    uint8_t *ptr;
    size_t size;
    uint8_t *start;
    size_t total_size;
};

static int read_packet(void *opaque, uint8_t *buf, int buf_size) {
    struct buffer_data *bd = (struct buffer_data *)opaque;
    buf_size = (buf_size < (int)bd->size) ? buf_size : (int)bd->size;

    if (buf_size <= 0) return AVERROR_EOF;

    memcpy(buf, bd->ptr, buf_size);
    bd->ptr += buf_size;
    bd->size -= buf_size;
    return buf_size;
}

static int64_t seek_packet(void *opaque, int64_t offset, int whence) {
    struct buffer_data *bd = (struct buffer_data *)opaque;
    uint8_t *next_ptr;

    switch (whence) {
        case SEEK_SET: next_ptr = bd->start + offset; break;
        case SEEK_CUR: next_ptr = bd->ptr + offset; break;
        case SEEK_END: next_ptr = bd->start + bd->total_size + offset; break;
        case AVSEEK_SIZE: return bd->total_size;
        default: return -1;
    }

    if (next_ptr < bd->start || next_ptr > bd->start + bd->total_size) return -1;

    bd->ptr = next_ptr;
    bd->size = bd->total_size - (bd->ptr - bd->start);
    return (int64_t)(bd->ptr - bd->start);
}

image ndp_avframe_to_image(AVFrame *frame) {
    int w = frame->width;
    int h = frame->height;
    int c = 3;
    
    // 1. Darknet image 구조체 생성 (내부적으로 float 데이터 버퍼 할당됨)
    image im = make_image(w, h, c);

    // Y, U, V 데이터의 시작 주소
    unsigned char *data_y = frame->data[0];
    unsigned char *data_u = frame->data[1];
    unsigned char *data_v = frame->data[2];

    // 각 평면의 한 줄(row)당 바이트 크기 (Padding 고려)
    int stride_y = frame->linesize[0];
    int stride_u = frame->linesize[1];
    int stride_v = frame->linesize[2];

    for (int y = 0; y < h; ++y) {
        for (int x = 0; x < w; ++x) {
            // YUV420P: U, V는 Y의 가로/세로 1/2 크기임
            int y_idx = y * stride_y + x;
            int uv_idx = (y / 2) * stride_u + (x / 2);

            float Y = data_y[y_idx];
            float U = data_u[uv_idx];
            float V = data_v[uv_idx];

            // 2. YUV to RGB 변환 공식 (표준 BT.601)
            float r = Y + 1.402 * (V - 128);
            float g = Y - 0.344136 * (U - 128) - 0.714136 * (V - 128);
            float b = Y + 1.772 * (U - 128);

            // 3. 정규화 (0~255 -> 0.0~1.0) 및 Clamping (0~1 범위 제한)
            r = r / 255.0;
            g = g / 255.0;
            b = b / 255.0;

            if (r < 0) r = 0; if (r > 1) r = 1;
            if (g < 0) g = 0; if (g > 1) g = 1;
            if (b < 0) b = 0; if (b > 1) b = 1;

            // 4. Darknet Planar 포맷으로 저장
            // im.data 구조: [Red 평면(w*h)] [Green 평면(w*h)] [Blue 평면(w*h)]
            im.data[x + y * w + 0 * w * h] = r; // R
            im.data[x + y * w + 1 * w * h] = g; // G
            im.data[x + y * w + 2 * w * h] = b; // B
        }
    }

    return im;
}

static void
preprocess_video_yolo(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
    struct ndp_request_ctx *ctx = (struct ndp_request_ctx *)cb_arg;
    struct spdk_nvmf_request *req = ctx->req;
    struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;

    if (!success) {
        SPDK_ERRLOG("[CUST] Read failed\n");
        rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        goto cleanup;
    }

	if (ctx->remaining_extents > 1) {
		ctx->remaining_extents--;
		spdk_bdev_free_io(bdev_io);
		return; // wait for next read completion
	}

    if (g_ndp_state != 0) {
        SPDK_ERRLOG("NDP Busy (g_ndp_state: %d)\n", g_ndp_state);
        goto cleanup;
    }
	g_ndp_state = 1;

	int g_video_width = 416; 
	int g_video_height = 416;
	int net_c = 3;
	int letter_box = 1; // 1: letterbox, 0: resize

	int processed_frame_bytes = g_video_width * g_video_height * net_c * sizeof(float);
	
    // init FFmpeg contexts
    AVFormatContext *fmt_ctx = NULL;
    AVCodecContext *codec_ctx = NULL;
    AVIOContext *avio_ctx = NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    uint8_t *avio_ctx_buffer = NULL;
    
    int video_stream_idx = -1;
    int frame_idx = 0;
    int sampled_count = 0;
    int current_frame_size = 0;

    // 1. set up AVIOContext to read from memory buffer
    struct buffer_data bd = {
        .ptr = (uint8_t *)ctx->read_buf,
        .start = (uint8_t *)ctx->read_buf,
        .size = (size_t)(ctx->total_video_size),
        .total_size = (size_t)(ctx->total_video_size)
    };

    avio_ctx_buffer = av_malloc(4096);
    avio_ctx = avio_alloc_context(avio_ctx_buffer, 4096, 0, &bd, &read_packet, NULL, &seek_packet);
    
    fmt_ctx = avformat_alloc_context();
    fmt_ctx->pb = avio_ctx;

    if (avformat_open_input(&fmt_ctx, NULL, NULL, NULL) < 0) {
        SPDK_ERRLOG("Failed to open MP4 from memory\n");
        goto release;
    }

    avformat_find_stream_info(fmt_ctx, NULL);

    for (unsigned int i = 0; i < fmt_ctx->nb_streams; i++) {
        if (fmt_ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
            video_stream_idx = i;
            break;
        }
    }

    if (video_stream_idx == -1) goto release;

    // 3. set up codec context
    const AVCodec *codec = avcodec_find_decoder(fmt_ctx->streams[video_stream_idx]->codecpar->codec_id);
    codec_ctx = avcodec_alloc_context3(codec);
    avcodec_parameters_to_context(codec_ctx, fmt_ctx->streams[video_stream_idx]->codecpar);
    if (avcodec_open2(codec_ctx, codec, NULL) < 0) goto release;

    // get total number of frames (if available) to estimate buffer size for sampled frames
    int64_t total_nb_frames = fmt_ctx->streams[video_stream_idx]->nb_frames;
    if (total_nb_frames <= 0) total_nb_frames = 1000; // temporary fallback if frame count is not available

    // (temporary) sample 1 out of every 10 frames, so we need buffer for at most total_nb_frames/10 frames
    int max_samples = (total_nb_frames / 10) + 1;
	// 모든 프레임을 샘플링하기 위해 max_samples를 total_nb_frames로 설정
	// int max_samples = total_nb_frames;

    // read frames and sample
    while (av_read_frame(fmt_ctx, pkt) >= 0) {
        if (pkt->stream_index == video_stream_idx) {
            if (avcodec_send_packet(codec_ctx, pkt) == 0) {
                while (avcodec_receive_frame(codec_ctx, frame) == 0) {
                    
                    // sample every 10th frame
                    if (frame_idx % 10 == 0) {
					// 모든 프레임을 샘플링하기 위해 조건문 제거
					// if (frame_idx % 1 == 0) {

                        // if (current_frame_size == 0) {
                        //     current_frame_size = av_image_get_buffer_size(frame->format, frame->width, frame->height, 1);
						// 	g_video_width = frame->width;
						// 	g_video_height = frame->height;
                        // }

                        if (full_video_buffer == NULL) {
                            // alloc buffer for sampled frames (assuming max_samples frames, each of size current_frame_size)
                            //full_video_buffer = spdk_dma_zmalloc(current_frame_size * max_samples, 4096, NULL);
							full_video_buffer = spdk_dma_zmalloc(processed_frame_bytes * max_samples, 4096, NULL);
							if (!full_video_buffer) {
                            	SPDK_ERRLOG("Failed to allocate DMA buffer\n");
                            	rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
								goto release;
                        	}
                        }

                        if (full_video_buffer) {
							image im_orig = ndp_avframe_to_image(frame);

							image im_input = letter_box
								? letterbox_image(im_orig, g_video_width, g_video_height)
								: resize_image(im_orig, g_video_width, g_video_height);

							uint8_t *dst_ptr = (uint8_t *)full_video_buffer + (sampled_count * processed_frame_bytes);
                        	memcpy(dst_ptr, im_input.data, processed_frame_bytes);
							
							free_image(im_orig);
							free_image(im_input);



                            // dst buffer for the current sampled frame is at offset (sampled_count * current_frame_size) in full_video_buffer
                            //uint8_t *dst_ptr = (uint8_t *)full_video_buffer + (sampled_count * current_frame_size);
                            
                            /* av_image_copy_to_buffer(dst_ptr, current_frame_size,
                                                    (const uint8_t * const*)frame->data,
                                                    frame->linesize, frame->format,
                                                    frame->width, frame->height, 1);
							*/

							// memcpy(dst_ptr, im_input.data, processed_frame_bytes);

                            sampled_count++;
                        }
                    }
                    frame_idx++;
                }
            }
        }
        av_packet_unref(pkt);
    }

    g_total_frames = sampled_count; 
    g_ndp_state = 2; // READY
    rsp->cdw0 = (uint32_t)(g_total_frames * processed_frame_bytes);
	rsp->status.sc = SPDK_NVME_SC_SUCCESS;

    SPDK_NOTICELOG("[NDP] Sampling Done. Total frames: %ld, Total Sampled: %d frames, Sampled Size: %u bytes\n"
                    , total_nb_frames, g_total_frames, rsp->cdw0);

release:
    if (codec_ctx) avcodec_free_context(&codec_ctx);
    if (fmt_ctx) avformat_close_input(&fmt_ctx);
    if (avio_ctx) av_freep(&avio_ctx->buffer);
    av_freep(&avio_ctx);
    av_frame_free(&frame);
    av_packet_free(&pkt);

cleanup:
    rsp->status.sct = SPDK_NVME_SCT_GENERIC;
    spdk_bdev_free_io(bdev_io);
    spdk_free(ctx->read_buf);
    spdk_free(ctx);
    spdk_nvmf_request_complete(req);
}

int
nvmf_bdev_ctrlr_custom_preprocess_cmd(struct spdk_bdev *bdev,
                                      struct spdk_bdev_desc *desc,
                                      struct spdk_io_channel *ch,
                                      struct spdk_nvmf_request *req)
{

    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;

	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);

	uint32_t extents_count = cmd->cdw10;
	uint64_t lba[extents_count], blocks[extents_count];

	SPDK_NOTICELOG("bdev_num_blocks : %lu block_size : %u \n", bdev_num_blocks, block_size);
    SPDK_NOTICELOG("[CUST] Entered custom preprocess (OPC=0x%02x)\n", cmd->opc);

    if (!req->iov || req->iovcnt <= 0) {
        SPDK_ERRLOG("[CUST] No IOV payload\n");
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

	void *buf = req->iov[0].iov_base;

	size_t total_len = 0;

	// parse extents from the buffer (LBA and block count pairs)
	for (uint32_t i = 0; i < extents_count; i++) {
		memcpy(&lba[i],    buf + 2 * i * 8,  8);
		memcpy(&blocks[i], buf + 2 * i * 8 + 8,  8);

		SPDK_NOTICELOG("[CUST] extent[%d]: LBA=%" PRIu64 ", blocks=%" PRIu64 "\n", i, 8 * lba[i], 8 * blocks[i]);

		total_len += blocks[i] * block_size * 8;
	}

	// alloc buffer for this read
	void *read_buf = spdk_dma_zmalloc(total_len, 0, NULL);

	if (!read_buf) {
		SPDK_ERRLOG("Memory allocation failed\n");
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	// alloc context for this read
	struct ndp_request_ctx *ctx = spdk_zmalloc(sizeof(struct ndp_request_ctx),0x1000, NULL, SPDK_ENV_SOCKET_ID_ANY, SPDK_MALLOC_DMA);
	if (!ctx) {
		SPDK_ERRLOG("Context allocation failed\n");
		spdk_free(read_buf);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	ctx->req = req;
    ctx->read_buf = read_buf; 
    ctx->remaining_extents = extents_count;
	ctx->total_video_size = 0;

	uint64_t current_buffer_offset = 0;

	for (uint32_t i = 0; i < extents_count; i++) {
		ctx->read_start_lba = 8 * lba[i];
		ctx->read_num_blocks = 8 * blocks[i];
		uint64_t current_read_size = ctx->read_num_blocks * block_size;

		// async I/O submission
		int rc = spdk_bdev_read_blocks(desc, ch,
					ctx->read_buf + current_buffer_offset, ctx->read_start_lba, ctx->read_num_blocks,
					preprocess_video_yolo, ctx);

		if (rc != 0) {
			SPDK_ERRLOG("Read submission failed rc=%d\n", rc);
			ctx->remaining_extents--;
			spdk_free(read_buf);
			spdk_free(ctx);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		current_buffer_offset += current_read_size;
		ctx->total_video_size += current_read_size;
	}

    // async I/O submitted, will complete in callback
    return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_custom_get_result_cmd(struct spdk_bdev *bdev,
                                     struct spdk_bdev_desc *desc,
                                     struct spdk_io_channel *ch,
                                     struct spdk_nvmf_request *req)
{
    struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
    struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

    // 1. state check, buffer check
    if (g_ndp_state != 2 || full_video_buffer == NULL) {
        SPDK_ERRLOG("[CUST] Data not ready. State: %d\n", g_ndp_state);
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

    if (req->iovcnt == 0 || req->length == 0) {
        SPDK_ERRLOG("[CUST] Invalid request: length=%u, iovcnt=%d\n", req->length, req->iovcnt);
        response->status.sct = SPDK_NVME_SCT_GENERIC;
        response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

	// 2. calculate video size and validate offset/length 
	int g_video_width = 416; 
	int g_video_height = 416;
	int net_c = 3;
    uint32_t processed_frame_size = g_video_width * g_video_height * net_c * sizeof(float);
    uint32_t total_video_size = (uint32_t)g_total_frames * processed_frame_size;
	//uint32_t frame_size = av_image_get_buffer_size(AV_PIX_FMT_YUV420P, g_video_width, g_video_height, 1);
    //uint32_t total_video_size = (uint32_t)g_total_frames * frame_size;      // total size of the sampled video data in bytes

    uint32_t total_to_transfer = req->length; // requested transfer size in bytes
	uint32_t offset = cmd->cdw10; // requested start position
	if (offset >= total_video_size) {
		SPDK_ERRLOG("[CUST] Invalid offset: %u (Total: %u)\n", offset, total_video_size);
        response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
        return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
    }

	uint32_t video_data_remaining = total_video_size - offset;
    uint32_t bytes_left_to_send = total_to_transfer; // requested data remaining to be sent to the host
    uint8_t *src_ptr = (uint8_t *)full_video_buffer + offset;

    SPDK_NOTICELOG("[CUST] Transferring Chunk: Offset=%u, Length=%u (Remaining Video: %u)\n", 
                   offset, total_to_transfer, video_data_remaining);

    // 3. Fill the request's IOVs with video data and padding
    for (int i = 0; i < req->iovcnt && bytes_left_to_send > 0; i++) {
        uint8_t *dst_ptr = (uint8_t *)req->iov[i].iov_base;
        uint32_t iov_len = req->iov[i].iov_len;

		// SPDK_NOTICELOG("[CUST] iovlen=%ld\n", req->iov[i].iov_len); => 131072

        // calculate how many bytes to fill in this IOV (either the full IOV or the remaining requested data)
        uint32_t current_fill_len = spdk_min(iov_len, bytes_left_to_send);
        uint32_t offset_in_iov = 0;

        // copy video data to IOV
        if (video_data_remaining > 0) {
            uint32_t to_copy = spdk_min(current_fill_len, video_data_remaining);
            memcpy(dst_ptr, src_ptr, to_copy);
            
            src_ptr += to_copy;
			offset_in_iov += to_copy;
            video_data_remaining -= to_copy;
        }

        // padding with zeros if this IOV is not fully filled with video data
        if (offset_in_iov < current_fill_len) {
            uint32_t to_pad = current_fill_len - offset_in_iov;
            memset(dst_ptr + offset_in_iov, 0, to_pad);
        }

        bytes_left_to_send -= current_fill_len;
    }

    response->cdw0 = g_video_width << 16 | g_video_height;


	if(offset + total_to_transfer >= total_video_size){
		SPDK_NOTICELOG("[CUST] Final chunk reached. Freeing buffer and resetting state.\n");
		if (full_video_buffer != NULL) {
			spdk_dma_free(full_video_buffer);
			full_video_buffer = NULL;
		}
		g_ndp_state = 0;
		g_video_width = 0;
		g_video_height = 0;
		g_total_frames = 0;
	} else {
        // remain data exists for future requests
        SPDK_NOTICELOG("[CUST] Chunk sent. Waiting for next offset: %u\n", offset + total_to_transfer);
    }

	response->status.sct = SPDK_NVME_SCT_GENERIC;
    response->status.sc = SPDK_NVME_SC_SUCCESS;
    
    return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
nvmf_bdev_ctrlr_compare_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Compare NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_blocks(desc, ch, req->iov, req->iovcnt, start_lba, num_blocks,
				       nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_compare_and_write_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				      struct spdk_io_channel *ch, struct spdk_nvmf_request *cmp_req, struct spdk_nvmf_request *write_req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_nvme_cmd *cmp_cmd = &cmp_req->cmd->nvme_cmd;
	struct spdk_nvme_cmd *write_cmd = &write_req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &write_req->rsp->nvme_cpl;
	uint64_t write_start_lba, cmp_start_lba;
	uint64_t write_num_blocks, cmp_num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmp_cmd, &cmp_start_lba, &cmp_num_blocks);
	nvmf_bdev_ctrlr_get_rw_params(write_cmd, &write_start_lba, &write_num_blocks);

	if (spdk_unlikely(write_start_lba != cmp_start_lba || write_num_blocks != cmp_num_blocks)) {
		SPDK_ERRLOG("Fused command start lba / num blocks mismatch\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, write_start_lba,
			  write_num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(write_num_blocks * block_size > write_req->length)) {
		SPDK_ERRLOG("Write NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    write_num_blocks, block_size, write_req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_comparev_and_writev_blocks(desc, ch, cmp_req->iov, cmp_req->iovcnt, write_req->iov,
			write_req->iovcnt, write_start_lba, write_num_blocks, nvmf_bdev_ctrlr_complete_cmd, write_req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(cmp_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, cmp_req);
			nvmf_bdev_ctrl_queue_io(write_req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, write_req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_write_zeroes_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t max_write_zeroes_size = req->qpair->ctrlr->subsys->max_write_zeroes_size_kib;
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(cmd, &start_lba, &num_blocks);
	if (spdk_unlikely(max_write_zeroes_size > 0 &&
			  num_blocks > (max_write_zeroes_size << 10) / spdk_bdev_get_block_size(bdev))) {
		SPDK_ERRLOG("invalid write zeroes size, should not exceed %" PRIu64 "Kib\n", max_write_zeroes_size);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(cmd->cdw12_bits.write_zeroes.deac)) {
		SPDK_ERRLOG("Write Zeroes Deallocate is not supported\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_write_zeroes_blocks(desc, ch, start_lba, num_blocks,
					   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_flush_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			  struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	int rc;

	/* As for NVMeoF controller, SPDK always set volatile write
	 * cache bit to 1, return success for those block devices
	 * which can't support FLUSH command.
	 */
	if (!spdk_bdev_io_type_supported(bdev, SPDK_BDEV_IO_TYPE_FLUSH)) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	rc = spdk_bdev_flush_blocks(desc, ch, 0, spdk_bdev_get_num_blocks(bdev),
				    nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

struct nvmf_bdev_ctrlr_unmap {
	struct spdk_nvmf_request	*req;
	uint32_t			count;
	struct spdk_bdev_desc		*desc;
	struct spdk_bdev		*bdev;
	struct spdk_io_channel		*ch;
	uint32_t			range_index;
};

static void
nvmf_bdev_ctrlr_unmap_cpl(struct spdk_bdev_io *bdev_io, bool success,
			  void *cb_arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = cb_arg;
	struct spdk_nvmf_request	*req = unmap_ctx->req;
	struct spdk_nvme_cpl		*response = &req->rsp->nvme_cpl;
	int				sc, sct;
	uint32_t			cdw0;

	unmap_ctx->count--;

	if (response->status.sct == SPDK_NVME_SCT_GENERIC &&
	    response->status.sc == SPDK_NVME_SC_SUCCESS) {
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);
		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	if (unmap_ctx->count == 0) {
		spdk_nvmf_request_complete(req);
		free(unmap_ctx);
	}
	spdk_bdev_free_io(bdev_io);
}

static int nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
				 struct nvmf_bdev_ctrlr_unmap *unmap_ctx);
static void
nvmf_bdev_ctrlr_unmap_resubmit(void *arg)
{
	struct nvmf_bdev_ctrlr_unmap *unmap_ctx = arg;
	struct spdk_nvmf_request *req = unmap_ctx->req;
	struct spdk_bdev_desc *desc = unmap_ctx->desc;
	struct spdk_bdev *bdev = unmap_ctx->bdev;
	struct spdk_io_channel *ch = unmap_ctx->ch;

	nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, unmap_ctx);
}

static int
nvmf_bdev_ctrlr_unmap(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		      struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		      struct nvmf_bdev_ctrlr_unmap *unmap_ctx)
{
	uint16_t nr, i;
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t max_discard_size = req->qpair->ctrlr->subsys->max_discard_size_kib;
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	struct spdk_iov_xfer ix;
	uint64_t lba;
	uint32_t lba_count;
	int rc;

	nr = cmd->cdw10_bits.dsm.nr + 1;
	if (nr * sizeof(struct spdk_nvme_dsm_range) > req->length) {
		SPDK_ERRLOG("Dataset Management number of ranges > SGL length\n");
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (unmap_ctx == NULL) {
		unmap_ctx = calloc(1, sizeof(*unmap_ctx));
		if (!unmap_ctx) {
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		unmap_ctx->req = req;
		unmap_ctx->desc = desc;
		unmap_ctx->ch = ch;
		unmap_ctx->bdev = bdev;

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_SUCCESS;
	} else {
		unmap_ctx->count--;	/* dequeued */
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);

	for (i = unmap_ctx->range_index; i < nr; i++) {
		struct spdk_nvme_dsm_range dsm_range = { 0 };

		spdk_iov_xfer_to_buf(&ix, &dsm_range, sizeof(dsm_range));

		lba = dsm_range.starting_lba;
		lba_count = dsm_range.length;
		if (max_discard_size > 0 && lba_count > (max_discard_size << 10) / block_size) {
			SPDK_ERRLOG("invalid unmap size, should not exceed %" PRIu64 "Kib\n", max_discard_size);
			response->status.sct = SPDK_NVME_SCT_GENERIC;
			response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
			return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
		}

		unmap_ctx->count++;

		rc = spdk_bdev_unmap_blocks(desc, ch, lba, lba_count,
					    nvmf_bdev_ctrlr_unmap_cpl, unmap_ctx);
		if (rc) {
			if (rc == -ENOMEM) {
				nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_bdev_ctrlr_unmap_resubmit, unmap_ctx);
				/* Unmap was not yet submitted to bdev */
				/* unmap_ctx->count will be decremented when the request is dequeued */
				return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
			}
			response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
			unmap_ctx->count--;
			/* We can't return here - we may have to wait for any other
				* unmaps already sent to complete */
			break;
		}
		unmap_ctx->range_index++;
	}

	if (unmap_ctx->count == 0) {
		free(unmap_ctx);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_dsm_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;

	if (cmd->cdw11_bits.dsm.ad) {
		return nvmf_bdev_ctrlr_unmap(bdev, desc, ch, req, NULL);
	}

	response->status.sct = SPDK_NVME_SCT_GENERIC;
	response->status.sc = SPDK_NVME_SC_SUCCESS;
	return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
}

int
nvmf_bdev_ctrlr_copy_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cmd *cmd = &req->cmd->nvme_cmd;
	struct spdk_nvme_cpl *response = &req->rsp->nvme_cpl;
	uint64_t sdlba = ((uint64_t)cmd->cdw11 << 32) + cmd->cdw10;
	struct spdk_nvme_scc_source_range range = { 0 };
	struct spdk_iov_xfer ix;
	int rc;

	SPDK_DEBUGLOG(nvmf, "Copy command: SDLBA %lu, NR %u, desc format %u, PRINFOR %u, "
		      "DTYPE %u, STCW %u, PRINFOW %u, FUA %u, LR %u\n",
		      sdlba,
		      cmd->cdw12_bits.copy.nr,
		      cmd->cdw12_bits.copy.df,
		      cmd->cdw12_bits.copy.prinfor,
		      cmd->cdw12_bits.copy.dtype,
		      cmd->cdw12_bits.copy.stcw,
		      cmd->cdw12_bits.copy.prinfow,
		      cmd->cdw12_bits.copy.fua,
		      cmd->cdw12_bits.copy.lr);

	if (spdk_unlikely(req->length != (cmd->cdw12_bits.copy.nr + 1) *
			  sizeof(struct spdk_nvme_scc_source_range))) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	/*
	 * We support only one source range, and rely on this with the xfer
	 * below.
	 */
	if (cmd->cdw12_bits.copy.nr > 0) {
		response->status.sct = SPDK_NVME_SCT_COMMAND_SPECIFIC;
		response->status.sc = SPDK_NVME_SC_CMD_SIZE_LIMIT_SIZE_EXCEEDED;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (cmd->cdw12_bits.copy.df != 0) {
		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INVALID_FIELD;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	spdk_iov_xfer_init(&ix, req->iov, req->iovcnt);
	spdk_iov_xfer_to_buf(&ix, &range, sizeof(range));

	rc = spdk_bdev_copy_blocks(desc, ch, sdlba, range.slba, range.nlb + 1,
				   nvmf_bdev_ctrlr_complete_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}

		response->status.sct = SPDK_NVME_SCT_GENERIC;
		response->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
nvmf_bdev_ctrlr_nvme_passthru_io(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
				 struct spdk_io_channel *ch, struct spdk_nvmf_request *req)
{
	int rc;

	rc = spdk_bdev_nvme_iov_passthru_md(desc, ch, &req->cmd->nvme_cmd, req->iov, req->iovcnt,
					    req->length, NULL, 0, nvmf_bdev_ctrlr_complete_cmd, req);

	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

int
spdk_nvmf_bdev_ctrlr_nvme_passthru_admin(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
		struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
		spdk_nvmf_nvme_passthru_cmd_cb cb_fn)
{
	int rc;

	if (spdk_unlikely(req->iovcnt > 1)) {
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	req->cmd_cb_fn = cb_fn;

	rc = spdk_bdev_nvme_admin_passthru(desc, ch, &req->cmd->nvme_cmd, req->iov[0].iov_base, req->length,
					   nvmf_bdev_ctrlr_complete_admin_cmd, req);
	if (spdk_unlikely(rc)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		req->rsp->nvme_cpl.status.sct = SPDK_NVME_SCT_GENERIC;
		if (rc == -ENOTSUP) {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INVALID_OPCODE;
		} else {
			req->rsp->nvme_cpl.status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		}

		req->rsp->nvme_cpl.status.dnr = 1;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_bdev_ctrlr_complete_abort_cmd(struct spdk_bdev_io *bdev_io, bool success, void *cb_arg)
{
	struct spdk_nvmf_request *req = cb_arg;

	if (success) {
		req->rsp->nvme_cpl.cdw0 &= ~1U;
	}

	spdk_nvmf_request_complete(req);
	spdk_bdev_free_io(bdev_io);
}

int
spdk_nvmf_bdev_ctrlr_abort_cmd(struct spdk_bdev *bdev, struct spdk_bdev_desc *desc,
			       struct spdk_io_channel *ch, struct spdk_nvmf_request *req,
			       struct spdk_nvmf_request *req_to_abort)
{
	int rc;

	assert((req->rsp->nvme_cpl.cdw0 & 1U) != 0);

	rc = spdk_bdev_abort(desc, ch, req_to_abort, nvmf_bdev_ctrlr_complete_abort_cmd, req);
	if (spdk_likely(rc == 0)) {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else if (rc == -ENOMEM) {
		nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_admin_cmd_resubmit, req);
		return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
	} else {
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}
}

bool
nvmf_bdev_ctrlr_get_dif_ctx(struct spdk_bdev *bdev, struct spdk_nvme_cmd *cmd,
			    struct spdk_dif_ctx *dif_ctx)
{
	uint32_t init_ref_tag, dif_check_flags = 0;
	int rc;
	struct spdk_dif_ctx_init_ext_opts dif_opts;

	if (spdk_bdev_get_md_size(bdev) == 0) {
		return false;
	}

	/* Initial Reference Tag is the lower 32 bits of the start LBA. */
	init_ref_tag = (uint32_t)from_le64(&cmd->cdw10);

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_REFTAG)) {
		dif_check_flags |= SPDK_DIF_FLAGS_REFTAG_CHECK;
	}

	if (spdk_bdev_is_dif_check_enabled(bdev, SPDK_DIF_CHECK_TYPE_GUARD)) {
		dif_check_flags |= SPDK_DIF_FLAGS_GUARD_CHECK;
	}

	dif_opts.size = SPDK_SIZEOF(&dif_opts, dif_pi_format);
	dif_opts.dif_pi_format = SPDK_DIF_PI_FORMAT_16;
	rc = spdk_dif_ctx_init(dif_ctx,
			       spdk_bdev_get_block_size(bdev),
			       spdk_bdev_get_md_size(bdev),
			       spdk_bdev_is_md_interleaved(bdev),
			       spdk_bdev_is_dif_head_of_md(bdev),
			       spdk_bdev_get_dif_type(bdev),
			       dif_check_flags,
			       init_ref_tag, 0, 0, 0, 0, &dif_opts);

	return (rc == 0) ? true : false;
}

static void
nvmf_bdev_ctrlr_zcopy_start_complete(struct spdk_bdev_io *bdev_io, bool success,
				     void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;
	struct iovec *iov;
	int iovcnt = 0;

	if (spdk_unlikely(!success)) {
		int                     sc = 0, sct = 0;
		uint32_t                cdw0 = 0;
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;

		spdk_bdev_free_io(bdev_io);
		spdk_nvmf_request_complete(req);
		return;
	}

	spdk_bdev_io_get_iovec(bdev_io, &iov, &iovcnt);

	assert(iovcnt <= NVMF_REQ_MAX_BUFFERS);
	assert(iovcnt > 0);

	req->iovcnt = iovcnt;

	assert(req->iov == iov);

	req->zcopy_bdev_io = bdev_io; /* Preserve the bdev_io for the end zcopy */

	spdk_nvmf_request_complete(req);
	/* Don't free the bdev_io here as it is needed for the END ZCOPY */
}

int
nvmf_bdev_ctrlr_zcopy_start(struct spdk_bdev *bdev,
			    struct spdk_bdev_desc *desc,
			    struct spdk_io_channel *ch,
			    struct spdk_nvmf_request *req)
{
	struct spdk_nvme_cpl *rsp = &req->rsp->nvme_cpl;
	uint64_t bdev_num_blocks = spdk_bdev_get_num_blocks(bdev);
	uint32_t block_size = spdk_bdev_get_block_size(bdev);
	uint64_t start_lba;
	uint64_t num_blocks;
	int rc;

	nvmf_bdev_ctrlr_get_rw_params(&req->cmd->nvme_cmd, &start_lba, &num_blocks);

	if (spdk_unlikely(!nvmf_bdev_ctrlr_lba_in_range(bdev_num_blocks, start_lba, num_blocks))) {
		SPDK_ERRLOG("end of media\n");
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_LBA_OUT_OF_RANGE;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	if (spdk_unlikely(num_blocks * block_size > req->length)) {
		SPDK_ERRLOG("Read NLB %" PRIu64 " * block size %" PRIu32 " > SGL length %" PRIu32 "\n",
			    num_blocks, block_size, req->length);
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_DATA_SGL_LENGTH_INVALID;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	bool populate = (req->cmd->nvme_cmd.opc == SPDK_NVME_OPC_READ) ? true : false;

	rc = spdk_bdev_zcopy_start(desc, ch, req->iov, req->iovcnt, start_lba,
				   num_blocks, populate, nvmf_bdev_ctrlr_zcopy_start_complete, req);
	if (spdk_unlikely(rc != 0)) {
		if (rc == -ENOMEM) {
			nvmf_bdev_ctrl_queue_io(req, bdev, ch, nvmf_ctrlr_process_io_cmd_resubmit, req);
			return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
		}
		rsp->status.sct = SPDK_NVME_SCT_GENERIC;
		rsp->status.sc = SPDK_NVME_SC_INTERNAL_DEVICE_ERROR;
		return SPDK_NVMF_REQUEST_EXEC_STATUS_COMPLETE;
	}

	return SPDK_NVMF_REQUEST_EXEC_STATUS_ASYNCHRONOUS;
}

static void
nvmf_bdev_ctrlr_zcopy_end_complete(struct spdk_bdev_io *bdev_io, bool success,
				   void *cb_arg)
{
	struct spdk_nvmf_request	*req = cb_arg;

	if (spdk_unlikely(!success)) {
		int                     sc = 0, sct = 0;
		uint32_t                cdw0 = 0;
		struct spdk_nvme_cpl    *response = &req->rsp->nvme_cpl;
		spdk_bdev_io_get_nvme_status(bdev_io, &cdw0, &sct, &sc);

		response->cdw0 = cdw0;
		response->status.sc = sc;
		response->status.sct = sct;
	}

	spdk_bdev_free_io(bdev_io);
	req->zcopy_bdev_io = NULL;
	spdk_nvmf_request_complete(req);
}

void
nvmf_bdev_ctrlr_zcopy_end(struct spdk_nvmf_request *req, bool commit)
{
	int rc __attribute__((unused));

	rc = spdk_bdev_zcopy_end(req->zcopy_bdev_io, commit, nvmf_bdev_ctrlr_zcopy_end_complete, req);

	/* The only way spdk_bdev_zcopy_end() can fail is if we pass a bdev_io type that isn't ZCOPY */
	assert(rc == 0);
}