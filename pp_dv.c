#include "mlx5_ifc.h"
#include "pp_dv.h"

#include "ccan/ilog.h"
#include "ccan/minmax.h"

#include "util/mmio.h"
#include "util/udma_barrier.h"
#include "util/util.h"

#define DVDBG printf

#define PP_ACCESS_FALGS (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE | IBV_ACCESS_REMOTE_READ)

int pp_create_cq_dv(const struct pp_context *ppc, struct pp_dv_cq *dvcq)
{
	uint32_t in[DEVX_ST_SZ_DW(create_cq_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(create_cq_out)] = {};
	void *cqc = DEVX_ADDR_OF(create_cq_in, in, cq_context);
	//int log_cq_size = PP_MAX_LOG_CQ_SIZE;
	//struct mlx5_cqe64 *cqe;
	uint32_t eqn;
	int ret;

	ret = mlx5dv_devx_query_eqn(ppc->ibctx, 0, &eqn);
	if (ret) {
		ERR("devx_query_eqn failed: %d, errno %d\n", ret, errno);
		return ret;
	}

	ret = posix_memalign((void **)&dvcq->db, 8, 8);
	if (ret) {
		ERR("cq.db posix_memalign(8) failed\n");
		return ret;
	}

	dvcq->db[0] = 0;
	dvcq->db[1] = 0;
	dvcq->db_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvcq->db, 8,
					     PP_ACCESS_FALGS);
	if (!dvcq->db_umem) {
		ERR("cq.db umem_reg() failed\n");
		goto fail;
	}

	int size = 4096 * 8;	/* FIXME: How to calculate it? (cq_size * cqe_size)? */
	dvcq->buflen = align(size, sysconf(_SC_PAGESIZE));
	ret = posix_memalign(&dvcq->buf, sysconf(_SC_PAGESIZE), dvcq->buflen);
	if (ret) {
		ERR("cq.buf posix_memalign(0x%lx) failed\n", dvcq->buflen);
		goto fail_alloc_cqbuf;
	}

	memset(dvcq->buf, 0, dvcq->buflen);
	dvcq->buff_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvcq->buf,
					       dvcq->buflen, PP_ACCESS_FALGS);
	if (!dvcq->buff_umem) {
		ERR("cq.buf umem_reg(0x%lx) failed\n", dvcq->buflen);
		goto fail_umem_reg_cqbuf;
	}

	dvcq->uar = mlx5dv_devx_alloc_uar(ppc->ibctx, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!dvcq->uar) {
		ERR("cq alloc_uar() failed\n");
		goto fail_alloc_uar;
	}

	DEVX_SET(create_cq_in, in, opcode, MLX5_CMD_OP_CREATE_CQ);

	DEVX_SET64(cqc, cqc, log_page_size, 0); /* 12 - MLX5_ADAPTER_PAGE_SHIFT */
	DEVX_SET64(cqc, cqc, page_offset, 0);
	DEVX_SET(cqc, cqc, log_cq_size, 6);
	DEVX_SET(cqc, cqc, cqe_sz, 0); /* BYTES_64 */

	/* FIXME: Check these args */
	DEVX_SET(cqc, cqc, uar_page, dvcq->uar->page_id);
	DEVX_SET(cqc, cqc, c_eqn, eqn);

	DEVX_SET64(cqc, cqc, dbr_umem_id, dvcq->db_umem->umem_id);
	DEVX_SET64(cqc, cqc, dbr_umem_valid, 1);

	DEVX_SET(create_cq_in, in, cq_umem_id, dvcq->buff_umem->umem_id);
	DEVX_SET(create_cq_in, in, cq_umem_valid, 1);

	dvcq->obj = mlx5dv_devx_obj_create(ppc->ibctx, in, sizeof(in), out, sizeof(out));
	if (!dvcq->obj) {
		ERR("devx_obj_create(cq) failed: eqn %d\n", eqn);
		goto fail_obj_create;
	}
	dvcq->cqn = DEVX_GET(create_cq_out, out, cqn);
	INFO("dv: CQ %d created, eqn %d, db@%p, buf@%p\n",
	     dvcq->cqn, eqn, dvcq->db, dvcq->buf);

