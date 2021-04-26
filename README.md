A simple example to demonstrate how to write a rdma app over:
1. verbs APIs,
2. mlx5dv APIs, and
3. mlx5dv APIs over VFIO.

verbs API:
In server side run: ./server <ib_devname>
In client side run: ./client.verb <ib_devname>


Note:
1. There's only 1 version for server, which is in verbs APIs;
2. The SERVER_IP, client_sgid_idx is hardcoded.

--
Mark Zhang <markzhang@nvidia.com>
