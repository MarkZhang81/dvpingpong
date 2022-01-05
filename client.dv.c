#include "pp_common.h"
#include "pp_dv.h"

#include "util/mmio.h"
#include "util/udma_barrier.h"

#define SERVER_IP "10.237.1.205"

static char ibv_devname[100] = "rocep175s0f0";
static int client_sgid_idx = 3;

//#define PP_DV_OPCODE_CLIENT IBV_WR_RDMA_WRITE_WITH_IMM /* IBV_WR_SEND_WITH_IMM */

#define PP_SEND_WRID_CLIENT  0x1000
#define PP_RECV_WRID_CLIENT  0x4000

static struct pp_dv_ctx ppdv;
static struct pp_exchange_info server = {};

struct mlx5dv_devx_event_channel *ech; /* cq_ech */

#define USE_CQ_EVENT 1

#ifdef USE_CQ_EVENT
static pthread_t unaffliated_event_tid, qp_event_tid;
struct mlx5dv_devx_event_channel *unaffliated_ech;
struct mlx5dv_devx_event_channel *qp_ech;
void *async_event_routine(void *arg)
{
	union {
		struct mlx5dv_devx_async_event_hdr event_resp;
		uint8_t buf[sizeof(struct mlx5dv_devx_async_event_hdr) + 128];
	} out;
	//struct pp_dv_ctx *ppdv = arg;
	struct mlx5dv_devx_event_channel *event_ech = arg;
	ssize_t sz;

	DBG("Start waiting for '%s' events......\n", event_ech == unaffliated_ech ? "unafflicated" : "qp");
	while (1) {
		sz = mlx5dv_devx_get_event(event_ech, &out.event_resp, sizeof(out.buf));
		if (sz < 0) {
			ERR("devx_get_event failed %ld, errno %d\n", sz, errno);
			return NULL;
		}

		DBG("Async event received!!!!!!!!!!!!!!!!!!!! sz %ld\n", sz);
	}

	return NULL;
}

static int listen_unaffliated_event(struct pp_dv_ctx *ppdv)
{
	//uint16_t event_nums[] = {1, 2, 3, 0x13, 0x14, 5, 7, 0x10, 0x11, 0x12, 0x27};
	uint16_t event_nums[] = {0x22};
	int ret;

	unaffliated_ech = mlx5dv_devx_create_event_channel(ppdv->ppc.ibctx,
							   MLX5DV_DEVX_CREATE_EVENT_CHANNEL_FLAGS_OMIT_EV_DATA);
	if (!unaffliated_ech) {
		ERR("create_event_channel failed %d", errno);
		return errno;
	}

	ret = mlx5dv_devx_subscribe_devx_event(unaffliated_ech, NULL,
					       sizeof(event_nums), event_nums, 0);
	if (ret) {
		ERR("unaffliateed subscribe_devx_event failed: %d, errno %d", ret, errno);
		goto out;
	}
	DBG("Unaffliated event channel created, fd %d\n", unaffliated_ech->fd);

	ret = pthread_create(&unaffliated_event_tid, NULL, async_event_routine, unaffliated_ech);
	if (ret) {
		ERR("pthread_create async_event_routine");
		goto out;
	}

	return 0;

out:
	mlx5dv_devx_destroy_event_channel(unaffliated_ech);
	return ret;
}

static int listen_qp_event(struct pp_dv_ctx *ppdv)
{
	uint16_t event_nums[] = {1, 2, 3, 0x13, 0x14, 5, 7, 0x10, 0x11, 0x12, 0x27};
	//uint16_t event_nums[] = {0x22};
	int ret;

	qp_ech = mlx5dv_devx_create_event_channel(ppdv->ppc.ibctx,
						  MLX5DV_DEVX_CREATE_EVENT_CHANNEL_FLAGS_OMIT_EV_DATA);
	if (!qp_ech) {
		ERR("create_event_channel failed %d", errno);
		return errno;
	}

	ret = mlx5dv_devx_subscribe_devx_event(qp_ech, ppdv->qp.obj, sizeof(event_nums), event_nums, 0);
	if (ret) {
		ERR("qp subscribe_devx_event failed: %d, errno %d\n", ret, errno);
		goto out;
	}
	DBG("QP(%d) event channel created, fd %d\n", ppdv->qp.qpn, qp_ech->fd);

	ret = pthread_create(&qp_event_tid, NULL, async_event_routine, qp_ech);
	if (ret) {
		ERR("pthread_create qp_event_routine");
		goto out;
	}

	return 0;

out:
	mlx5dv_devx_destroy_event_channel(qp_ech);
	return ret;
}

static void stop_listen_qp_event(void)
{
}
#else
static int listen_unaffliated_event(struct pp_dv_ctx *ppdv)
{
	return 0;
}
static int listen_qp_event(struct pp_dv_ctx *ppdv)
{
	return 0;
}
#endif

