#include "hrd.h"
#include "main.h"

void* run_client(void* arg) {
  struct thread_params params = *(struct thread_params*)arg;

  /*
   * The local HID of a control block should be <= 64 to keep the SHM key low.
   * But the number of clients over all machines can be larger.
   */
  int clt_gid = params.id; /* Global ID of this client thread */
  int clt_local_hid = clt_gid % params.num_threads;

  int srv_gid = clt_gid % NUM_SERVER_THREADS;
  int ib_port_index = params.dual_port == 0 ? 0 : srv_gid % 2;

  struct hrd_ctrl_blk* cb = hrd_ctrl_blk_init(
      clt_local_hid, ib_port_index, -1, /* port_index, numa_node_id */
      0, 0,                             /* num conn qps, use uc */
      NULL, 0, -1,                      /* prealloc conn buf, buf size, key */
      1, BUF_SIZE, -1);                 /* num_dgram_qps, dgram_buf_size, key */

  /* Buffer to receive responses into */
  memset((void*)cb->dgram_buf, 0, BUF_SIZE);

  /* Buffer to send requests from */
  uint8_t* req_buf = malloc(params.size);
  assert(req_buf != 0);
  memset(req_buf, clt_gid, params.size);

  char srv_name[HRD_QP_NAME_SIZE];
  sprintf(srv_name, "server-%d", srv_gid);
  char clt_name[HRD_QP_NAME_SIZE];
  sprintf(clt_name, "client-%d", clt_gid);

  hrd_publish_dgram_qp(cb, 0, clt_name);
  printf("main: Client %s published. Waiting for server %s\n", clt_name,
         srv_name);

  struct hrd_qp_attr* srv_qp = NULL;
  while (srv_qp == NULL) {
    srv_qp = hrd_get_published_qp(srv_name);
    if (srv_qp == NULL) {
      usleep(200000);
    }
  }

  printf("main: Client %s found server! Now posting SENDs.\n", clt_name);

  struct ibv_ah_attr ah_attr = {
      .is_global = 0,
      .dlid = srv_qp->lid,
      .sl = 0,
      .src_path_bits = 0,
      .port_num = cb->dev_port_id,
  };

  struct ibv_ah* ah = ibv_create_ah(cb->pd, &ah_attr);
  assert(ah != NULL);

  struct ibv_send_wr wr[MAX_POSTLIST], *bad_send_wr;
  struct ibv_wc wc[MAX_POSTLIST];
  struct ibv_sge sgl[MAX_POSTLIST];
  long long rolling_iter = 0; /* For throughput measurement */
  int w_i = 0;                /* Window index */
  int ret;

  struct timespec start, end;
  clock_gettime(CLOCK_REALTIME, &start);

  while (1) {
    if (rolling_iter >= K_512) {
      clock_gettime(CLOCK_REALTIME, &end);
      double seconds = (end.tv_sec - start.tv_sec) +
                       (double)(end.tv_nsec - start.tv_nsec) / 1000000000;
      printf("main: Client %d: %.2f Mops\n", clt_gid, K_512 / seconds);
      rolling_iter = 0;

      clock_gettime(CLOCK_REALTIME, &start);
    }

    for (w_i = 0; w_i < params.postlist; w_i++) {
      hrd_post_dgram_recv(cb->dgram_qp[0], (void*)cb->dgram_buf, BUF_SIZE,
                          cb->dgram_buf_mr->lkey);

      wr[w_i].wr.ud.ah = ah;
      wr[w_i].wr.ud.remote_qpn = srv_qp->qpn;
      wr[w_i].wr.ud.remote_qkey = HRD_DEFAULT_QKEY;

      wr[w_i].opcode = IBV_WR_SEND;
      wr[w_i].num_sge = 1;
      wr[w_i].next = (w_i == params.postlist - 1) ? NULL : &wr[w_i + 1];
      wr[w_i].sg_list = &sgl[w_i];

      wr[w_i].send_flags = (w_i == 0) ? IBV_SEND_SIGNALED : 0;
      wr[w_i].send_flags |= IBV_SEND_INLINE;

      sgl[w_i].addr = (uint64_t)(uintptr_t)req_buf;
      sgl[w_i].length = params.size;

      rolling_iter++;
    }

    ret = ibv_post_send(cb->dgram_qp[0], &wr[0], &bad_send_wr);
    CPE(ret, "ibv_post_send error", ret);

    hrd_poll_cq(cb->dgram_send_cq[0], 1, wc); /* Poll SEND (w_i = 0) */

    /* Poll for all RECVs */
    hrd_poll_cq(cb->dgram_recv_cq[0], params.postlist, wc);
  }

  return NULL;
}