/*
  FIXME
	pp->drcq.cqe_sz = 64;
	pp->drcq.cons_index = 0;
	pp->drcq.ncqe = 1 << log_cq_size;
	pp->drcq.qp = &pp->drqp;
	for (i = 0; i < 1 << log_cq_size; i++) {
		cqe = dr_cq_get_cqe(&pp->drcq, i);
		cqe->op_own = MLX5_CQE_INVALID << 4;
	}
*/
	return 0;

fail_obj_create:
	mlx5dv_devx_free_uar(dvcq->uar);
fail_alloc_uar:
	mlx5dv_devx_umem_dereg(dvcq->buff_umem);
fail_umem_reg_cqbuf:
	free(dvcq->buf);
	dvcq->buf = NULL;
fail_alloc_cqbuf:
	mlx5dv_devx_umem_dereg(dvcq->db_umem);
fail:
	free(dvcq->db);
	dvcq->db = NULL;

	return -1;
}

void pp_destroy_cq_dv(struct pp_dv_cq *dvcq)
{
	mlx5dv_devx_obj_destroy(dvcq->obj);
	mlx5dv_devx_free_uar(dvcq->uar);
	mlx5dv_devx_umem_dereg(dvcq->buff_umem);
	free(dvcq->buf);
	dvcq->buf = NULL;
	mlx5dv_devx_umem_dereg(dvcq->db_umem);
	free(dvcq->db);
	dvcq->db = NULL;
}

static int calc_rc_send_wqe(const struct ibv_qp_cap *qp_cap)
{
	int size, inl_size, tot_size;

	//size = sizeof(struct mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg);
	size = 192;		/* Ref. providers/mlx5/verb.c::sq_overhead() */
	if (qp_cap->max_inline_data)
		inl_size = size + align(sizeof(struct mlx5_wqe_inl_data_seg) + qp_cap->max_inline_data, 16);

	/* 8.9.4.1.14 Send Data Segments */
	size += qp_cap->max_send_sge * sizeof(struct mlx5_wqe_data_seg);
	tot_size = max(size, inl_size);
	return align(tot_size, MLX5_SEND_WQE_BB);
}

static int calc_sq_size(struct dv_wq *sq, const struct ibv_qp_cap *qp_cap)
{
	int wqe_size;
	int wq_size;

	wqe_size = calc_rc_send_wqe(qp_cap);
	wq_size = roundup_pow_of_two(qp_cap->max_send_wr * wqe_size);
	sq->wqe_cnt = wq_size / MLX5_SEND_WQE_BB;
	sq->wqe_shift = STATIC_ILOG_32(MLX5_SEND_WQE_BB) - 1;
	sq->max_gs = qp_cap->max_send_sge;
	sq->max_post = wq_size / wqe_size;

	DVDBG("    sq: wqe_size %d(0x%x), wq_size %d(0x%x), wqe_cnt %d, wqe_shift %d, max_gs %d, max_post %d\n",
	       wqe_size, wqe_size, wq_size, wq_size, sq->wqe_cnt, sq->wqe_shift, sq->max_gs, sq->max_post);
	return wq_size;
}

static int calc_recv_wqe(const struct ibv_qp_cap *qp_cap)
{
	int size, num_scatter;

	num_scatter = max_t(uint32_t, qp_cap->max_recv_sge, 1);
	size = sizeof(struct mlx5_wqe_data_seg) * num_scatter;
	size = roundup_pow_of_two(size);

	return size;
}

