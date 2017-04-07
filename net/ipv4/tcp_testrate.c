#include <linux/module.h>
#include <net/tcp.h>

#define MYRATE 300000

struct testrate {
  u32 rate; /* rate to pace packets, in bytes per second */
};

static void tcp_testrate_init(struct sock *sk) {
  struct testrate *ca = inet_csk_ca(sk);
  ca->rate = MYRATE;
  sk->sk_pacing_rate = ca->rate;
}

void tcp_testrate_cong_avoid(struct sock *sk, u32 ack, u32 acked) {
  struct testrate *ca = inet_csk_ca(sk);
  sk->sk_pacing_rate = ca->rate;
}

static struct tcp_congestion_ops tcp_testrate = {
  .init = tcp_testrate_init,
  .ssthresh = tcp_reno_ssthresh,
  .cong_avoid = tcp_testrate_cong_avoid,
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