static int client_traffic_dv(struct pp_dv_ctx *ppdv)
{
	//int opcode = MLX5_OPCODE_RDMA_WRITE_IMM;
	int opcode = MLX5_OPCODE_SEND_IMM;
	uint16_t event_nums[1] = {0};
	int num_post = PP_MAX_WR;
	int num_comp, i, ret;
	//int num_post = 129;

	ech = mlx5dv_devx_create_event_channel(ppdv->ppc.ibctx,
					       MLX5DV_DEVX_CREATE_EVENT_CHANNEL_FLAGS_OMIT_EV_DATA);
	if (!ech) {
		ERR("create_event_channel failed %d", errno);
		return errno;
	}
	DBG("CQ event channgel created, fd %d\n", ech->fd);

	ret = mlx5dv_devx_subscribe_devx_event(ech, ppdv->cq.obj, sizeof(event_nums), event_nums, 0);
	if (ret) {
		ERR("cq subscribe_devx_event failed: %d, errno %d", ret, errno);
		return ret;
	}
	DBG("subscribe cq event succeeded\n\n");


	DBG("Pause 1sec before post send, opcode %d num_post %d length 0x%lx..\n",
	    opcode, num_post, ppdv->ppc.mrbuflen);
	sleep(1);

	for (i = 0; i < num_post; i++) {
		mem_string(ppdv->ppc.mrbuf[i], ppdv->ppc.mrbuflen);
		*ppdv->ppc.mrbuf[i] = i % ('z' - '0') + '0';
	}

	ret = pp_dv_post_send(&ppdv->ppc, &ppdv->qp, &server, num_post,
			      opcode, IBV_SEND_SIGNALED);
	if (ret) {
		ERR("pp_dv_post_send failed\n");
		return ret;
	}

	num_comp = 0;

#ifdef USE_CQ_EVENT
	DBG("Using cq event...\n");

	union {
		struct mlx5dv_devx_async_event_hdr event_resp;
		uint8_t buf[sizeof(struct mlx5dv_devx_async_event_hdr) + 128];
	} out;
	unsigned int cmd_sn = 0, arm_ci = 0, cmd = 0;
	__be32 doorbell[2];
	ssize_t sz;

	ppdv->cq.db[MLX5_CQ_ARM_DB] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
	udma_to_device_barrier();

	doorbell[0] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
	doorbell[1] = htobe32(ppdv->cq.cqn);
	mmio_write64_be(((uint8_t *)ppdv->cq.uar->base_addr + 0x20), *(__be64 *)doorbell);

	while (num_comp < num_post) {

		DBG("Waiting for cq event...\n");
		sz = mlx5dv_devx_get_event(ech, &out.event_resp, sizeof(out.buf));
		if (sz < 0) {
			ERR("devx_get_event failed %ld, errno %d\n", sz, errno);
			return sz;
		}

		ret = pp_dv_poll_cq(&ppdv->cq, 1);
		if (ret <= 0) {
			ERR("poll_cq(send) failed %d, %d/%d\n", ret, num_comp, num_post);
			return ret;
		}
		DBG("cq event received, poll_cq returns %d\n", ret);

		num_comp++;
		cmd_sn = (cmd_sn + 1) & 3;
		arm_ci = (arm_ci + 1) & 0xffffff;

		ppdv->cq.db[MLX5_CQ_ARM_DB] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
		udma_to_device_barrier();

		doorbell[0] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
		doorbell[1] = htobe32(ppdv->cq.cqn);
		mmio_write64_be(((uint8_t *)ppdv->cq.uar->base_addr + 0x20), *(__be64 *)doorbell);
	}
#else
	DBG("Using cq poll...\n");
	while (num_comp < num_post) {
		ret = pp_dv_poll_cq(&ppdv->cq, 1);
		if (ret == CQ_POLL_ERR) {
			ERR("poll_cq(send) failed %d, %d/%d\n", ret, num_comp, num_post);
			return ret;
		}
		if (ret > 0)
			num_comp++;
	}
#endif

	/* Reset the buffer so that we can check it the received data is expected */
	for (i = 0; i < num_post; i++)
		memset(ppdv->ppc.mrbuf[i], 0, ppdv->ppc.mrbuflen);

	INFO("Send done (num_post %d), now recving reply...\n", num_post);
	ret = pp_dv_post_recv(&ppdv->ppc, &ppdv->qp, num_post);
	if (ret) {
		ERR("pp_dv_post_recv failed\n");
		return ret;
	}

	num_comp = 0;
	while (num_comp < num_post) {
		ret = pp_dv_poll_cq(&ppdv->cq, 1);
		if (ret == CQ_POLL_ERR) {
			ERR("poll_cq(recv) failed %d, %d/%d\n", ret, num_comp, num_post);
			return ret;
		}
		if (ret > 0) {
			dump_msg_short(num_comp, &ppdv->ppc);
			num_comp++;
		}
	}

	INFO("Client(dv) traffic test done\n");

	mlx5dv_devx_destroy_event_channel(ech);
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

	ret = listen_unaffliated_event(&ppdv);
	if (ret)
		goto out_create_cq;

	ret = pp_create_cq_dv(&ppdv.ppc, &ppdv.cq);
	if (ret)
		goto out_create_cq;

	ret = pp_create_qp_dv(&ppdv.ppc, &ppdv.cq, &ppdv.qp);
	if (ret)
		goto out_create_qp;

	ret = listen_qp_event(&ppdv);
	if (ret)
		goto out_exchange;

	ret = pp_exchange_info(&ppdv.ppc, client_sgid_idx, ppdv.qp.qpn,
			       CLIENT_PSN, &server, SERVER_IP);
	if (ret)
		goto out_exchange;

	ret = pp_move2rts_dv(&ppdv.ppc, &ppdv.qp, client_sgid_idx,
			     CLIENT_PSN, &server);
	if (ret)
		goto out_exchange;

	ret = client_traffic_dv(&ppdv);

	stop_listen_qp_event();
out_exchange:
	pp_destroy_qp_dv(&ppdv.qp);
out_create_qp:
	pp_destroy_cq_dv(&ppdv.cq);
out_create_cq:
	pp_ctx_cleanup(&ppdv.ppc);
	return ret;
}