static int calc_rq_size(struct dv_wq *rq, const struct ibv_qp_cap *qp_cap)
{
	int wqe_size;
	int wq_size;

	wqe_size = calc_recv_wqe(qp_cap);

	wq_size = roundup_pow_of_two(qp_cap->max_recv_wr) * wqe_size;
	wq_size = max(wq_size, MLX5_SEND_WQE_BB);
	rq->wqe_cnt = wq_size / wqe_size;
	rq->wqe_shift = ilog32(wqe_size - 1);
	rq->max_post = 1 << ilog32(wq_size / wqe_size - 1);
	rq->max_gs = wqe_size / sizeof(struct mlx5_wqe_data_seg);

	DVDBG("    rq: wqe_size %d(0x%x), wq_size %d(0x%x), wqe_cnt %d, wqe_shift %d, max_gs %d, max_post %d\n",
	      wqe_size, wqe_size, wq_size, wq_size, rq->wqe_cnt, rq->wqe_shift, rq->max_gs, rq->max_post);
	return wq_size;
}

static int calc_wq_size(struct pp_dv_qp *dvqp, const struct ibv_qp_cap *cap)
{
	int result;
	int ret;

	DVDBG("    qp_cap: .max_send_wr %d, max_recv_wr %d, max_send_sge %d, max_recv_sge %d, max_inline_data %d\n",
	      cap->max_send_wr, cap->max_recv_wr, cap->max_send_sge, cap->max_recv_sge, cap->max_inline_data);

	result = calc_sq_size(&dvqp->sq, cap);
	ret = calc_rq_size(&dvqp->rq, cap);

	result += ret;
	dvqp->rq.offset = 0;
	dvqp->sq.offset = ret;
/*
	dvqp->max_inline_data =
		calc_rc_send_wqe(&dr_qp->cap) -
		(sizeof(struct  mlx5_wqe_ctrl_seg) + sizeof(struct mlx5_wqe_raddr_seg)) -
		sizeof(struct mlx5_wqe_inl_data_seg);
*/
	DVDBG("   wq total %d(0x%x) sq.offset %d(0x%x)\n\n",
	      result, result, dvqp->sq.offset, dvqp->sq.offset);
	return result;
}

int pp_create_qp_dv(const struct pp_context *ppc,
		    const struct pp_dv_cq *dvcq,
		    struct pp_dv_qp *dvqp)
{
	uint32_t in[DEVX_ST_SZ_DW(create_qp_in)] = {};
	uint32_t out[DEVX_ST_SZ_DW(create_qp_out)] = {};
	void *qpc = DEVX_ADDR_OF(create_qp_in, in, qpc);
	struct mlx5dv_pd dv_pd = {};
	struct mlx5dv_obj obj = {};
	int size, ret;

	obj.pd.in = ppc->pd;
	obj.pd.out = &dv_pd;
	ret = mlx5dv_init_obj(&obj, MLX5DV_OBJ_PD);
	if (ret) {
		ERR("mlx5dv_init_obj(PD) failed\n");
		return ret;
	}

	ret = posix_memalign((void **)&dvqp->db, 8, 8);
	if (ret) {
		ERR("qp.db posix_memalign(8) failed\n");
		return ret;
	}

