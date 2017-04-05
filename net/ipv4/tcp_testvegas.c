#include <linux/module.h>
#include <net/tcp.h>

static int alpha = 2;
static int beta  = 4;
static int gamma = 1;

module_param(alpha, int, 0644);
MODULE_PARM_DESC(alpha, "lower bound of packets in network");
module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "upper bound of packets in network");
module_param(gamma, int, 0644);
MODULE_PARM_DESC(gamma, "upper bound of slow-start-excess packets");

struct testvegas {
  u32 baseRTT; /* minimum RTT across *all* samples */
  u32 minRTT;  /* minimum RTT over per-RTT samples */
  u32 beg_snd_una; /* left edge during last RTT */
};

/* Maintain two separate min-estimators: minRTT over the last RTT samples;
 * baseRTT over RTT samples forever. */
void tcp_testvegas_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
  struct testvegas *testvegas = inet_csk_ca(sk);
  u32 sampleRTT = sample->rtt_us;

  testvegas->minRTT  = min(sampleRTT, testvegas->minRTT);
  testvegas->baseRTT = min(sampleRTT, testvegas->baseRTT);
}
EXPORT_SYMBOL_GPL(tcp_testvegas_pkts_acked);

void tcp_testvegas_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
  struct tcp_sock *tp = tcp_sk(sk);
  struct testvegas *testvegas = inet_csk_ca(sk);
  u32 diff;

  /* Simplified vegas control loop below; disregarding ugly details about number
     of RTT samples, slow start, etc. */

  /* detect one RTT elapsed: one window of packets acked? */
  if (after(ack, testvegas->beg_snd_una)) {
    diff = tp->snd_cwnd*(testvegas->minRTT-testvegas->baseRTT)/testvegas->minRTT;
    testvegas->beg_snd_una = tp->snd_nxt;
    if (tcp_in_slow_start(tp)) {
      if (diff > gamma) {
      /* Gotta set the ssthresh somewhere; else will permanently be stuck in
         slow start. */
        tp->snd_ssthresh = min(tp->snd_ssthresh, tp->snd_cwnd-1);
        /* A couple of options. We could either directly jump to the target cwnd
           and rate, or do additive decrease until the point of convergence. */
        tp->snd_cwnd--;
      } else {
        tcp_slow_start(tp, acked);
      }
    } else if (diff < alpha) {
      tp->snd_cwnd++;
    } else if (diff > beta) {
      tp->snd_cwnd--;
    } else {
      /* Sending just as fast as we should be. */
    }
    printk(KERN_INFO "testvegas base %d min %d\n", testvegas->baseRTT, testvegas->minRTT);
    /* wipe out minRTT for next RTT */
    testvegas->minRTT = 0x7fffffff;
  }
}

void tcp_testvegas_init(struct sock *sk)
{
  struct testvegas *testvegas = inet_csk_ca(sk);
  testvegas->baseRTT = 0x7fffffff;
  testvegas->minRTT  = 0x7fffffff;
  testvegas->beg_snd_una = 1;
}
EXPORT_SYMBOL_GPL(tcp_testvegas_init);

static struct tcp_congestion_ops tcp_testvegas = {
  .init = tcp_testvegas_init,
  .ssthresh = tcp_reno_ssthresh,
  .undo_cwnd = tcp_reno_undo_cwnd, /* double check what this is */
  .cong_avoid = tcp_testvegas_cong_avoid,
  .pkts_acked = tcp_testvegas_pkts_acked,

  .owner = THIS_MODULE,
  .name = "testvegas",
};

static int __init tcp_testvegas_register(void)
{
  printk(KERN_INFO "Initializing testvegas\n");
  BUILD_BUG_ON(sizeof(struct testvegas) > ICSK_CA_PRIV_SIZE);
  tcp_register_congestion_control(&tcp_testvegas);
  return 0;
}

static void __exit tcp_testvegas_unregister(void)
{
  printk(KERN_INFO "Exiting testvegas\n");
  tcp_unregister_congestion_control(&tcp_testvegas);
}

module_init(tcp_testvegas_register);
module_exit(tcp_testvegas_unregister);

MODULE_AUTHOR("Srinivas Narayana");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Test Vegas");
