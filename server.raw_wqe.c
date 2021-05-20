#include "pp_common.h"
#include "pp_verb.h"

static char ibv_devname[100] = "mlx5_0";
static int server_sgid_idx = 3;

static struct pp_verb_ctx ppv_ctx;
static struct pp_exchange_info client = {};

#define PP_SEND_WRID_SERVER  0x10000000
#define PP_RECV_WRID_SERVER  0x20000000

#define PP_VERB_OPCODE_SERVER IBV_WR_RDMA_WRITE_WITH_IMM /* IBV_WR_SEND_WITH_IMM */


static int server_traffic_verb(struct pp_verb_ctx *ppv, int wr_num)
{
	struct ibv_send_wr wrs[PP_MAX_WR] = {}, *bad_wrs;
	struct ibv_recv_wr wrr[PP_MAX_WR] = {}, *bad_wrr;
	struct ibv_sge sglists[PP_MAX_WR] = {};
	int ret;
#if 1
	prepare_recv_wr_verb(ppv, wrr, sglists, wr_num, PP_RECV_WRID_SERVER);

	/* 1. Recv "ping" */
	ret = ibv_post_recv(ppv->cqqp.qp, wrr, &bad_wrr);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, wr_num, true);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

	sleep(1);
	INFO("Now sending reply (%d)...\n", wr_num);
	prepare_send_wr_verb(ppv, wrs, sglists, &client, wr_num,
			     PP_SEND_WRID_SERVER, PP_VERB_OPCODE_SERVER, false);
	ret = ibv_post_send(ppv->cqqp.qp, wrs, &bad_wrs);
	if (ret) {
		ERR("%d: ibv_post_send failed %d\n", wr_num, ret);
		return ret;
	}

	ret = poll_cq_verb(ppv, wr_num, false);
	if (ret) {
		ERR("poll_cq_verb failed\n");
		return ret;
	}

#else
	int msgid = 0, ret;
	while(1) {
		prepare_recv_wr_verb(ppv, wrr, sglists, 1, PP_RECV_WRID_SERVER);

		INFO("\nWaiting for msg %d...\n", msgid);
		ret = ibv_post_recv(ppv->cqqp.qp, wrr, &bad_wrr);
		if (ret) {
			ERR("%d: ibv_post_send failed %d\n", msgid, ret);
			return ret;
		}

		ret = poll_cq_verb(ppv, 1, true);
		if (ret) {
			ERR("poll_cq_verb failed\n");
			return ret;
		}
		sleep(1);
		INFO("Now sending reply...\n");
		prepare_send_wr_verb(ppv, wrs, sglists, &client, 1,
				     PP_SEND_WRID_SERVER, PP_VERB_OPCODE_SERVER, false);

		ret = ibv_post_send(ppv->cqqp.qp, wrs, &bad_wrs);
		if (ret) {
			ERR("%d: ibv_post_send failed %d\n", msgid, ret);
			return ret;
		}

		ret = poll_cq_verb(ppv, 1, false);
		if (ret) {
			ERR("poll_cq_verb failed\n");
			return ret;
		}

		msgid++;
	}
#endif
	INFO("Server traffic test done\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argv[1]) {
		memset(ibv_devname, 0, sizeof(ibv_devname));
		strcpy(ibv_devname, argv[1]);
	}
	INFO("IB device %s\n", ibv_devname);

	ret = pp_ctx_init(&ppv_ctx.ppc, ibv_devname, 0, NULL);
	if (ret)
		return ret;

	ret = pp_create_cq_qp_verb(&ppv_ctx.ppc, &ppv_ctx.cqqp);
	if (ret)
		goto out_create_cq_qp;

	ret = pp_exchange_info(&ppv_ctx.ppc, server_sgid_idx,
			       ppv_ctx.cqqp.qp->qp_num, SERVER_PSN, &client, NULL);
	if (ret)
		goto out_exchange;

	ret = pp_move2rts_verb(&ppv_ctx.ppc, ppv_ctx.cqqp.qp, server_sgid_idx,
			       SERVER_PSN, &client);
	if (ret)
		goto out_exchange;

	/* To match the wrap-around test; Check client_traffic_verb()
	 * Need to improve the server logic...
	 */
#if 0
	ret = server_traffic_verb(&ppv_ctx, 3);
	ret = server_traffic_verb(&ppv_ctx, 3);
	ret = server_traffic_verb(&ppv_ctx, 3);
	ret = server_traffic_verb(&ppv_ctx, 3);
	ret = server_traffic_verb(&ppv_ctx, 2);
#endif

	ret = server_traffic_verb(&ppv_ctx, 3);

out_exchange:
	pp_destroy_cq_qp_verb(&ppv_ctx.cqqp);
out_create_cq_qp:
	pp_ctx_cleanup(&ppv_ctx.ppc);
	return ret;
}