	dvqp->db[0] = 0;
	dvqp->db[1] = 0;
	dvqp->db_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvqp->db, 8,
					     PP_ACCESS_FALGS);
	if (!dvqp->db_umem) {
		ERR("qp.db umem_reg() failed\n");
		goto fail;
	}

	size = calc_wq_size(dvqp, &ppc->cap);
	dvqp->buflen = align(size, sysconf(_SC_PAGESIZE));
	ret = posix_memalign(&dvqp->buf, sysconf(_SC_PAGESIZE), dvqp->buflen);
	if (ret) {
		ERR("qp.buf posix_memalign(0x%lx) failed\n", dvqp->buflen);
		goto fail_alloc_qpbuf;
	}

	memset(dvqp->buf, 0, dvqp->buflen);
	dvqp->buff_umem = mlx5dv_devx_umem_reg(ppc->ibctx, dvqp->buf,
					       dvqp->buflen, PP_ACCESS_FALGS);
	if (!dvqp->buff_umem) {
		ERR("qp.buf umem_reg(0x%lx) failed\n", dvqp->buflen);
		goto fail_umem_reg_qpbuf;
	}

	dvqp->uar = mlx5dv_devx_alloc_uar(ppc->ibctx, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!dvqp->uar) {
		ERR("qp alloc_uar() failed\n");
		goto fail_alloc_uar;
	}

	DEVX_SET(create_qp_in, in, opcode, MLX5_CMD_OP_CREATE_QP);
        DEVX_SET(qpc, qpc, st, MLX5_QPC_ST_RC);
	DEVX_SET(qpc, qpc, pd, dv_pd.pdn);
	DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
	DEVX_SET(qpc, qpc, log_rq_size, ilog32(dvqp->rq.wqe_cnt - 1));
	DEVX_SET(qpc, qpc, log_rq_stride, dvqp->rq.wqe_shift - 4);
	DEVX_SET(qpc, qpc, log_sq_size, ilog32(dvqp->sq.wqe_cnt - 1));
	DEVX_SET(qpc, qpc, no_sq, 0);
	DEVX_SET(qpc, qpc, rlky, 0);
	DEVX_SET(qpc, qpc, uar_page, dvqp->uar->page_id);
	DEVX_SET(qpc, qpc, log_page_size, 0); /* 12 - MLX5_ADAPTER_PAGE_SHIFT */
	DEVX_SET(qpc, qpc, fre, 0);

	DEVX_SET(qpc, qpc, cqn_snd, dvcq->cqn);
	DEVX_SET(qpc, qpc, cqn_rcv, dvcq->cqn);

	DEVX_SET(qpc, qpc, page_offset, 0);

	DEVX_SET(qpc, qpc, dbr_addr, 0);
	DEVX_SET(qpc, qpc, rq_type, 0); /* NON_ZERO_RQ */

	DEVX_SET(qpc, qpc, dbr_umem_id, dvqp->db_umem->umem_id);
	DEVX_SET(qpc, qpc, dbr_umem_valid, 1);
	DEVX_SET(qpc, qpc, isolate_vl_tc, 0);

	DEVX_SET(create_qp_in, in, wq_umem_id, dvqp->buff_umem->umem_id);
	DEVX_SET(create_qp_in, in, wq_umem_valid, 1);

	dvqp->obj = mlx5dv_devx_obj_create(ppc->ibctx, in, sizeof(in), out, sizeof(out));
	if (!dvqp->obj) {
		ERR("devx_obj_create(qp) failed\n");
		goto fail_obj_create;
	}

	dvqp->qpn = DEVX_GET(create_qp_out, out, qpn);

	dvqp->sq_start = dvqp->buf + dvqp->sq.offset;
	dvqp->sq.qend = dvqp->buf + dvqp->sq.offset + (dvqp->sq.wqe_cnt << dvqp->sq.wqe_shift);
	dvqp->sq.cur_post = 0;
	dvqp->rq.head = 0;
	dvqp->rq.tail = 0;

	INFO("dv: QP %d created; sq.wqe_cnt %d(log_sq_size %d), rq.wqe_cnt %d(log_rq_size %d), rq_wqe_shift %d\n",
	     dvqp->qpn, dvqp->sq.wqe_cnt, ilog32(dvqp->sq.wqe_cnt - 1),
	     dvqp->sq.wqe_cnt, ilog32(dvqp->rq.wqe_cnt - 1), dvqp->rq.wqe_shift);

	return 0;

fail_obj_create:
	mlx5dv_devx_free_uar(dvqp->uar);
fail_alloc_uar:
	mlx5dv_devx_umem_dereg(dvqp->buff_umem);
