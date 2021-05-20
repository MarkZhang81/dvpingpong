#include <arpa/inet.h>
#include <assert.h>

#include "pp_common.h"
#include "pp_verb.h"

#define SERVER_IP "10.237.1.205"
//#define SERVER_IP "192.168.60.205"

static char ibv_devname[100] = "mlx5_0";
static int client_sgid_idx = 3;

//#define PP_VERB_OPCODE_CLIENT IBV_WR_SEND_WITH_IMM
#define PP_VERB_OPCODE_CLIENT IBV_WR_RDMA_WRITE_WITH_IMM

#define PP_SEND_WRID_CLIENT  0x1000
#define PP_RECV_WRID_CLIENT  0x4000

static struct pp_verb_ctx ppv_ctx;
static struct pp_exchange_info server = {};

#define PP_NUM_SGE_PER_WQE  8
static void mem_sglist_buf(unsigned char *addr, ssize_t len)
{
	int i;

	for (i = 0; i < len; i++)
		addr[i] = i % 16 + '0';
}

#define PP_IBV_ACCESS (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE)
void prepare_sge_list(struct pp_verb_ctx *ppv, struct ibv_sge sglists[], int num_sge)
{
	int i;

	/* FIXME: need to do cleanup */
	for (i = 0; i < num_sge; i++) {
		struct ibv_mr *mr;
		sglists[i].length = PP_DATA_BUF_LEN/num_sge;
		sglists[i].addr = (uint64_t)memalign(sysconf(_SC_PAGESIZE), sglists[i].length);
		assert(sglists[i].addr);
		mem_sglist_buf((unsigned char *)sglists[i].addr, sglists[i].length);
		mr = ibv_reg_mr(ppv->ppc.pd, (void *)sglists[i].addr, sglists[i].length, PP_IBV_ACCESS);
		assert(mr);
		sglists[i].lkey = mr->lkey;
	}
}

void build_raw_wqe_send(struct pp_verb_ctx *ppv, void *wqe, uint32_t lkey,
			uint64_t addr, uint32_t len, uint32_t imm)
{
	struct mlx5_wqe_ctrl_seg *ctrl = (struct mlx5_wqe_ctrl_seg *)wqe;
	void *seg;
	int ds;

	//*(uint32_t *)((void *)ctrl + 8) = 0;
	ctrl->imm = htobe32(imm);
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ds = sizeof(*ctrl) / 16;
	seg = ctrl + 1;
	mlx5dv_set_data_seg(seg, len, lkey, addr);
	ds += sizeof(struct mlx5_wqe_data_seg) / 16;
	ctrl->opmod_idx_opcode = htobe32(MLX5_OPCODE_SEND_IMM);
	ctrl->qpn_ds = htobe32(ds | (ppv->cqqp.qp->qp_num << 8));
	printf("=DEBUG:%s:%d: lkey 0x%x addr 0x%lx len 0x%x qpn %d\n", __func__, __LINE__, lkey, addr, len, ppv->cqqp.qp->qp_num);
}

void build_raw_wqe_send_sglist(struct pp_verb_ctx *ppv, void *wqe,
			       struct ibv_sge *sglist, int num_sge, uint32_t imm)
{
	struct mlx5_wqe_ctrl_seg *ctrl = (struct mlx5_wqe_ctrl_seg *)wqe;
	struct mlx5_wqe_data_seg *dseg;
	int ds, i;

	ctrl->imm = htobe32(imm);
	ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
	ds = sizeof(*ctrl) / 16;
	dseg = (struct mlx5_wqe_data_seg *)(ctrl + 1);
	for (i = 0; i < num_sge; i++) {
		mlx5dv_set_data_seg(dseg, sglist[i].length, sglist[i].lkey, sglist[i].addr);
		dseg++;
		ds += sizeof(struct mlx5_wqe_data_seg) / 16;
	}
	ctrl->opmod_idx_opcode = htobe32(MLX5_OPCODE_SEND_IMM);
	ctrl->qpn_ds = htobe32(ds | (ppv->cqqp.qp->qp_num << 8));
	INFO("num_sge %d ds %d, qpn_ds = 0x%08x\n", num_sge, ds, ds | (ppv->cqqp.qp->qp_num << 8));
}

