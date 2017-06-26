#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

#define MTU (1500)
#define S_TO_US (1000000)
#define BW_ERROR_PERC_THRESH 15
/* TODO: Hard-coding the number of bytes in the MTU is really hacky. Will fix
   this once I figure out the right way. */

#define MYRATE 875000000
/* Rate above is in bytes per second. 1 MSS/millisecond is 12 Mbit/s or
   1.5 MBytes/second. */

struct testrate {
  u32 rate;           /* rate to pace packets, in bytes per second */

  u32 min_rtt_us;

  u32 rtt_epoch;
  u32 recent_rtt;
  u32 rtt_count;
};

static void tcp_testrate_init(struct sock *sk)
{
  struct testrate *ca = inet_csk_ca(sk);
  ca->rate = MYRATE;
  sk->sk_max_pacing_rate = ca->rate;
  sk->sk_pacing_rate = 0;
  sk->sk_pacing_rate = ca->rate;

  // RTT estimator
  ca->rtt_epoch = tcp_time_stamp;
  ca->recent_rtt = 0x7fffffff;
  ca->min_rtt_us = 0x7fffffff;
  ca->rtt_count = 0;

  pr_info("intialized testrate\n");
}

/* Use a vegas-like formula for computing the RTT:
 * Maintain two values, global and recent min RTTs
 * 
 * o Global min RTT: get from TCP with tp->rtt_min
 * o Recent min RTT: use as the current RTT to avoid weird TCP RTT reports.
 *   Value reset on nimbus rate update epoch.
 */
void tcp_testrate_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
  struct testrate *ca = inet_csk_ca(sk);
  struct tcp_sock *tp = tcp_sk(sk);
  s32 sampleRTT = sample->rtt_us;

  /* Check the validity of the RTT samples */
  if (sampleRTT <= 0) {
    //pr_info("Nimbus: invalid sample rtt %d in pkts_acked\n", sampleRTT);
    return;
  }

  /* Always update latest estimate of min RTT. This estimate only holds the
   * minimum over a short period of time. A rate update will reset this value. */
  if (after(tcp_time_stamp, ca->rtt_epoch + msecs_to_jiffies(100))) {
    pr_info("testrate: min_rtt: %d recent_rtt: %d count: %d\n", ca->min_rtt_us, ca->recent_rtt, ca->rtt_count);
    ca->rtt_count = 0;
    ca->recent_rtt = 0x7fffffff;
    ca->rtt_epoch = tcp_time_stamp;

    // get min rtt from TCP
    ca->min_rtt_us = minmax_get(&(tp->rtt_min));
  } else {
    ca->recent_rtt = min(ca->recent_rtt, (u32)sampleRTT);
    ca->rtt_count++;
  }
}

static int rate_sample_valid(const struct rate_sample *rs)
{
  return (rs->delivered > 0) && (rs->snd_int_us > 0) && (rs->rcv_int_us > 0);
}

void tcp_testrate_check_rate_mismatch(u64 achieved_snd_rate,
                                      u64 achieved_rcv_rate,
                                      u32 set_rate,
                                      const struct rate_sample *rs,
                                      u32 perc_thresh)
{
  u32 diff_rate;
  diff_rate = set_rate - achieved_snd_rate;
  if (set_rate > achieved_snd_rate &&
      diff_rate > (perc_thresh * (set_rate / 100))) {
    pr_info("TestRate: found a rate mismatch %d bps over %ld us\n",
            diff_rate, rs->interval_us);
    pr_info("TestRate: delivered %d bytes. expected rate: %d achieved: snd %lld"
            " rcv %lld\n",
            rs->delivered,
            set_rate,
            achieved_snd_rate,
            achieved_rcv_rate);
  }
}

void tcp_testrate_cong_control(struct sock *sk, const struct rate_sample *rs)
{
  u64 snd_bw_bps;   /* send bandwidth in bytes per second */
  u64 rcv_bw_bps;   /* recv bandwidth in bytes per second */
  u64 segs_in_flight; /* compute desired cwnd as rate * rtt */

  struct tcp_sock *tp = tcp_sk(sk);
  struct testrate *ca = inet_csk_ca(sk);
  if (rate_sample_valid(rs)) {
    rcv_bw_bps = snd_bw_bps = (u64)rs->delivered * MTU * S_TO_US;
    do_div(snd_bw_bps, rs->snd_int_us);
    do_div(rcv_bw_bps, rs->rcv_int_us);
    /* Check rate mismatch through a threshold difference between the set and
       achieved send rates. */
    tcp_testrate_check_rate_mismatch(snd_bw_bps,
                                     rcv_bw_bps,
                                     ca->rate,
                                     rs,
                                     BW_ERROR_PERC_THRESH);
    /* Want to ensure window can support the set rate. */
    if (likely (rs->rtt_us > 0)) {
      segs_in_flight = (u64)ca->rate * rs->rtt_us;
      do_div(segs_in_flight, MTU);
      do_div(segs_in_flight, S_TO_US);
      /* Add one more segment to segs_to_flight to prevent rate underflow due to
         temporary RTT fluctuations. */
      tp->snd_cwnd = segs_in_flight + 3;
    }
  }
}

static struct tcp_congestion_ops tcp_testrate = {
  .init = tcp_testrate_init,
  .ssthresh = tcp_reno_ssthresh,
  .cong_control = tcp_testrate_cong_control,
  .undo_cwnd = tcp_reno_undo_cwnd,
  .pkts_acked = tcp_testrate_pkts_acked,

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