fail_umem_reg_qpbuf:
	free(dvqp->buf);
	dvqp->buf = NULL;
fail_alloc_qpbuf:
	mlx5dv_devx_umem_dereg(dvqp->db_umem);
fail:
	free(dvqp->db);
	dvqp->db = NULL;

	return -1;
}

void pp_destroy_qp_dv(struct pp_dv_qp *dvqp)
{
	mlx5dv_devx_obj_destroy(dvqp->obj);
	mlx5dv_devx_free_uar(dvqp->uar);
	mlx5dv_devx_umem_dereg(dvqp->buff_umem);
	free(dvqp->buf);
	dvqp->buf = NULL;
	mlx5dv_devx_umem_dereg(dvqp->db_umem);
	free(dvqp->db);
	dvqp->db = NULL;
}

static int dvqp_rst2init(const struct pp_context *ppc, struct pp_dv_qp *dvqp)
{
	uint32_t out[DEVX_ST_SZ_DW(rst2init_qp_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(rst2init_qp_in)] = {};
	void *qpc = DEVX_ADDR_OF(rst2init_qp_in, in, qpc);
	int ret;

	DEVX_SET(rst2init_qp_in, in, opcode, MLX5_CMD_OP_RST2INIT_QP);
	DEVX_SET(rst2init_qp_in, in, qpn, dvqp->qpn);

	DEVX_SET(qpc, qpc, pm_state, MLX5_QPC_PM_STATE_MIGRATED);
	DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, ppc->port_num);
	DEVX_SET(qpc, qpc, rre, 1);
	DEVX_SET(qpc, qpc, rwe, 1);
	//DEVX_SET(qpc, qpc, lag_tx_port_affinity, 1);
	//DEVX_SET(qpc, qpc, primary_address_path.pkey_index, 0);
	//DEVX_SET(qpc, qpc, counter_set_id, 0);

	ret = mlx5dv_devx_obj_modify(dvqp->obj, in,
				     sizeof(in), out, sizeof(out));
	if (ret) {
		ERR("Failed to move qp %d to INIT state, port_num %d\n",
		    dvqp->qpn, ppc->port_num);
		return ret;
	}

	INFO("qp %d moved to INIT state, port_num %d\n", dvqp->qpn, ppc->port_num);
	return 0;
}

/* FIXME: For RoCE currently dmac is hard-coded */
//static uint8_t dmac[6] = {0x7c, 0xfe, 0x90, 0xcb, 0x74, 0x6e};
static uint8_t dmac[6] = {0xec, 0x0d, 0x9a, 0x8a, 0x28, 0x2a};
static int dvqp_init2rtr(const struct pp_context *ppc,
			 const struct pp_exchange_info *peer,
			 int my_sgid_idx, struct pp_dv_qp *dvqp)
{
	uint32_t out[DEVX_ST_SZ_DW(init2rtr_qp_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(init2rtr_qp_in)] = {};
	void *qpc = DEVX_ADDR_OF(init2rtr_qp_in, in, qpc);
	void *pri_path =  DEVX_ADDR_OF(qpc, qpc, primary_address_path);
	int ret;

	DEVX_SET(init2rtr_qp_in, in, opcode, MLX5_CMD_OP_INIT2RTR_QP);
	DEVX_SET(init2rtr_qp_in, in, qpn, dvqp->qpn);

	DEVX_SET(qpc, qpc, mtu, IBV_MTU_1024);
	DEVX_SET(qpc, qpc, log_msg_max, 20);
	DEVX_SET(qpc, qpc, remote_qpn, peer->qpn);
	DEVX_SET(qpc, qpc, next_rcv_psn, peer->psn);

	if (ppc->port_attr.link_layer == IBV_LINK_LAYER_ETHERNET) {
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rmac_47_32),
		       dmac, sizeof(dmac));
		memcpy(DEVX_ADDR_OF(qpc, qpc, primary_address_path.rgid_rip),
		       peer->gid.raw, sizeof(peer->gid.raw));
		//DEVX_SET(qpc, qpc, primary_address_path.src_addr_index, peer->rc.sgid_idx);
		DEVX_SET(qpc, qpc, primary_address_path.src_addr_index, my_sgid_idx);
		DEVX_SET(qpc, qpc, primary_address_path.udp_sport, 0xccdd);
		DEVX_SET(ads, pri_path, hop_limit, 64);
	} else {		/* IB */
		DEVX_SET(ads, pri_path, rlid, peer->lid);
	}
	//DEVX_SET(ads, pri_path, stat_rate, 0);

	DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, ppc->port_num);
	DEVX_SET(qpc, qpc, min_rnr_nak, 12);

	ret = mlx5dv_devx_obj_modify(dvqp->obj, in,
				     sizeof(in), out, sizeof(out));
	if (ret) {
		ERR("Failed to move qp %d to RTR state, dmac %02x:%02x:%02x:%02x:%02x:%02x\n",
		    dvqp->qpn, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5]);
		return ret;
	}

	INFO("qp %d moved to RTR state, dmac(hard-coded) %02x:%02x:%02x:%02x:%02x:%02x, peer.qpn %d\n",
	     dvqp->qpn, dmac[0], dmac[1], dmac[2], dmac[3], dmac[4], dmac[5], peer->qpn);
	return 0;
}

