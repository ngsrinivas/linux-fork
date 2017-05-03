#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

#define MTU (1500)
#define S_TO_US (1000000)
#define BW_ERROR_PERC_THRESH 5
#define EWMA_SAMPLE_WT 4
/* TODO: Hard-coding the number of bytes in the MTU is really hacky. Will fix
   this once I figure out the right way. */

#define MYRATE 750000
/* Rate above is in bytes per second. 1 MSS/millisecond is 12 Mbit/s or
   1.5 MBytes/second. */

/* Link capacity in bytes per second. */
#define LINK_CAP 750000
/* Nimbus parameters. */
#define NIMBUS_THRESH_DEL 15
#define NIMBUS_ALPHA 8
#define NIMBUS_BETA 5
#define NIMBUS_MAX_RATE (12 * LINK_CAP / 10)
#define NIMBUS_FRAC_DR 10
#define NIMBUS_CROSS_TRAFFIC_EST_VALID_THRESH 11
#define NIMBUS_EPOCH_MS 20
#define NIMBUS_RATE_STALE_TIMEOUT_MS 100

struct nimbus {
  u32 rate;           /* rate to pace packets, in bytes per second */
  u32 min_rtt_us;     /* maintain min rtt samples */
  u32 last_rtt_us;    /* maintain the last rtt sample */
  u32 rate_stamp;     /* last time when rate was updated */
};

void tcp_nimbus_pkts_acked(struct sock *sk, const struct ack_sample *sample)
{
  struct nimbus *ca = inet_csk_ca(sk);
  u32 sampleRTT = sample->rtt_us;
  ca->min_rtt_us = min(ca->min_rtt_us, sampleRTT);
  ca->last_rtt_us = sampleRTT;
}

static void tcp_nimbus_init(struct sock *sk)
{
  struct nimbus *ca = inet_csk_ca(sk);
  ca->rate = MYRATE;
  ca->min_rtt_us = 0x7fffffff;
  ca->last_rtt_us = 0x7fffffff;
  ca->rate_stamp = 0;
  sk->sk_max_pacing_rate = ca->rate;
  sk->sk_pacing_rate = 0;
  sk->sk_pacing_rate = ca->rate;
}

static u32 estimate_cross_traffic(u32 est_bandwidth,
                                  u32 rint,
                                  u32 routt,
                                  u32 last_rtt,
                                  u32 min_rtt)
{
  s64 zt;
  zt = (u64)est_bandwidth * rint;
  do_div(zt, routt);
  zt -= rint;
  pr_info("Nimbus: Estimated cross traffic: %lld bps\n", zt);
  if (last_rtt < (NIMBUS_CROSS_TRAFFIC_EST_VALID_THRESH * min_rtt / NIMBUS_FRAC_DR))
    zt = 0;
  else if (zt < 0)
    zt = 0;
  else if (zt > LINK_CAP)
    zt = LINK_CAP;
  pr_info("Nimbus: Corrected estimated cross traffic: %lld bps\n", zt);
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
  u32 last_rtt;
  u32 min_rtt;
  u16 sign;
  s32 delay_diff;
  s64 delay_term;
  s32 spare_cap;
  s32 rate_term;
  u32 two_seg_rate;

  struct nimbus *ca = inet_csk_ca(sk);
  last_rtt = ca->last_rtt_us;
  min_rtt  = ca->min_rtt_us;
  spare_cap = (s32)est_bandwidth - zt - rint;
  rate_term = NIMBUS_ALPHA * spare_cap / NIMBUS_FRAC_DR;
  delay_diff = (last_rtt -
                ((NIMBUS_THRESH_DEL * min_rtt)/NIMBUS_FRAC_DR));
  delay_term = (s64)est_bandwidth * delay_diff;
  sign = 0;
  if (delay_term < 0) {
    sign = 1;
    delay_term = -delay_term;
  }
  do_div(delay_term, (s64)min_rtt);
  delay_term = delay_term * (s64)NIMBUS_BETA;
  do_div(delay_term, (s64)NIMBUS_FRAC_DR);
  if (sign == 1)
    delay_term = -delay_term;

  /* Compute new rate as a combination of delay mismatch and rate mismatch. */
  new_rate = rint + rate_term - delay_term;
  pr_info("Nimbus: min_rtt %d last_rtt %d\n", min_rtt, last_rtt);
  pr_info("Nimbus: rint %d spare_cap %d rate_term %d delay_diff %d delay_term"
          " %lld new_rate %d\n", 
          rint, spare_cap, rate_term, delay_diff, delay_term, new_rate);
  /* Clamp the rate between two reasonable limits. */
  two_seg_rate = 2 * single_seg_bps(last_rtt);
  if (new_rate < (s32)two_seg_rate) new_rate = two_seg_rate;
  if (new_rate > (s32)NIMBUS_MAX_RATE) new_rate = NIMBUS_MAX_RATE;
  pr_info("Nimbus: clamped rate %d\n", new_rate);
  return (u32)new_rate;
}

static int rate_sample_valid(const struct rate_sample *rs)
{
  if ((rs->delivered > 0) && (rs->snd_int_us > 0) && (rs->rcv_int_us > 0))
    return 0;
  else if (rs->delivered <= 0)
    return 1;
  else if (rs->snd_int_us <= 0)
    return 2;
  else
    return 3;
}

void tcp_nimbus_check_rate_mismatch(u64 achieved_snd_rate,
                                      u64 achieved_rcv_rate,
                                      u32 set_rate,
                                      const struct rate_sample *rs,
                                      u32 perc_thresh)
{
  u32 diff_rate;
  diff_rate = set_rate - achieved_snd_rate;
  if (set_rate > achieved_snd_rate &&
      diff_rate > (perc_thresh * (set_rate / 100))) {
    pr_info("tcp_nimbus found a rate mismatch %d bps over %ld us\n",
            diff_rate, rs->interval_us);
    pr_info("(delivered %d bytes) expected: %d achieved: snd %lld rcv %lld\n",
            rs->delivered,
            set_rate,
            achieved_snd_rate,
            achieved_rcv_rate);
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
  u64 segs_in_flight; /* compute desired cwnd as rate * rtt */
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
                                ca->last_rtt_us, ca->min_rtt_us);
    new_rate = nimbus_rate_control(sk, snd_bw_bps, rcv_bw_bps, LINK_CAP, zt);
    /* Set the socket rate to nimbus proposed rate */
    ca->rate = new_rate;
    pr_info("Nimbus: Setting new rate %d\n", ca->rate);
    /* Want to ensure window can support the set rate. */
    if (likely (rs->rtt_us > 0)) {
      segs_in_flight = (u64)ca->rate * rs->rtt_us;
      do_div(segs_in_flight, MTU);
      do_div(segs_in_flight, S_TO_US);
      /* Add one more segment to segs_to_flight to prevent rate underflow due to
         temporary RTT fluctuations. */
      tp->snd_cwnd = segs_in_flight + 1;
    }
  } else if (rate_not_changed_awhile(ca)) {
    pr_info("Nimbus: Rate hasn't changed in a while! Valid rate: %d\n",
            measured_valid_rate);
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