int post_msg(struct pp_verb_ctx *ppv, int wr_num)
{
	struct ibv_send_wr wrs[PP_MAX_WR] = {}, *bad_wr_send;
	struct ibv_recv_wr wrr[PP_MAX_WR] = {}, *bad_wr_recv;
	struct ibv_sge sglists[PP_MAX_WR] = {};
	int i, ret;

	sleep(1);
	for (i = 0; i < PP_MAX_WR; i++) {
			mem_string(ppv->ppc.mrbuf[i], ppv->ppc.mrbuflen);
			*ppv->ppc.mrbuf[i] = i % 16 + '0';
	}

	printf("=DEBUG:%s:%d: Now post %d msgs\n", __func__, __LINE__, wr_num);

	prepare_send_wr_verb(ppv, wrs, sglists, &server, wr_num,
			     PP_SEND_WRID_CLIENT, PP_VERB_OPCODE_CLIENT, true);

	/* 1. Send "ping" */
	ret = ibv_post_send(ppv->cqqp.qp, wrs, &bad_wr_send);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, wr_num, false);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	INFO("Send done, now recving reply...\n");
	prepare_recv_wr_verb(ppv, wrr, sglists, wr_num, PP_RECV_WRID_CLIENT);
	/* 2. Get "pong" with same data */
	ret = ibv_post_recv(ppv->cqqp.qp, wrr, &bad_wr_recv);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, wr_num, true);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	return 0;
}

static int client_traffic_verb(struct pp_verb_ctx *ppv)
{
	struct ibv_sge sglists[PP_NUM_SGE_PER_WQE] = {};
	struct ibv_recv_wr wrr[PP_MAX_WR] = {}, *bad_wr_recv;
	struct ibv_qp_ex *qpx = ppv->cqqp.qpx;
	int wid = 0, wr_num = 0, i, ret;
	//int num_sge = PP_NUM_SGE_PER_WQE;
	int num_sge = 8;

	/* This is for wrap-around test; We post enough WQEs so that
	 * mqp->cur_data will near the end of the sq.qend
	 *Note:
	 *  1. The server should match the code here
	 *  2. How many WQEs needs here is depended on qp_cap.max_*** in pp_common.c::pp_ctx_init()
	 */
#if 0
	post_msg(ppv, 3);
	post_msg(ppv, 3);
	post_msg(ppv, 3);
	post_msg(ppv, 3);
	post_msg(ppv, 2);
#endif

	for (i = 0; i < PP_MAX_WR; i++) {
			mem_string(ppv->ppc.mrbuf[i], ppv->ppc.mrbuflen);
			*ppv->ppc.mrbuf[i] = i % 16 + '0';
	}

	ibv_wr_start(qpx);

	printf("\n\n");
	/* First WR */
	wid = 0;
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	wr_num++;
#if 0
	INFO("ibv_wr_send (wr %d)....\n", wr_num);
	qpx->wr_flags = IBV_SEND_SIGNALED;
	ibv_wr_send(qpx);
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid],
		       ppv->ppc.mrbuflen);
#else
	INFO("raw_wqe send....\n");
	char wqe[64] = {};
	build_raw_wqe_send(ppv, wqe, ppv->ppc.mr[wid]->lkey,
			   (uint64_t)ppv->ppc.mrbuf[wid], ppv->ppc.mrbuflen, 0x10203041);
	mlx5dv_wr_raw_wqe(ppv->cqqp.mqpx, wqe);
#endif
	printf("\n");

	/* Second WR, with num_sge > 1 */
	wid++;
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	prepare_sge_list(ppv, sglists, num_sge);
	wr_num++;
