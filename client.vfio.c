#include <linux/vfio.h>
#include <signal.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/ioctl.h>
#include <semaphore.h>

#include "ccan/ilog.h"

#include "util/mmio.h"
#include "util/udma_barrier.h"
#include "util/util.h"

#include "pp_common.h"
#include "pp_dv.h"
#include "pp_vfio.h"

#define SERVER_IP "10.237.1.205"

char *vfio_pci_name = "0000:3b:00.1"; /* env(VFIO_PCI_NAME) */

static struct pp_dv_ctx ppvfio;
static struct pp_exchange_info server = {};

static struct mlx5_eq {
        __be32 *doorbell;
	uint32_t cons_index;
	uint8_t eqn;
	int nent;
	void *vaddr;
	struct mlx5dv_devx_uar *uar;
	struct mlx5dv_devx_msi_vector *msi;
	struct mlx5dv_devx_eq *dv_eq;
} async_eq;

/* sempaphore is used here to simply demostrate how the traffic thread
 * and event thread are synchronized. It doesn't work if one eq are
 * share by multiple CQs, as one semaphore signal doesn't tell which cq
 * has the event. In this case need to use another way, e.g., pipe(2).
 */
static sem_t sem;

static int client_traffic_dv(struct pp_dv_ctx *ppdv)
{
	int num_post = PP_MAX_WR, num_comp, i, ret;
	//int opcode = MLX5_OPCODE_RDMA_WRITE_IMM;
	int opcode = MLX5_OPCODE_SEND_IMM;
	unsigned int cmd_sn = 0, arm_ci = 0, cmd = 0;
	__be32 doorbell[2];

	DBG("Pause 1sec before post send, opcode %d\n", opcode);
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

#if 1
	ppdv->cq.db[MLX5_CQ_ARM_DB] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
	udma_to_device_barrier();
	doorbell[0] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
	doorbell[1] =  htobe32(ppdv->cq.cqn);
	//mmio_write64_be(((uint8_t *)ppdv->cq.uar->base_addr + 0x20), *(__be64 *)doorbell);
	mmio_write64_be(((uint8_t *)async_eq.uar->base_addr + 0x20), *(__be64 *)doorbell);
	printf("=DEBUG:%s:%d: ppdv->cq.uar->base_addr %p, async_eq.uar->base_addr %p\n", __func__, __LINE__, ppdv->cq.uar->base_addr, async_eq.uar->base_addr);
#endif

	while (num_comp < num_post) {
		sem_wait(&sem);

		/* FIXME: Need to wait for the event from the do_process_async_event() thread,
		 *        otherwise not sure if there's any contention
		 */
		ret = pp_dv_poll_cq(&ppdv->cq, 1);
		if (ret == CQ_POLL_ERR) {
			ERR("poll_cq(send) failed %d, %d/%d\n", ret, num_comp, num_post);
			return ret;
		}
		if (ret > 0)
			num_comp++;

		cmd_sn = (cmd_sn + 1) & 3;
		arm_ci = (arm_ci + 1) & 0xffffff;

		ppdv->cq.db[MLX5_CQ_ARM_DB] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
		udma_to_device_barrier();

		doorbell[0] = htobe32((cmd_sn << 28) | (cmd << 24) | arm_ci);
		doorbell[1] = htobe32(ppdv->cq.cqn);
		mmio_write64_be(((uint8_t *)async_eq.uar->base_addr + 0x20), *(__be64 *)doorbell);
	}

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
	return 0;
}

struct mlx5_eqe_comp {
	__be32  reserved[6];
	__be32  cqn;
};
struct mlx5_eqe_port_state {
	uint8_t reserved0[8];
	uint8_t port_num;
};
union ev_data {
	__be32 raw[7];
	struct mlx5_eqe_comp comp;
	struct mlx5_eqe_port_state port;
	//struct mlx5_eqe_cmd cmd;
	//struct mlx5_eqe_page_req req_pages;
};

struct mlx5_eqe {
	uint8_t rsvd0;
	uint8_t type;
	uint8_t rsvd1;
	uint8_t sub_type;
	__be32 rsvd2[7];
	union ev_data data;
	__be16 rsvd3;
	uint8_t signature;
	uint8_t owner;
};

#define MLX5_EQE_SIZE (sizeof(struct mlx5_eqe))
#define MLX5_NUM_SPARE_EQE (0x80)
#define EQE_ENTRY_NUM  (0x80 + MLX5_NUM_SPARE_EQE)

