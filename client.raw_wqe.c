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
static void prepare_sge_list(struct pp_verb_ctx *ppv, struct ibv_sge sglists[], int num_sge)
{
	int i;

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

static int client_traffic_verb(struct pp_verb_ctx *ppv)
{
	struct ibv_sge sglists[PP_NUM_SGE_PER_WQE] = {};
	struct ibv_recv_wr wrr[PP_MAX_WR] = {}, *bad_wr_recv;
	struct ibv_qp_ex *qpx = ppv->cqqp.qpx;
	int wid, wr_num, i, ret;
	//int num_sge = PP_NUM_SGE_PER_WQE;
	int num_sge = 8;

	for (i = 0; i < PP_MAX_WR; i++) {
			mem_string(ppv->ppc.mrbuf[i], ppv->ppc.mrbuflen);
			*ppv->ppc.mrbuf[i] = i % 16 + '0';
	}

	ibv_wr_start(qpx);

	wid = 0;
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	ibv_wr_send(qpx);
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid],
		       ppv->ppc.mrbuflen);

	wid ++;
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	ibv_wr_send_imm(qpx, htobe32(0x10203040));
	prepare_sge_list(ppv, sglists, num_sge);
	ibv_wr_set_sge_list(qpx, num_sge, sglists);
	//ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid], ppv->ppc.mrbuflen);

	// wid++;
	//mlx5dv_wr_build_raw_wqe();

	wid ++;
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	ibv_wr_rdma_write_imm(qpx, server.mrkey[wid], (uint64_t)server.addr[wid], be32toh(0x50607080));
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid],
		       ppv->ppc.mrbuflen);

	ibv_wr_complete(qpx);

	wr_num = wid + 1;
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


#if 0
void prepare_one_wqe(struct pp_verb_ctx *ppv, struct ibv_send_wr *wr, int wr_id,
		     struct ibv_sge sglist[])
//struct ibv_sge *sglists, struct ibv_mr *mrs[], unsigned char *bufs[])
{
	int start = wr_id * PP_NUM_SGE_PER_WQE;

	for (i = 0; i < PP_NUM_SGE_PER_WQE; i++) {
		mem_string(ppv->ppc.mrbuf[start + i], ppv->ppc.mrbuflen);
		*mrbuf[i] = (start + i) % 16 + '0';

		sglist[start + i].lkey = ppv->ppc.mr[start + i]->lkey;
		sglist[start + i].addr = (uint64_t)ppv->ppc.mrbuf[start + i];
		sglist[start + i].length = ppv->ppc.mrbuflen;
	}
	wr->wr_id = PP_SEND_WRID_CLIENT + wr_id;
	wr->sg_list = sglist
	wr->num_sge = PP_NUM_SGE_PER_WQE;

}

static int client_traffic_verb(struct pp_verb_ctx *ppv)
{
	struct ibv_send_wr wrs[PP_MAX_WR] = {}, *bad_wr_send;
	struct ibv_recv_wr wrr[PP_MAX_WR] = {}, *bad_wr_recv;
	struct ibv_sge sglists[PP_MAX_WR * PP_NUM_SGE_PER_WQE] = {};
	int wr_num, wr_id, sge_num = 1, ret;

	DBG("Pause 1sec ");
	sleep(1);		/* Wait until server side is ready to recv */
	DBG("Do post_send %d messages with length 0x%lx..\n", max_wr_num, ppv->ppc.mrbuflen);

	ibv_wr_start(qpx);

	wr_id = 0;
	prepare_one_wqe(wrs + wr_id, sglists + wr_id * PP_NUM_SGE_PER_WQE,
			ppv->ppc.mr + wr_id * PP_NUM_SGE_PER_WQE,
			ppv->ppc.mrbuf + wr_id * PP_NUM_SGE_PER_WQE);

	wr_id++;
	prepare_one_raw_wqe(wrs + wr_id, sglists + wr_id * PP_NUM_SGE_PER_WQE);

	wr_id++;
	prepare_one_wqe(wrs + wr_id, sglists + wr_id * PP_NUM_SGE_PER_WQE);

	ret = ibv_wr_complete(qpx);

	ret = poll_cq_verb(ppv, max_wr_num, false);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	INFO("Send done, now recving reply...\n");

	wr_num = wr_id;
	memset(sglists, 0, sizeof(sglists));
	for (i = 0; i < PP_MAX_WR * PP_NUM_SGE_PER_WQE; i++)
		memset(ppv->ppc.mrbuf[i], 0, ppv->ppc.mrbuflen);

	prepare_recv_wr_verb(ppv, wrr, sglists, wr_num, PP_RECV_WRID_CLIENT, sge_num);
	/* 2. Get "pong" with same data */
	ret = ibv_post_recv(ppv->cqqp.qp, wrr, &bad_wr_recv);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", max_wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, max_wr_num, true);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	INFO("Client(verb) traffic test done\n");
	return 0;
}
#endif

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
