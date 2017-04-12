#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

#define MTU (1500)
#define S_TO_US (1000000)
#define BW_ERROR_PERC_THRESH 5
#define EWMA_SAMPLE_WT 4
/* TODO: Hard-coding the number of bytes in the MTU is really hacky. Will fix
   this once I figure out the right way. */

#define MYRATE 800000
/* Rate above is in bytes per second. 1 MSS/millisecond is 12 Mbit/s or
   1.5 MBytes/second. */

struct testrate {
  u32 rate; /* rate to pace packets, in bytes per second */
  u32 mismatch_cnt; /* how frequently is delivered rate mismatched? */
  u32 ewma_rtt; /* ewma over rtt samples in us */
};

void tcp_testrate_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
  struct testrate *ca = inet_csk_ca(sk);
  u32 sampleRTT = sample->rtt_us;
  ca->ewma_rtt = (ca->ewma_rtt + (EWMA_SAMPLE_WT-1) * sampleRTT) / EWMA_SAMPLE_WT;
}

static void tcp_testrate_init(struct sock *sk) {
  struct testrate *ca = inet_csk_ca(sk);
  ca->rate = MYRATE;
  sk->sk_max_pacing_rate = ca->rate;
  sk->sk_pacing_rate = 0;
  sk->sk_pacing_rate = ca->rate;
  ca->mismatch_cnt = 0;
  ca->ewma_rtt = 0;
}

static int rate_sample_valid(const struct rate_sample *rs)
{
  return (rs->delivered > 0) && (rs->interval_us > 0);
}

void tcp_testrate_cong_control(struct sock *sk, const struct rate_sample *rs)
{
  u64 bw_bps;   /* delivered bandwidth in bytes per second */
  u32 diff_bps; /* difference in delivered and set bandwidths */
  u64 segs_in_flight; /* compute desired cwnd as rate * rtt */

  struct tcp_sock *tp = tcp_sk(sk);
  struct testrate *ca = inet_csk_ca(sk);
  /* Report rate mismatches beyond a threshold of BW_ERROR_PERC_THRESH
     percent. */
  if (rate_sample_valid(rs)) {
    bw_bps = (u64)rs->delivered * MTU * S_TO_US;
    do_div(bw_bps, rs->interval_us);
    diff_bps = ca->rate - bw_bps;
    if (ca->rate > bw_bps &&
        diff_bps > (BW_ERROR_PERC_THRESH * (ca->rate / 100))) {
      pr_info("tcp_testrate found a rate mismatch %d %d %ld %lld\n",
              diff_bps,
              rs->delivered,
              rs->interval_us,
              bw_bps);
      ca->mismatch_cnt++;
    }

    /* Want to ensure window can support the set rate. */
    if (likely (rs->rtt_us > 0)) {
      segs_in_flight = (u64)ca->rate * rs->rtt_us;
      do_div(segs_in_flight, MTU);
      do_div(segs_in_flight, S_TO_US);
      /* Add one more segment to segs_to_flight to prevent rate underflow due to
         temporary RTT fluctuations. */
      tp->snd_cwnd = segs_in_flight + 1;
    }
  }
}

/* Moved from congestion avoidance function to congestion 'control'
 * function. Currently the window doesn't change at all -- fixed to a value that
 * permits the specified rate at the given RTT. */
/* void tcp_testrate_cong_avoid(struct sock *sk, u32 ack, u32 acked) { */
/*   struct tcp_sock *tp = tcp_sk(sk); */

/*   // Run slow start and AIMD to see how queues are built. */
/*   if (tcp_in_slow_start(tp)) { */
/*     acked = tcp_slow_start(tp, acked); */
/*     if (! acked) */
/*       return; */
/*   } */
/*   tcp_cong_avoid_ai(tp, tp->snd_cwnd, acked); */
/* } */

static struct tcp_congestion_ops tcp_testrate = {
  .init = tcp_testrate_init,
  .ssthresh = tcp_reno_ssthresh,
  .pkts_acked = tcp_testrate_pkts_acked,
  .cong_control = tcp_testrate_cong_control,
  .undo_cwnd = tcp_reno_undo_cwnd,

  .owner = THIS_MODULE,
  .name  = "testrate",
};

static int __init tcp_testrate_register(void)
{
  printk(KERN_INFO "Initializing testrate\n");
  BUILD_BUG_ON(sizeof(struct testrate) > ICSK_CA_PRIV_SIZE);
  tcp_register_congestion_control(&tcp_testrate);
  return 0;
}

static void __exit tcp_testrate_unregister(void)
{
  printk(KERN_INFO "Exiting testrate\n");
  tcp_unregister_congestion_control(&tcp_testrate);
}

module_init(tcp_testrate_register);
module_exit(tcp_testrate_unregister);

MODULE_AUTHOR("Srinivas Narayana");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Test Rate");
