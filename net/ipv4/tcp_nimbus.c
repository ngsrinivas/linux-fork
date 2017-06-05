#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

#define MTU (1500)
#define S_TO_US (1000000)
#define BW_ERROR_PERC_THRESH 5
#define EWMA_SAMPLE_WT 4
/* TODO: Hard-coding the number of bytes in the MTU is really hacky. Will fix
   this once I figure out the right way. */

#define MYRATE 12000000
/* Rate above is in bytes per second. 1 MSS/millisecond is 12 Mbit/s or
   1.5 MBytes/second. */

/* Link capacity in bytes per second. */
#define LINK_CAP 134217728
/* Nimbus parameters. */
#define NIMBUS_THRESH_DEL 15
#define NIMBUS_ALPHA 8
#define NIMBUS_BETA 5
#define NIMBUS_MAX_RATE (12 * LINK_CAP / 10)
#define NIMBUS_FRAC_DR 10
#define NIMBUS_CROSS_TRAFFIC_EST_VALID_THRESH 13
#define NIMBUS_EPOCH_MS 20
#define NIMBUS_RATE_STALE_TIMEOUT_MS 100
#define NIMBUS_MIN_SEGS_IN_FLIGHT 3
#define NIMBUS_EWMA_RECENCY 6
/* Error accounting parameters. */
#define DELAY_ERROR_THRESH_US 5000

struct nimbus {
  u32 rate;           /* rate to pace packets, in bytes per second */
  u32 min_rtt_us;     /* maintain min rtt samples */
  u32 min_rtt_stamp;  /* time when min rtt was recorded */
  u32 new_min_rtt;    /* new estimate of min rtt */
  u32 last_rtt_us;    /* maintain the last rtt sample */
  u32 ewma_rtt_us;    /* maintain an ewma of instantaneous rtt samples */
  u32 rate_stamp;     /* last time when rate was updated */
  u32 ewma_rtt_error; /* account samples with total_rtt error */
  u32 total_samples;  /* total number of rtt samples */
};

bool min_rtt_time_to_update(struct nimbus *ca)
{
  return after(tcp_time_stamp, ca->min_rtt_stamp +
               msecs_to_jiffies(ca->new_min_rtt / 100));
}

void tcp_nimbus_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
  struct nimbus *ca = inet_csk_ca(sk);
  u32 sampleRTT = sample->rtt_us;
  /* Always update latest estimate of min RTT. This estimate only holds the
   * minimum over a short period of time, namely 10 RTTs. */
  ca->new_min_rtt = min(ca->new_min_rtt, sampleRTT);
  /* If a sufficient period of time has elapsed since the last update to
   * min_rtt_us, update it. */
  if (min_rtt_time_to_update(ca)) {
    ca->min_rtt_us = ca->new_min_rtt;
    ca->min_rtt_stamp = tcp_time_stamp;
    ca->new_min_rtt = 0x7fffffff;
  }
  ca->last_rtt_us = sampleRTT;
  ca->ewma_rtt_us = ((sampleRTT * NIMBUS_EWMA_RECENCY) +
                     (ca->ewma_rtt_us *
                      (NIMBUS_FRAC_DR-NIMBUS_EWMA_RECENCY))) / NIMBUS_FRAC_DR;
}

static void tcp_nimbus_init(struct sock *sk)
{
  struct nimbus *ca = inet_csk_ca(sk);
  ca->rate = MYRATE;
  ca->min_rtt_us = 0x7fffffff;
  ca->min_rtt_stamp = 0;
  ca->new_min_rtt = 0x7fffffff;
  ca->last_rtt_us = 0x7fffffff;
  ca->ewma_rtt_us = 0x7fffffff;
  ca->rate_stamp = 0;
  sk->sk_max_pacing_rate = LINK_CAP;
  sk->sk_pacing_rate = 0;
  sk->sk_pacing_rate = ca->rate;
  ca->ewma_rtt_error = 0;
  ca->total_samples = 0;
}

static void tcp_nimbus_set_pacing_rate(struct sock *sk)
{
  struct tcp_sock *tp = tcp_sk(sk);
  struct nimbus *ca = inet_csk_ca(sk);
  u64 segs_in_flight; /* desired cwnd as rate * rtt */
  sk->sk_pacing_rate = ca->rate;
  if (likely (ca->ewma_rtt_us > 0)) {
    segs_in_flight = (u64)ca->rate * ca->ewma_rtt_us;
    do_div(segs_in_flight, MTU);
    do_div(segs_in_flight, S_TO_US);
    /* Add few more segments to segs_to_flight to prevent rate underflow due to
       temporary RTT fluctuations. */
    tp->snd_cwnd = segs_in_flight + 3;
  }
}