static int dvqp_rtr2rts(const struct pp_context *ppc, uint32_t my_sq_psn,
			struct pp_dv_qp *dvqp)
{
	uint32_t out[DEVX_ST_SZ_DW(rtr2rts_qp_out)] = {};
	uint32_t in[DEVX_ST_SZ_DW(rtr2rts_qp_in)] = {};
	void *qpc = DEVX_ADDR_OF(rtr2rts_qp_in, in, qpc);
	void *pri_path =  DEVX_ADDR_OF(qpc, qpc, primary_address_path);
	int ret;

	DEVX_SET(rtr2rts_qp_in, in, opcode, MLX5_CMD_OP_RTR2RTS_QP);
	DEVX_SET(rtr2rts_qp_in, in, qpn, dvqp->qpn);

	DEVX_SET(qpc, qpc, log_ack_req_freq, 0);
	DEVX_SET(qpc, qpc, retry_count, 7);
	DEVX_SET(qpc, qpc, rnr_retry, 7);
	DEVX_SET(qpc, qpc, next_send_psn, my_sq_psn);
	DEVX_SET(qpc, qpc, primary_address_path.vhca_port_num, ppc->port_num);

	DEVX_SET(qpc, qpc, log_sra_max, 20);
	DEVX_SET(ads, pri_path, ack_timeout, 14);

	ret = mlx5dv_devx_obj_modify(dvqp->obj, in,
				     sizeof(in), out, sizeof(out));
	if (ret) {
		ERR("dv: Failed to move qp %d to RTS state\n", dvqp->qpn);
		return ret;
	}

	INFO("qp %d moved to RTS state\n", dvqp->qpn);
	return 0;
}

int pp_move2rts_dv(struct pp_context *ppc, struct pp_dv_qp *dvqp,
		     int my_sgid_idx, uint32_t my_sq_psn,
		     struct pp_exchange_info *peer)
{
	int ret;

	ret = dvqp_rst2init(ppc, dvqp);
	if (ret)
		return ret;

	ret = dvqp_init2rtr(ppc, peer, my_sgid_idx, dvqp);
	if (ret)
		return ret;

	return dvqp_rtr2rts(ppc, my_sq_psn, dvqp);
}

static void set_raddr_seg(struct mlx5_wqe_raddr_seg *rseg,
			  void *remote_addr, uint32_t rkey)
{
	rseg->raddr = htobe64((uint64_t)remote_addr);
	rseg->rkey = htobe32(rkey);
	rseg->reserved = 0;
}