enum {
	MLX5_EQE_OWNER_INIT_VAL = 0x1,
};

static struct mlx5_eqe *get_eqe(struct mlx5_eq *eq, uint32_t entry)
{
	return eq->vaddr + entry * MLX5_EQE_SIZE;
}

static void init_eq_buf(struct mlx5_eq *eq)
{
	struct mlx5_eqe *eqe;
	int i;


	for (i = 0; i < eq->nent; i++) {
		eqe = get_eqe(eq, i);
		eqe->owner = MLX5_EQE_OWNER_INIT_VAL;
	}
}

static void eq_update_ci(struct mlx5_eq *eq, uint32_t cc, int arm)
{
        __be32 *addr = eq->doorbell + (arm ? 0 : 2);
        uint32_t val;

        eq->cons_index += cc;
        val = (eq->cons_index & 0xffffff) | (eq->eqn << 24);

        mmio_write32_be(addr, htobe32(val));
        udma_to_device_barrier();
}

static struct mlx5_eqe *mlx5_eq_get_eqe(struct mlx5_eq *eq, uint32_t cc)
{
	uint32_t ci = eq->cons_index + cc;
	struct mlx5_eqe *eqe;

	eqe = get_eqe(eq, ci & (eq->nent - 1));
	eqe = ((eqe->owner & 1) ^ !!(ci & eq->nent)) ? NULL : eqe;

	if (eqe)
		udma_from_device_barrier();

	return eqe;
}

/* The HCA will think the queue has overflowed if we don't tell it we've been
 * processing events.
 * We create EQs with MLX5_NUM_SPARE_EQE extra entries,
 * so we must update our consumer index at least that often.
 */
static inline uint32_t mlx5_eq_update_cc(struct mlx5_eq *eq, uint32_t cc)
{
	if (unlikely(cc >= MLX5_NUM_SPARE_EQE)) {
		eq_update_ci(eq, cc, 0);
		cc = 0;
	}
	return cc;
}

static int create_eq(struct pp_context *ppc)
{
	uint32_t in[DEVX_ST_SZ_DW(create_eq_in)] = {}, out[DEVX_ST_SZ_DW(create_eq_out)] = {};
	struct mlx5dv_devx_eq *eq;
	uint64_t mask[4] = {};
	void *eqc;
	int i;

	async_eq.uar = mlx5dv_devx_alloc_uar(ppc->ibctx, MLX5_IB_UAPI_UAR_ALLOC_TYPE_NC);
	if (!async_eq.uar) {
		ERR("mlx5dv_devx_alloc_uar errno %d\n", errno);
		return errno;
	}

	mask[0] = 1ull << MLX5_EVENT_TYPE_PORT_STATE_CHANGE;
	for (i = 0; i < 4; i++)
		DEVX_ARRAY_SET64(create_eq_in, in, event_bitmask, i, mask[i]);

	DEVX_SET(create_eq_in, in, opcode, MLX5_CMD_OP_CREATE_EQ);

	async_eq.nent = EQE_ENTRY_NUM;
	eqc = DEVX_ADDR_OF(create_eq_in, in, eq_context_entry);
        DEVX_SET(eqc, eqc, log_eq_size, ilog32(async_eq.nent - 1));
	DEVX_SET(eqc, eqc, uar_page, async_eq.uar->page_id);
	DEVX_SET(eqc, eqc, intr, async_eq.msi->vector);

	eq = mlx5dv_devx_create_eq(ppc->ibctx, in, sizeof(in), out, sizeof(out));
	if (!eq) {
		ERR("mlx5dv_devx_create_eq errno %d\n", errno);
		goto fail_obj_create;
	}

	async_eq.vaddr = eq->vaddr;
	async_eq.dv_eq = eq;
	async_eq.eqn = DEVX_GET(create_eq_out, out, eq_number);
	async_eq.cons_index = 0;
	async_eq.doorbell = async_eq.uar->base_addr + MLX5_EQ_DOORBEL_OFFSET;

	init_eq_buf(&async_eq);

	DBG("async_eq: doorbell %p cons_index %d vecidx %d eqn %d nent %d vaddr %p uar->page_id %d\n", async_eq.doorbell, async_eq.cons_index, async_eq.msi->vector, async_eq.eqn, async_eq.nent, async_eq.dv_eq->vaddr, async_eq.uar->page_id);

	return 0;

fail_obj_create:
	mlx5dv_devx_free_uar(async_eq.uar);
	return errno;
}