#if 0
	INFO("rw_send_imm (wr %d)....\n", wr_num);
	ibv_wr_send_imm(qpx, htobe32(0x10203043));
	//ibv_wr_set_sge_list(qpx, num_sge, sglists);
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid],
		       ppv->ppc.mrbuflen);
#else
	INFO("raw_wqe_send with sglist (wr %d, num_sge %d cap.max_send_sge %d)....\n", wr_num, num_sge, ppv->ppc.cap.max_send_sge);
	char wqe2[512] = {};
	build_raw_wqe_send_sglist(ppv, wqe2, sglists, num_sge, 0x10203043);
	mlx5dv_wr_raw_wqe(ppv->cqqp.mqpx, wqe2);
#endif
	printf("\n");

	/* Third WR, mix use with ibv_wr_* */
	wid ++;
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	qpx->wr_flags = IBV_SEND_SIGNALED;
	ibv_wr_rdma_write_imm(qpx, server.mrkey[wid], (uint64_t)server.addr[wid], be32toh(0x50607080));
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid],
		       ppv->ppc.mrbuflen);
	wr_num++;
	printf("\n");

	ret = ibv_wr_complete(qpx);
	if (ret) {
		if (ret == ENOMEM)
			abort();

		ERR("ibv_wr_complete failed %d\n", ret);
		wr_num = 0;

		/* Test another round post after failure */
		ibv_wr_start(qpx);
		wid = 0;
		qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
		wr_num++;
#if 0
		qpx->wr_flags = IBV_SEND_SIGNALED;
		ibv_wr_send(qpx);
		ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid],
			       ppv->ppc.mrbuflen);
#else
		char wqe[64] = {};
		build_raw_wqe_send(ppv, wqe, ppv->ppc.mr[wid]->lkey,
				   (uint64_t)ppv->ppc.mrbuf[wid], ppv->ppc.mrbuflen, 0x10203041);
		mlx5dv_wr_raw_wqe(ppv->cqqp.mqpx, wqe);
#endif
		ibv_wr_complete(qpx);
	}

	ret = poll_cq_verb(ppv, wr_num, false);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	/* Recv */
	INFO("Send done(wr_num %d), now recving reply...\n", wr_num);
	memset(sglists, 0, sizeof(sglists));
	for (i = 0; i < PP_MAX_WR; i++)
		memset(ppv->ppc.mrbuf[i], 0, ppv->ppc.mrbuflen);

	prepare_recv_wr_verb(ppv, wrr, sglists, wr_num, PP_RECV_WRID_CLIENT);
	/* 2. Get "pong" with same data */
	ret = ibv_post_recv(ppv->cqqp.qp, wrr, &bad_wr_recv);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, wr_num, true);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	INFO("Client(verb) traffic test done\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argv[1]) {
		memset(ibv_devname, 0, sizeof(ibv_devname));
		strcpy(ibv_devname, argv[1]);
	}
	INFO("IB device %s, server ip %s\n", ibv_devname, SERVER_IP);

	ret = pp_ctx_init(&ppv_ctx.ppc, ibv_devname, 0, NULL);
	if (ret)
		return ret;

	ret = pp_create_cq_qp_verb(&ppv_ctx.ppc, &ppv_ctx.cqqp);
	if (ret)
		goto out_create_cq_qp;

	ret = pp_exchange_info(&ppv_ctx.ppc, client_sgid_idx, ppv_ctx.cqqp.qp->qp_num,
			       CLIENT_PSN, &server, SERVER_IP);
	if (ret)
		goto out_exchange;

	ret = pp_move2rts_verb(&ppv_ctx.ppc, ppv_ctx.cqqp.qp, client_sgid_idx,
			       CLIENT_PSN, &server);
	if (ret)
		goto out_exchange;

	ret = client_traffic_verb(&ppv_ctx);

out_exchange:
	pp_destroy_cq_qp_verb(&ppv_ctx.cqqp);
out_create_cq_qp:
	pp_ctx_cleanup(&ppv_ctx.ppc);
	return ret;
}
