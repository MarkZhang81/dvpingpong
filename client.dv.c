#include "pp_common.h"
#include "pp_dv.h"


#define SERVER_IP "192.168.0.122"

static char ibv_devname[100] = "rocep8s0f0";
static int client_sgid_idx = 3;

//#define PP_DV_OPCODE_CLIENT IBV_WR_RDMA_WRITE_WITH_IMM /* IBV_WR_SEND_WITH_IMM */

#define PP_SEND_WRID_CLIENT  0x1000
#define PP_RECV_WRID_CLIENT  0x4000

static struct pp_dv_ctx ppdv;
static struct pp_exchange_info server = {};

static int client_traffic_dv(struct pp_dv_ctx *ppdv)
{
	//int opcode = MLX5_OPCODE_RDMA_WRITE_IMM;
	int opcode = MLX5_OPCODE_SEND_IMM;
	int num_post = PP_MAX_WR, i, ret;

	DBG("Pause 1sec before post send, opcode %d\n", opcode);
	sleep(1);

	for (i = 0; i < num_post; i++) {
		mem_string(ppdv->ppc.mrbuf[i], ppdv->ppc.mrbuflen);
		*ppdv->ppc.mrbuf[i] = i % ('z' - '0') + '0';
	}

	ret = pp_dv_post_send(&ppdv->ppc, &ppdv->qp, &server, num_post,
			      opcode, IBV_SEND_SIGNALED);
	if (ret)
		return ret;

	INFO("Send done, now recving reply...\n");
	return 0;
}

int main(int argc, char *argv[])
{
	int ret;

	if (argv[1]) {
		memset(ibv_devname, 0, sizeof(ibv_devname));
		strcpy(ibv_devname, argv[1]);
	}

	ret = pp_ctx_init(&ppdv.ppc, ibv_devname, 0, NULL);
	if (ret)
		return ret;

	ret = pp_create_cq_dv(&ppdv.ppc, &ppdv.cq);
	if (ret)
		goto out_create_cq;

	ret = pp_create_qp_dv(&ppdv.ppc, &ppdv.cq, &ppdv.qp);
	if (ret)
		goto out_create_qp;

	ret = pp_exchange_info(&ppdv.ppc, client_sgid_idx, ppdv.qp.qpn,
			       CLIENT_PSN, &server, SERVER_IP);
	if (ret)
		goto out_exchange;

	ret = pp_move2rts_dv(&ppdv.ppc, &ppdv.qp, client_sgid_idx,
			     CLIENT_PSN, &server);
	if (ret)
		goto out_exchange;

	ret = client_traffic_dv(&ppdv);

out_exchange:
	pp_destroy_qp_dv(&ppdv.qp);
out_create_qp:
	pp_destroy_cq_dv(&ppdv.cq);
out_create_cq:
	pp_ctx_cleanup(&ppdv.ppc);
	return ret;
}