static int destroy_eq(struct mlx5_eq *eq)
{
	mlx5dv_devx_destroy_eq(eq->dv_eq);
	mlx5dv_devx_free_uar(eq->uar);
	return 0;
}

static void process_event_comp(struct mlx5_eqe *eqe)
{
	printf("=DEBUG:%s:%d: Received cq comp event (sub_type %d) for cq %d........\n", __func__, __LINE__,
	       eqe->sub_type, be32toh(eqe->data.comp.cqn));
	sem_post(&sem);
}

static void process_event_port_state_change(struct mlx5_eqe *eqe)
{
	printf("=DEBUG:%s:%d: Received port state change event(sub_type %d) for port %d.........\n", __func__, __LINE__, eqe->sub_type, eqe->data.port.port_num >> 4);
}

static int do_process_async_event(void)
{
	struct mlx5_eqe *eqe;
	int ret = 0;
	int cc = 0;

	while ((eqe = mlx5_eq_get_eqe(&async_eq, cc))) {
		switch (eqe->type) {
		case MLX5_EVENT_TYPE_COMP:
			process_event_comp(eqe);
			break;
		case MLX5_EVENT_TYPE_PORT_STATE_CHANGE:
			process_event_port_state_change(eqe);
			break;
		default:
			printf("=DEBUG:%s:%d: eqe->type %d......\n", __func__, __LINE__, eqe->type);
			break;
		}

		cc = mlx5_eq_update_cc(&async_eq, ++cc);
	}

	eq_update_ci(&async_eq, cc, 1);
	return ret;
}

int process_async_event(int fd)
{
	uint64_t u;
	ssize_t s;

	/* read to re-arm the FD and process all existing events */
	s = read(fd, &u, sizeof(uint64_t));
	if (s < 0 && errno != EAGAIN) {
		ERR("read failed, errno=%d\n", errno);
		return errno;
	}

	return do_process_async_event();
}

#define MAX_EVENTS 1
void *vfio_poll_eq_event_routine(void *arg)
{
	struct pp_context *pp = (struct pp_context *)arg;
	struct epoll_event ev, events[MAX_EVENTS];
	int vfio_driver_fd, epoll_fd, nfds, i, ret;

	vfio_driver_fd = mlx5dv_vfio_get_events_fd(pp->ibctx);
	if (vfio_driver_fd < 0) {
		ERR("mlx5dv_vfio_get_events_fd failed %d\n", vfio_driver_fd);
		return NULL;
	}

	INFO("running poll thread, internal efd = %d...\n", vfio_driver_fd);

	epoll_fd = epoll_create1(0);
	if (epoll_fd < 0) {
		ERR("epoll_create1 failed\n");
		return NULL;
	}
	ev.events = EPOLLIN;
	ev.data.fd = vfio_driver_fd;
	ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, vfio_driver_fd, &ev);
	if (ret < 0) {
		ERR("epoll_ctl failed\n");
		return NULL;
	}

	ev.events = EPOLLIN;
	ev.data.fd = async_eq.msi->fd;
	ret = epoll_ctl(epoll_fd, EPOLL_CTL_ADD, async_eq.msi->fd, &ev);
	if (ret < 0) {
		ERR("epoll_ctl failed\n");
		return NULL;
	}

	while (1) {
		nfds = epoll_wait(epoll_fd, events, MAX_EVENTS, -1);
		if (nfds < 0) {
			ERR("epoll_wait failed\n");
			return NULL;
		}

		for (i = 0; i < nfds; i++) {
			if ((events[i].events & EPOLLERR) ||
			    (events[i].events & EPOLLHUP) ||
			    (!(events[i].events & EPOLLIN))) {
				ERR("events 0x%x\n", events[i].events);
				return NULL;
			}
			if (events[i].data.fd == vfio_driver_fd)
				mlx5dv_vfio_process_events(pp->ibctx);
			else if (events[i].data.fd == async_eq.msi->fd)
				process_async_event(async_eq.msi->fd);
		}
	}

	ERR("returned unexpectedly");
	return NULL;
}