static u32 estimate_cross_traffic(u32 est_bandwidth,
                                  u32 rint,
                                  u32 routt,
                                  u32 rtt,
                                  u32 min_rtt)
{
  s64 zt;
  zt = (u64)est_bandwidth * rint;
  if (routt == 0) {
    pr_info("Nimbus: Unexpected condition rout===0 in "
            "estimate_cross_traffic\n");
    return 0;
  }
  do_div(zt, routt);
  zt -= rint;
  pr_info("Nimbus: Estimated cross traffic: %lld bps\n", zt);
  if (rtt < (NIMBUS_CROSS_TRAFFIC_EST_VALID_THRESH * min_rtt / NIMBUS_FRAC_DR))
    zt = 0;
  else if (zt < 0)
    zt = 0;
  else if (zt > LINK_CAP)
    zt = LINK_CAP;
  pr_info("Nimbus: Corrected estimated cross traffic: %lld Mbit/s\n", zt >> 17);
  return (u32)zt;
}

static u32 single_seg_bps(u32 rtt_us) {
  return MTU * 1000000 / rtt_us;
}

/* Sets rin(t+1) given rin(t) and several other network observations. Closely
 * follows the control law from the nimbus userspace implementation. */
static u32 nimbus_rate_control(const struct sock *sk,
                               u32 rint,
                               u32 routt,
                               u32 est_bandwidth,
                               u32 zt)
{
  s32 new_rate;
  u32 rtt; /* rtt used to compute new rate */
  u32 last_rtt;
  u32 min_rtt;
  u16 sign;
  s32 delay_diff;
  u32 expected_rtt;
  s64 delay_term;
  s32 spare_cap;
  s32 rate_term;
  u32 min_seg_rate;

  struct nimbus *ca = inet_csk_ca(sk);
  rtt = ca->ewma_rtt_us;
  min_rtt  = ca->min_rtt_us;
  last_rtt = ca->last_rtt_us;
  spare_cap = (s32)est_bandwidth - zt - rint;
  rate_term = NIMBUS_ALPHA * spare_cap / NIMBUS_FRAC_DR;
  expected_rtt = (NIMBUS_THRESH_DEL * min_rtt)/NIMBUS_FRAC_DR;
  delay_diff = (s32)rtt - (s32)expected_rtt;
  delay_term = (s64)est_bandwidth * delay_diff;
  sign = 0;
  if (delay_term < 0) {
    sign = 1;
    delay_term = -delay_term;
  }
  if (min_rtt == 0) {
    pr_info("Nimbus: unexpected min_rtt == 0 in rate control!\n");
    return rint;
  }
  do_div(delay_term, (s64)min_rtt);
  delay_term = delay_term * (s64)NIMBUS_BETA;
  do_div(delay_term, (s64)NIMBUS_FRAC_DR);
  if (sign == 1)
    delay_term = -delay_term;

  /* Error accounting */
  ca->total_samples++;
  if (((sign == 0) && (delay_diff > (s32)DELAY_ERROR_THRESH_US)) ||
      ((sign == 1) && (delay_diff < -(s32)DELAY_ERROR_THRESH_US)))
    ca->ewma_rtt_error++;

  /* Compute new rate as a combination of delay mismatch and rate mismatch. */
  new_rate = rint + rate_term - delay_term;
  pr_info("Nimbus: min_rtt %d ewma_rtt %d last_rtt %d expected_rtt %d\n",
          min_rtt, rtt, last_rtt, expected_rtt);
  pr_info("Nimbus: rint %d Mbit/s "
          "spare_cap %d Mbit/s rate_term %d Mbit/s "
          "delay_diff %d us delay_term %lld "
          "new_rate %d Mbit/s\n", 
          rint >> 17,
          spare_cap >> 17,
          rate_term >> 17,
          delay_diff,
          delay_term,
          new_rate >> 17);
  /* Clamp the rate between two reasonable limits. */
  min_seg_rate = NIMBUS_MIN_SEGS_IN_FLIGHT * single_seg_bps(rtt);
  if (new_rate < (s32)min_seg_rate) new_rate = min_seg_rate;
  if (new_rate > (s32)NIMBUS_MAX_RATE) new_rate = NIMBUS_MAX_RATE;
  pr_info("Nimbus: clamped rate %d Mbit/s\n", new_rate >> 17);
  pr_info("Nimbus: total rtt samples %d errors %d\n",
          ca->total_samples,
          ca->ewma_rtt_error);
  return (u32)new_rate;
}

static int rate_sample_valid(const struct rate_sample *rs)
{
  int ret = 0;
  if ((rs->delivered > 0) && (rs->snd_int_us > 0) && (rs->rcv_int_us > 0))
    return 0;
  if (rs->delivered <= 0)
    ret |= 1;
  if (rs->snd_int_us <= 0)
    ret |= 2;
  if (rs->rcv_int_us <= 0)
    ret |= 4;
  return ret;
}