static void post_send_db(struct pp_dv_qp *dvqp, int size, void *ctrl)
{
	/*
	 * Make sure that descriptors are written before
	 * updating doorbell record and ringing the doorbell
	 */
	udma_to_device_barrier();
	dvqp->db[MLX5_SND_DBR] = htobe32(dvqp->sq.cur_post & 0xffff);

	udma_to_device_barrier();
	mmio_write64_be((uint8_t *)dvqp->uar->reg_addr, *(__be64 *)ctrl);

	/* FIXME: Somehow it doesn't work without this usleep */
	usleep(100);
}

static void post_send_one(const struct pp_context *ppc, struct pp_dv_qp *dvqp,
			  struct pp_exchange_info *peer, int id,
			  int opcode, uint32_t send_flags)

{
	void *qend = dvqp->sq.qend, *seg;
	struct mlx5_wqe_ctrl_seg *ctrl;
	uint32_t imm = 0x50607080 + id;
	int idx, size = 0;

	idx = dvqp->sq.cur_post & (dvqp->sq.wqe_cnt - 1);

	ctrl = dvqp->sq_start + (idx << MLX5_SEND_WQE_SHIFT);
	*(uint32_t *)((void *)ctrl + 8) = 0;
	ctrl->imm = htobe32(imm);
	ctrl->fm_ce_se = send_flags & IBV_SEND_SIGNALED ? MLX5_WQE_CTRL_CQ_UPDATE : 0;

	seg = ctrl;
	seg += sizeof(*ctrl);
	size = sizeof(*ctrl) / 16;

	if ((opcode == MLX5_OPCODE_RDMA_WRITE_IMM) ||
	    (opcode == MLX5_OPCODE_RDMA_WRITE) ||
	    ((opcode == MLX5_OPCODE_RDMA_READ))) {
		set_raddr_seg(seg, peer->addr[id], peer->mrkey[id]);
		seg += sizeof(struct mlx5_wqe_raddr_seg);
		size += sizeof(struct mlx5_wqe_raddr_seg) / 16;
	}

//	printf("=DEBUG:%s:%d: curpost %d idx %d ctrl %p seg %p qend %p\n", __func__, __LINE__,
//	       dvqp->sq.cur_post, idx, ctrl, seg, dvqp->sq.qend);

	if (unlikely(seg == qend))
		seg = dvqp->sq_start;
	mlx5dv_set_data_seg(seg, ppc->mrbuflen, ppc->mr[id]->lkey, (uint64_t)ppc->mrbuf[id]);
	size += sizeof(struct mlx5_wqe_data_seg) / 16;

	ctrl->opmod_idx_opcode =
		htobe32(((dvqp->sq.cur_post & 0xffff) << 8) | opcode);
	ctrl->qpn_ds = htobe32(size | (dvqp->qpn << 8));

	post_send_db(dvqp, size, ctrl);

	dvqp->sq.cur_post += DIV_ROUND_UP(size * 16, MLX5_SEND_WQE_BB);
}

int pp_dv_post_send(const struct pp_context *ppc, struct pp_dv_qp *dvqp,
		    struct pp_exchange_info *peer, unsigned int num_post,
		    int opcode, uint32_t send_flags)
{
	int i;

	if (!num_post ||
	    (num_post > dvqp->sq.max_post) || (num_post > PP_MAX_WR)) {
		ERR("Invalid num_post %d (max %d)\n", num_post, dvqp->sq.max_post);
		return EINVAL;
	}

	if (send_flags & IBV_SEND_INLINE) {
		ERR("send flag 0x%x: IBV_SEND_INLINE is not supported\n", send_flags);
		return EINVAL;
	}

	for (i = 0; i < num_post; i++)
		post_send_one(ppc, dvqp, peer, i, opcode, send_flags);

	return 0;
}