static int setup_async_eq(struct pp_context *ppc)
{
	int ret;

	async_eq.msi = mlx5dv_devx_alloc_msi_vector(ppc->ibctx);
	if (!async_eq.msi)
		return -1;

	DBG("async event vector %d fd %d\n", async_eq.msi->vector, async_eq.msi->fd);

	ret = create_eq(ppc);
	if (ret) {
		ERR("eventfd failed errno %d\n", errno);
		goto fail_create_eq;
	}

	eq_update_ci(&async_eq, 0, 1);

	sem_init(&sem, 0, 0);
	return 0;

fail_create_eq:
	mlx5dv_devx_free_msi_vector(async_eq.msi);
	return ret;
}

static pthread_t event_tid;
int setup_event_routine(struct pp_context *pp)
{
	int ret;

	ret = pthread_create(&event_tid, NULL, vfio_poll_eq_event_routine, pp);
	if (ret) {
		perror("pthread_create");
		return ret;
	}

	usleep(100);
	INFO("pthread created\n");
	return 0;
}

static void sig_handler(int signum)
{
	ERR("Signal %d is captured!\n", signum);
}

static int setup_sighandler(void)
{
	struct sigaction new_action;
	int ret;

	new_action.sa_handler = sig_handler;
	sigemptyset(&new_action.sa_mask);
	new_action.sa_flags = 0;
	ret = sigaction(SIGABRT, &new_action, NULL);
	if (ret)
		ERR("sigaction(SIGABRT) failed %d\n", ret);

	return ret;
}

static int vfio_init(struct pp_context *ppc)
{
	int ret;

	/* Dump some hca_cap to check if vfio works */
	ret = pp_query_hca_cap(ppc);
	if (ret)
		return ret;

	ret = setup_sighandler();
	if (ret)
		return ret;

	ret = setup_async_eq(ppc);
	if (ret)
		return ret;

	ret = setup_event_routine(ppc);
	if (ret)
		return ret;

	ret = pp_config_port(ppc->ibctx, MLX5_PORT_UP);
	if (ret)
		return ret;

	do {
		ret = pp_query_mad_ifc_port(ppc->ibctx, 1, &ppc->port_attr);
		if (ret)
			return ret;

		if ((ppc->port_attr.state >= IBV_PORT_ACTIVE) &&
		    (ppc->port_attr.lid != 65535))
			break;

		sleep(1);
	} while (1);
	INFO("Pause 3 seconds to make sure server start to listen...\n\n");
	sleep(3);
	return 0;
}

static void vfio_cleanup(struct pp_context *ppc)
{
	void *res;

	pthread_cancel(event_tid);
	pthread_join(event_tid, &res);

	destroy_eq(&async_eq);
	mlx5dv_devx_free_msi_vector(async_eq.msi);
}

static void parse_arg(int argc, char *argv[])
{
	char *v;

	v = getenv("VFIO_PCI_NAME");
	if (v)
		vfio_pci_name = v;
}

int main(int argc, char *argv[])
{
	int ret;

	parse_arg(argc, argv);
	INFO("VFIO pci device: %s\n", vfio_pci_name);

	ret = pp_ctx_init(&ppvfio.ppc, NULL, true, vfio_pci_name);
	if (ret)
		return ret;

	ret = vfio_init(&ppvfio.ppc);
	if (ret)
		goto out_vfio_init;

	ret = pp_create_cq_dv(&ppvfio.ppc, &ppvfio.cq, async_eq.eqn);
	if (ret)
		goto out_create_cq;

	ret = pp_create_qp_dv(&ppvfio.ppc, &ppvfio.cq, &ppvfio.qp);
	if (ret)
		goto out_create_qp;

	ret = pp_exchange_info(&ppvfio.ppc, 0, ppvfio.qp.qpn,
			       CLIENT_PSN, &server, SERVER_IP);
	if (ret)
		goto out_exchange;

	ret = pp_move2rts_dv(&ppvfio.ppc, &ppvfio.qp, 0,
			     CLIENT_PSN, &server);
	if (ret)
		goto out_exchange;

	ret = client_traffic_dv(&ppvfio);


out_exchange:
	pp_destroy_qp_dv(&ppvfio.qp);
out_create_qp:
	pp_destroy_cq_dv(&ppvfio.cq);
out_create_cq:
	vfio_cleanup(&ppvfio.ppc);
out_vfio_init:
	pp_ctx_cleanup(&ppvfio.ppc);
	return ret;
}
