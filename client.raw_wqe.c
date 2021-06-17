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
//#define PP_IBV_ACCESS (IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ)
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

#if 0
static struct ibv_mr *umr_mr;
static unsigned char *umr_buf;
static ssize_t umr_buflen = 1024;
int umr_test(struct pp_verb_ctx *ppv)
{
	struct ibv_send_wr wr = {}, *bad_wr_send;
	struct ibv_sge sglist = {};
	struct ibv_wc wc = {};
	int ret;

	umr_buf = memalign(sysconf(_SC_PAGESIZE), umr_buflen);
	umr_mr = ibv_reg_mr(ppv->ppc.pd, umr_buf, umr_buflen,
			    IBV_ACCESS_LOCAL_WRITE);
	assert(umr_mr);

	mem_string(umr_buf, umr_buflen);
	sglist.lkey = umr_mr->lkey;
	sglist.addr = (uint64_t)umr_buf;
	sglist.length = umr_buflen;
	wr.next = NULL;
	wr.wr_id = 100;
	wr.sg_list = &sglist;
	wr.num_sge = 1;
	wr.imm_data = 0x12345678;
	wr.opcode = IBV_WR_RDMA_WRITE_WITH_IMM;
	wr.send_flags = IBV_SEND_SIGNALED;
	wr.wr.rdma.remote_addr = (uint64_t)server.addr[0];
	wr.wr.rdma.rkey = server.mrkey[0];

	ret = ibv_post_send(ppv->cqqp.qp, &wr, &bad_wr_send);
	if (ret) {
		ERR("ibv_post_send failed %d\n", ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, 1, false);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	if (wc.status != IBV_WC_SUCCESS) {
		ERR("Failed status %s(%x) for wr_id 0x%x\n",
		    ibv_wc_status_str(wc.status), wc.status,
		    (int)wc.wr_id);
		return -1;
	}

	INFO("umr test done\n");
	return 0;
}
#endif

struct mlx5dv_mkey_init_attr init_attr;
struct mlx5dv_mkey *dv_mkey;

/*
qpn: 0x3b3
(gdb) p /x *dv_mkey
$3 = {lkey = 0xc800, rkey = 0xc800}
	(gdb) p /x sge
	$5 = {addr = 0x7ffff7b70000, length = 0x10003f, lkey = 0xab7a6}
[idx = 000000]   00000025 0003b30c 00000008 0000c800
                 80000000 00040000 00000000 203c0001
                 00000000 00000000 00000000 00000000
                 00000000 00000000 00000000 00000000
[idx = 0x0001]   00003000 ffffff00 00000000 00000000
                 00000000 00000000 00000000 0010003f
                 00000000 00000000 00000000 00000000
                 00000000 00000000 00000000 00000000
[idx = 0x0002]   0010003f 000ab7a6 00007fff f7b70000
                 00000000 00000000 00000000 00000000
                 00000000 00000000 00000000 00000000
                 00000000 00000000 00000000 00000000
[idx = 0x0003]   00000000 00000000 00000000 00000000    <---- ci (0x3), pi (0x3)
*/
static int umr_update_access_flags(struct pp_verb_ctx *ppv, int n)
{
	struct ibv_qp_ex *qpx = ppv->cqqp.qpx;
	struct mlx5dv_qp_ex *dvqp = ppv->cqqp.mqpx;
	struct ibv_sge sge = {};
	int i, ret = 0;

	struct mlx5dv_mkey_conf_attr mkey_attr = {};
	init_attr.pd = ppv->ppc.pd;
	init_attr.create_flags = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT;
	init_attr.max_entries = 1;
	dv_mkey = mlx5dv_create_mkey(&init_attr);

	for (i = 0; i < n; i++) {
		sge.addr = (uint64_t)ppv->ppc.mrbuf[i];
		sge.length = ppv->ppc.mrbuflen;
		sge.lkey = ppv->ppc.mr[i]->lkey;

		qpx->wr_id = 0x12345;
		qpx->wr_flags = IBV_SEND_INLINE;

		ibv_wr_start(qpx);
		mlx5dv_wr_mkey_configure(dvqp, dv_mkey, 2, &mkey_attr);
		mlx5dv_wr_set_mkey_access_flags(dvqp, IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
		mlx5dv_wr_set_mkey_layout_list(dvqp, 1, &sge);
		ret = ibv_wr_complete(qpx);
		printf("=DEBUG:%s:%d: i %d ret %d\n", __func__, __LINE__, i, ret);

		ret = poll_cq_verb(ppv, 1, false);
		printf("=DEBUG:%s:%d: poll_cq %d\n", __func__, __LINE__, ret);
	}

	return ret;
}

#define ALIGN(x, log_a) ((((x) + (1 << (log_a)) - 1)) & ~((1 << (log_a)) - 1))
static inline __be16 get_klm_octo(int nentries)
{
	return htobe16(ALIGN(nentries, 3) / 2);
}


/*
$  sudo wqdump -d 00:06.00 --source Snd --qp 0x3b5  --dump WQ --num 4 --format raw
==============================================================================================================
Dump of SQ with gvmi=0x0, qn=0x3b5. 4 entries, 64 bytes each. connected to CQ 0x3a.  ts: 0  pi 0x3   ci 0x3
==============================================================================================================
	[idx = 000000]   00000025 0003b50c 00000008 0000e000
	                 90000000 00040000 00000000 201c2041
	                 00000000 00000000 00000000 00000000
	                 00000000 00000000 00000000 00000000
	[idx = 0x0001]   00003800 0003b572 00000000 00000000
	                 00007fff f7b70000 00000000 0010003f
	                 00000000 00000000 00000000 00000000
	                 00000000 00000000 00000000 00000000
	[idx = 0x0002]   0010003f 00148372 00007fff f7b70000
	                 00000000 00000000 00000000 00000000
	                 00000000 00000000 00000000 00000000
	                 00000000 00000000 00000000 00000000
	[idx = 0x0003]   00000000 00000000 00000000 00000000    <---- ci (0x3), pi (0x3)
	                 00000000 00000000 00000000 00000000
	                 00000000 00000000 00000000 00000000
	                 00000000 00000000 00000000 00000000
*/
static int umr_update_access_flags_raw_wqe(struct pp_verb_ctx *ppv, int n)
{
	struct ibv_qp_ex *qpx = ppv->cqqp.qpx;
	struct mlx5dv_qp_ex *dvqp = ppv->cqqp.mqpx;
	struct mlx5_wqe_ctrl_seg *ctrl;
	struct mlx5_wqe_umr_ctrl_seg *umr;
	struct mlx5_wqe_mkey_context_seg *mkeyc;
	int i, ds, ret;
	char wqe[256] = {};
	struct ibv_sge sge = {};

	init_attr.pd = ppv->ppc.pd;
	init_attr.create_flags = MLX5DV_MKEY_INIT_ATTR_FLAGS_INDIRECT;
	init_attr.max_entries = 1;
	dv_mkey = mlx5dv_create_mkey(&init_attr);


        union umr_data_seg {
                struct mlx5_wqe_umr_klm_seg     klm;
                uint8_t                         reserved[64];
        } *data;

	ctrl = (struct mlx5_wqe_ctrl_seg *)wqe;
	umr = (struct mlx5_wqe_umr_ctrl_seg *)(ctrl + 1);
	mkeyc = (struct mlx5_wqe_mkey_context_seg *)(umr + 1);
	data = (union umr_data_seg *)(mkeyc + 1);

	for (i = 0; i < n; i++) {

		sge.addr = (uint64_t)ppv->ppc.mrbuf[i];
		sge.length = ppv->ppc.mrbuflen;
		sge.lkey = ppv->ppc.mr[i]->lkey;

		memset(wqe, 0, sizeof(wqe));

		ibv_wr_start(qpx);

		ctrl->fm_ce_se = MLX5_WQE_CTRL_CQ_UPDATE;
		ctrl->opmod_idx_opcode = htobe32(MLX5_OPCODE_UMR);
		ctrl->imm = htobe32(dv_mkey->lkey);
		ds = sizeof(*ctrl) / 16;

		umr->flags = MLX5_WQE_UMR_CTRL_FLAG_TRNSLATION_OFFSET |
			MLX5_WQE_UMR_CTRL_FLAG_INLINE;
		umr->klm_octowords = get_klm_octo(1);
		umr->mkey_mask = htobe64(MLX5_WQE_UMR_CTRL_MKEY_MASK_FREE |
					 MLX5_WQE_UMR_CTRL_MKEY_MASK_MKEY);
		umr->mkey_mask |= htobe64(MLX5_WQE_UMR_CTRL_MKEY_MASK_LEN |
					  MLX5_WQE_UMR_CTRL_MKEY_MASK_START_ADDR |
					  MLX5_WQE_UMR_CTRL_MKEY_MASK_ACCESS_LOCAL_WRITE |
					  MLX5_WQE_UMR_CTRL_MKEY_MASK_ACCESS_REMOTE_READ |
					  MLX5_WQE_UMR_CTRL_MKEY_MASK_ACCESS_REMOTE_WRITE);
		ds += sizeof(*umr) / 16;

		mkeyc->qpn_mkey = htobe32((ppv->ppc.mr[i]->lkey & 0xFF) | (ppv->cqqp.qp->qp_num << 8));
		mkeyc->access_flags = MLX5_WQE_MKEY_CONTEXT_ACCESS_FLAGS_LOCAL_WRITE |
			MLX5_WQE_MKEY_CONTEXT_ACCESS_FLAGS_REMOTE_WRITE |
			MLX5_WQE_MKEY_CONTEXT_ACCESS_FLAGS_REMOTE_READ;
		mkeyc->start_addr = htobe64((uint64_t)ppv->ppc.mrbuf[i]);
		mkeyc->len = htobe64(ppv->ppc.mrbuflen);

		ds += sizeof(*mkeyc) / 16;

		data->klm.byte_count = htobe32(ppv->ppc.mrbuflen);
		data->klm.mkey = htobe32(ppv->ppc.mr[i]->lkey);
		data->klm.address = htobe64((uint64_t)ppv->ppc.mrbuf[i]);

		/*
		printf("=DEBUG:%s:%d: data %p &data->klm %p &data->klm + 1 %p size %ld - %ld = %ld\n", __func__, __LINE__,
		       data, &data->klm, &data->klm + 1, sizeof(data->reserved), sizeof(data->klm), sizeof(data->reserved) - sizeof(data->klm));
		*/
		memset(&data->klm + 1, 0, sizeof(data->reserved) -
		       sizeof(data->klm));

		ds += (sizeof(*data) / 16);

		ctrl->qpn_ds = htobe32(ds | (ppv->cqqp.qp->qp_num << 8));

		ppv->cqqp.qpx->wr_id = 101 + i;
		mlx5dv_wr_raw_wqe(ppv->cqqp.mqpx, wqe);

		ret = ibv_wr_complete(qpx);
		printf("=DEBUG:%s:%d: n %d ret %d ds %d\n", __func__, __LINE__, n ,ret, ds);

		ret = poll_cq_verb(ppv, 1, false);
		printf("=DEBUG:%s:%d: poll_cq %d\n", __func__, __LINE__, ret);
	}

	return 0;
}

static int poll_cq_lazy(struct pp_verb_ctx *ppv, int wc_num)
{
	struct ibv_cq_ex *cq_ex = ppv->cqqp.cq_ex;
	struct ibv_poll_cq_attr attr = {};
	int id = 0, ret;

	do {
		ret = ibv_start_poll(cq_ex, &attr);
	} while (ret == ENOENT);
	if (ret) {
		ERR("poll CQ failed %d\n", ret);
		return ret;
	}
	INFO("CQ %d: wr_id 0x%lx status %d opcode 0x%x\n",
	     id, cq_ex->wr_id, cq_ex->status, ibv_wc_read_opcode(cq_ex));
	id++;

	while (id < wc_num) {
		do {
			ret = ibv_next_poll(cq_ex);
		} while (ret == ENOENT);
		if (ret) {
			ERR("Poll CQ failed %d %d\n", ret, id);
			goto out;
		}
		INFO("CQ %d: wr_id 0x%lx status %d opcode 0x%x\n",
		     id, cq_ex->wr_id, cq_ex->status, ibv_wc_read_opcode(cq_ex));
		id++;
	}

out:
	ibv_end_poll(cq_ex);
	return ret;
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

	//umr_test(ppv);
	//for (i = 0; i < PP_MAX_WR; i++) {
	for (i = 0; i < 3; i++) {
			mem_string(ppv->ppc.mrbuf[i], ppv->ppc.mrbuflen);
			*ppv->ppc.mrbuf[i] = i % 16 + '0';
	}

	//umr_update_access_flags(ppv, 1);
	//umr_update_access_flags_raw_wqe(ppv, 1);

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
		if (ret == ENOMEM) {
			ERR("Fatal: nomem\n");
			abort();
		}

		ERR("ibv_wr_complete failed %d\n", ret);
		exit(1);
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

#if 0
	INFO("Poll CQ (normal mode)\n");
	ret = poll_cq_verb(ppv, wr_num, false);
#else
	INFO("Poll CQ (lazy mode)\n");
	ret = poll_cq_lazy(ppv, wr_num);
#endif
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

	ret = pp_ctx_init(&ppv_ctx.ppc, ibv_devname, 0, NULL, PP_IBV_ACCESS);
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