void tcp_nimbus_check_rate_mismatch(u64 achieved_snd_rate,
                                      u64 achieved_rcv_rate,
                                      u32 set_rate,
                                      const struct rate_sample *rs,
                                      u32 perc_thresh)
{
  u32 diff_rate;
  if (set_rate > achieved_snd_rate)
    diff_rate = set_rate - achieved_snd_rate;
  else
    diff_rate = achieved_snd_rate - set_rate;
  if (diff_rate > (perc_thresh * (set_rate / 100))) {
    pr_info("Nimbus: tcp_nimbus found a rate mismatch %d Mbit/s over %ld us\n",
            diff_rate >> 17, rs->interval_us);
    pr_info("Nimbus: (delivered %d segments) expected: %d Mbit/s achieved: snd %lld rcv %lld\n",
            rs->delivered,
            set_rate >> 17,
            achieved_snd_rate >> 17,
            achieved_rcv_rate >> 17);
  }
}

bool epoch_elapsed(struct nimbus *ca)
{
  return ((ca->rate_stamp == 0) ||
          (after(tcp_time_stamp,
                 ca->rate_stamp + msecs_to_jiffies(NIMBUS_EPOCH_MS))));
}

bool rate_not_changed_awhile(struct nimbus *ca)
{
  return after(tcp_time_stamp, ca->rate_stamp +
               msecs_to_jiffies(NIMBUS_RATE_STALE_TIMEOUT_MS));
}

void tcp_nimbus_cong_control(struct sock *sk, const struct rate_sample *rs)
{
  u64 snd_bw_bps;   /* send bandwidth in bytes per second */
  u64 rcv_bw_bps;   /* recv bandwidth in bytes per second */
  u32 zt; /* Estimate of cross traffic */
  u32 new_rate; /* New rate proposed by nimbus */
  int measured_valid_rate;

  struct tcp_sock *tp = tcp_sk(sk);
  struct nimbus *ca = inet_csk_ca(sk);
  measured_valid_rate = rate_sample_valid(rs);
  if (epoch_elapsed(ca) && measured_valid_rate == 0) {
    pr_info("Nimbus: ======= epoch elapsed; recomputing rate ======\n");
    /* update per-ack state */
    ca->rate_stamp = tcp_time_stamp;
    rcv_bw_bps = snd_bw_bps = (u64)rs->delivered * MTU * S_TO_US;
    do_div(snd_bw_bps, rs->snd_int_us);
    do_div(rcv_bw_bps, rs->rcv_int_us);
    /* Check rate mismatch through a threshold difference between the set and
       achieved send rates. */
    tcp_nimbus_check_rate_mismatch(snd_bw_bps,
                                   rcv_bw_bps,
                                   ca->rate,
                                   rs,
                                   BW_ERROR_PERC_THRESH);
    /* Perform nimbus rate control */
    zt = estimate_cross_traffic(LINK_CAP, snd_bw_bps, rcv_bw_bps,
                                ca->ewma_rtt_us, ca->min_rtt_us);
    new_rate = nimbus_rate_control(sk, snd_bw_bps, rcv_bw_bps, LINK_CAP, zt);
    /* Set the socket rate to nimbus proposed rate */
    ca->rate = new_rate;
    pr_info("Nimbus: Setting new rate %d Mbit/s\n", ca->rate >> 17);
    tcp_nimbus_set_pacing_rate(sk);
  } else if (rate_not_changed_awhile(ca)) {
    pr_info("Nimbus: Rate hasn't changed in a while! Valid rate: %d %d\n",
            measured_valid_rate, tp->snd_cwnd);
  }
}

static struct tcp_congestion_ops tcp_nimbus = {
  .init = tcp_nimbus_init,
  .ssthresh = tcp_reno_ssthresh,
  .pkts_acked = tcp_nimbus_pkts_acked,
  .cong_control = tcp_nimbus_cong_control,
  .undo_cwnd = tcp_reno_undo_cwnd,

  .owner = THIS_MODULE,
  .name  = "nimbus",
};

static int __init tcp_nimbus_register(void)
{
  printk(KERN_INFO "Initializing nimbus\n");
  BUILD_BUG_ON(sizeof(struct nimbus) > ICSK_CA_PRIV_SIZE);
  tcp_register_congestion_control(&tcp_nimbus);
  return 0;
}

static void __exit tcp_nimbus_unregister(void)
{
  printk(KERN_INFO "Exiting nimbus\n");
  tcp_unregister_congestion_control(&tcp_nimbus);
}

module_init(tcp_nimbus_register);
module_exit(tcp_nimbus_unregister);

MODULE_AUTHOR("Srinivas Narayana");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Nimbus");
