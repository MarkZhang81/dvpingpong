#include "pp_common.h"
#include "pp_verb.h"

//#define SERVER_IP "10.237.1.205"
#define SERVER_IP "192.168.2.5"

static char ibv_devname[100] = "rocep8s0f0";
static int client_sgid_idx = 3;

#define PP_VERB_OPCODE_CLIENT IBV_WR_SEND_WITH_IMM
//#define PP_VERB_OPCODE_CLIENT IBV_WR_RDMA_WRITE_WITH_IMM

#define PP_SEND_WRID_CLIENT  0x1000
#define PP_RECV_WRID_CLIENT  0x4000

static struct pp_verb_ctx ppv_ctx;
static struct pp_exchange_info server = {};

static void dump_buf(char *prompt, unsigned char *p, uint64_t len)
{
	if (!p)
		return;

	if (len <= 32)
		printf("  %s(len 0x%lx): %s\n", prompt, len, p);
	else {
		p[16] = '\0';
		printf("  %s(len 0x%lx): %s...%s\n", prompt, len, p, p + len - 16);
	}
}

struct ibv_mr *dest, *src;
void *dest_addr, *src_addr;
uint64_t length = 256;
static void do_memcpy(struct pp_verb_ctx *ppv)
{
	struct mlx5dv_qp_ex *mqpx = ppv->cqqp.mqpx;

	//src_addr = malloc(length);
	//dest_addr = malloc(length);
	src_addr = memalign(4096, length);
	dest_addr = memalign(4096, length);
	if (!src_addr || !dest_addr) {
		ERR("malloc: %p %p\n", src_addr, dest_addr);
		abort();
	}
	src = ibv_reg_mr(ppv->ppc.pd, src_addr, length, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	dest = ibv_reg_mr(ppv->ppc.pd, dest_addr, length, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_READ | IBV_ACCESS_REMOTE_WRITE);
	if (!src || !dest) {
		ERR("ibv_reg_mr: %p %p\n", src, dest);
		abort();
	}

	mem_string(src_addr, length);
	memset(dest_addr, 0, length);

	INFO("src_addr %p(lkey 0x%x), dest_addr %p(lkey 0x%x), length ox%lx\n",
	     src_addr, src->lkey, dest_addr, dest->lkey, length);
	dump_buf("src_buf", src_addr, length);
	mlx5dv_wr_memcpy(mqpx, dest->lkey, (uint64_t)dest_addr, src->lkey, (uint64_t)src_addr, length);
}

static void do_memcpy_verify(struct pp_verb_ctx *ppv)
{
	//unsigned char *p = dest_addr;

	dump_buf("dest_buf", dest_addr, length);
	/*
	if (!p)
		return;

	if (length <= 32)
		printf("  dest_buf(len 0x%lx): %s\n", length, p);
	else {
		p[16] = '\0';
		printf("  dest_buf(len 0x%lx): %s...%s\n", length, p, p + length - 16);
	}
	*/
}

static int client_traffic_verb(struct pp_verb_ctx *ppv)
{
	struct ibv_recv_wr wrr[PP_MAX_WR] = {}, *bad_wr_recv;
	struct ibv_send_wr wrs[PP_MAX_WR] = {};
	struct ibv_sge sglists[PP_MAX_WR] = {};
	int max_wr_num = PP_MAX_WR, send_wr_num = 0, recv_wr_num = 0, wid = 0, ret;
	struct ibv_qp_ex *qpx = ppv->cqqp.qpx;

	DBG("Pause 1sec ");
	sleep(1);		/* Wait until server side is ready to recv */
	DBG("Do post_send %d messages with length 0x%lx..\n", max_wr_num, ppv->ppc.mrbuflen);

	prepare_send_wr_verb(ppv, wrs, sglists, &server, max_wr_num,
			     PP_SEND_WRID_CLIENT, PP_VERB_OPCODE_CLIENT, true);

	ibv_wr_start(qpx);

	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	qpx->wr_flags = IBV_SEND_SIGNALED;
	ibv_wr_send_imm(qpx, htobe32(0x10203040));
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid], ppv->ppc.mrbuflen);
	send_wr_num++;
	recv_wr_num++;
	wid++;

	/* memcpy_wqe */
	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	do_memcpy(ppv);
	send_wr_num++;
	wid++;

	qpx->wr_id = PP_SEND_WRID_CLIENT + wid;
	qpx->wr_flags = IBV_SEND_SIGNALED;
	// need to do "wid-1" if there's a memcpy_wqe
	/*ibv_wr_rdma_write_imm(qpx, server.mrkey[wid],
	  (uint64_t)server.addr[wid], be32toh(0x50607080)); */
	ibv_wr_send_imm(qpx,be32toh(0x50607080));
	ibv_wr_set_sge(qpx, ppv->ppc.mr[wid]->lkey, (uint64_t)ppv->ppc.mrbuf[wid], ppv->ppc.mrbuflen);
	send_wr_num++;
	recv_wr_num++;

	ret = ibv_wr_complete(qpx);
	if (ret) {
		ERR("Fatal ret %d\n", ret);
		abort();
	}

	ret = poll_cq_verb(ppv, send_wr_num, false);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	do_memcpy_verify(ppv);

	INFO("Send done, now recving reply...\n");
	prepare_recv_wr_verb(ppv, wrr, sglists, recv_wr_num, PP_RECV_WRID_CLIENT);
	/* 2. Get "pong" with same data */
	ret = ibv_post_recv(ppv->cqqp.qp, wrr, &bad_wr_recv);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", recv_wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, recv_wr_num, true);
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
