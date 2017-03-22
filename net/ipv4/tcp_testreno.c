/* Hello world kernel module. */

#include <linux/module.h>
#include <net/tcp.h>

/* Slow start threshold is half the congestion window (min 2) */
u32 tcp_testreno_ssthresh(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd >> 1U, 2U);
}
EXPORT_SYMBOL_GPL(tcp_testreno_ssthresh);

u32 tcp_testreno_undo_cwnd(struct sock *sk)
{
	const struct tcp_sock *tp = tcp_sk(sk);

	return max(tp->snd_cwnd, tp->snd_ssthresh << 1);
}
EXPORT_SYMBOL_GPL(tcp_testreno_undo_cwnd);

/*
 * TCP Reno congestion control
 * This is special case used for fallback as well.
 */
/* This is Jacobson's slow start and congestion avoidance.
 * SIGCOMM '88, p. 328.
 */
void tcp_testreno_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
	struct tcp_sock *tp = tcp_sk(sk);

	if (!tcp_is_cwnd_limited(sk))
		return;

	/* In "safe" area, increase. */
	if (tcp_in_slow_start(tp)) {
		acked = tcp_slow_start(tp, acked);
		if (!acked)
			return;
	}
	/* In dangerous area, increase slowly. */
	tcp_cong_avoid_ai(tp, tp->snd_cwnd, acked);
}
EXPORT_SYMBOL_GPL(tcp_testreno_cong_avoid);

struct tcp_congestion_ops tcp_testreno_congestion_ops = {
  .flags = TCP_CONG_NON_RESTRICTED,
  .name = "testreno",
  .owner = THIS_MODULE,
  .ssthresh = tcp_testreno_ssthresh,
  .cong_avoid = tcp_testreno_cong_avoid,
  .undo_cwnd = tcp_testreno_undo_cwnd,
};

static int __init tcp_testreno_register(void)
{
  printk(KERN_INFO "Init testreno\n");
  return tcp_register_congestion_control(&tcp_testreno_congestion_ops);
}

static void __exit tcp_testreno_unregister(void)
{
  printk(KERN_INFO "Exit testreno\n");
  tcp_unregister_congestion_control(&tcp_testreno_congestion_ops);
}

module_init(tcp_testreno_register);
module_exit(tcp_testreno_unregister);

MODULE_AUTHOR("Srinivas Narayana <ngsrinivas@gmail.com>");
MODULE_DESCRIPTION("Test module for experimentation");
MODULE_LICENSE("GPL");
