#include <linux/module.h>
#include <net/tcp.h>
#include <linux/math64.h>

/* TODO */
/* define parameters bictcp_hz and cube_rtt_scale and cube_factor and
 * beta and bictcp_beta_scale. */
#define BICTCP_BETA_SCALE    1024	/* Scale factor beta calculation
					 * max_cwnd = snd_cwnd * beta
					 */
#define	BICTCP_HZ		10	/* BIC HZ 2^10 = 1024 */
static int beta __read_mostly = 717;	/* = 717/1024 (BICTCP_BETA_SCALE) */
static int bic_scale __read_mostly = 41;
static u32 cube_rtt_scale __read_mostly;
static u64 cube_factor __read_mostly;

module_param(beta, int, 0644);
MODULE_PARM_DESC(beta, "beta for multiplicative increase");
module_param(bic_scale, int, 0444);
MODULE_PARM_DESC(bic_scale, "scale (scaled by 1024) value for bic function"
                 " (bic_scale/1024)"); 

struct testcubic {
  u32 bic_K;         /* origin point on time axis */
  u32 origin_cwnd;   /* origin point on cwnd axis */
  u32 epoch_start;   /* time-axis adjustment for cubic function */
  u32 last_time;     /* last time when a cubic update happened */
  u32 last_cwnd;     /* last cwnd value when a cubic update happened */
  u32 last_max_cwnd; /* the max window from the last cubic 'turnaround'.
                        Useful to compute the ssthresh and origin_cwnd next
                        time. */
};

static void tcp_testcubic_cong_avoid(struct sock *sk, u32 ack, u32 acked)
{
  struct tcp_sock *tp = tcp_sk(sk);
  struct testcubic *ca = inet_csk_ca(sk);

  /* Standard boilerplate for cong_avoid functions */
  if (!tcp_is_cwnd_limited(sk))
    return;

  if (tcp_in_slow_start(tp)) {
    acked = tcp_slow_start(tp, acked);
    if (! acked)
      return;
  }

  /* Begin actual cubic update logic. */
  u32 t, target, bic_target, delta, cwnd;
  s32 offs;

  /* Only update windows if something changed in the last jiffy. */
  cwnd = tp->snd_cwnd;
  if (ca->last_cwnd == cwnd &&
	    (s32)(tcp_time_stamp - ca->last_time) <= HZ / 32)
		return;

  /* Save a few things before starting cwnd updates. */
  ca->last_cwnd = cwnd;
  ca->last_time = tcp_time_stamp;

  /* Shamelessly copied cubic function arithmetic from cubic tcp
     implementation. */
	if (ca->epoch_start == 0) {
		ca->epoch_start = tcp_time_stamp;	/* record beginning */

		if (ca->last_max_cwnd <= cwnd) {
			ca->bic_K = 0;
			ca->origin_cwnd = cwnd;
		} else {
			/* Compute new K based on
			 * (wmax-cwnd) * (srtt>>3 / HZ) / c * 2^(3*bictcp_HZ)
			 */
			ca->bic_K = cubic_root(cube_factor
					       * (ca->last_max_cwnd - cwnd));
			ca->origin_cwnd = ca->last_max_cwnd;
		}
	}

  /* Here come the actual cubic updates with gory operations to avoid
   * overflows. */
  t = (s32)(tcp_time_stamp - ca->epoch_start);
	t += msecs_to_jiffies(ca->delay_min >> 3);
	/* change the unit from HZ to bictcp_HZ */
	t <<= BICTCP_HZ;
	do_div(t, HZ);

	if (t < ca->bic_K)		/* t - K */
		offs = ca->bic_K - t;
	else
		offs = t - ca->bic_K;

	/* c/rtt * (t-K)^3 */
	delta = (cube_rtt_scale * offs * offs * offs) >> (10+3*BICTCP_HZ);
	if (t < ca->bic_K)                            /* below origin*/
		bic_target = ca->origin_cwnd - delta;
	else                                          /* above origin*/
		bic_target = ca->origin_cwnd + delta;

	/* cubic function - calc bictcp_cnt*/
	if (bic_target > cwnd) {
		ca->cnt = cwnd / (bic_target - cwnd);
	} else {
		ca->cnt = 100 * cwnd;              /* very small increment*/
	}
}

static u32 tcp_testcubic_ssthresh(struct sock *sk)
{
  /* I wonder if there's an implicit assumption that this function is called
   * whenever there is a loss... */
  const struct tcp_sock *tp = tcp_sk(sk);
  struct testcubic *ca = inet_csk_ca(sk);

  /* Set the last max cwnd to current cwnd. */
  ca->last_max_cwnd = tp->snd_cwnd;
  return max((tp->snd_cwnd * beta) / BICTCP_BETA_SCALE, 2U);
}

static u32 tcp_testcubic_undo_cwnd(struct sock *sk)
{
  /* Still have to understand crystal-clearly where and how undo_cwnd is
   * used. */
  struct tcp_sock *tp = tcp_sk(sk);
  struct testcubic *ca = inet_csk_ca(sk);

  return max(tp->snd_cwnd, ca->last_max_cwnd);
}

static void tcp_testcubic_set_state(struct sock *sk, u8 new_state)
{
  if (new_state == TCP_CA_LOSS) {
    tcp_testcubic_init();
  }
}

/* This function may not even be needed for the preliminary implementation... */
/* static void tcp_testcubic_cwnd_event(struct sock *sk, enum tcp_ca_event event) */
/* { */
/*   struct tcp_sock *tp = tcp_sk(sk); */
/*   struct testcubic *ca = inet_csk_ca(sk); */

/*   if (event == CA_EVENT_TX_START) { */
/*     /\* Restarting transmission after an idle period... *\/ */
/*     u32 now = tcp_time_stamp; */
/*     s32 delta; */

/*     delta = now - tp->lsndtime; */
/*     if (ca->epoch_start && delta >= 0) { */
/*       ca->epoch_start += delta; /\* adjust the cubic curve as though it's beginning */
/*                                    from some time in the recent past, instead of a */
/*                                    long time ago...*\/ */
/*       if (after(ca->epoch_start, now)) */
/*         ca->epoch_start = now; */
/*     } */
/*     return; */
/*   } */
/* } */

static void tcp_testcubic_init(struct sock *sk)
{
  struct testcubic *testcubic = inet_csk_ca(sk);
  testcubic->bic_K = 0;
  testcubic->origin_cwnd = 0;
  testcubic->epoch_start = 0;
  testcubic->last_max_cwnd = 0;
  testcubic->last_time = 0;
  testcubic->last_cwnd = 0;
}
EXPORT_SYMBOL_GPL(tcp_testcubic_init);

static struct tcp_congestion_ops tcp_testcubic = {
  .cong_avoid = tcp_testcubic_cong_avoid,
  .ssthresh = tcp_testcubic_ssthresh,
  .undo_cwnd = tcp_testcubic_undo_cwnd,
  .set_state = tcp_testcubic_set_state,
  .init = tcp_testcubic_init,
  /*  .cwnd_event = tcp_testcubic_cwnd_event, */

  .owner = THIS_MODULE,
  .name = "testcubic",
};

static int __init tcp_testcubic_register(void)
{
  printk(KERN_INFO "Initializing testcubic\n");
  BUILD_BUG_ON(sizeof(struct testcubic) > ICSK_CA_PRIV_SIZE);

  /* Boilerplate initializations from TCP's cubic implementations. */
	cube_rtt_scale = (bic_scale * 10);	/* 1024*c/rtt */
  cube_factor = 1ull << (10+3*BICTCP_HZ); /* 2^40 */
  do_div(cube_factor, bic_scale * 10);
  
  tcp_register_congestion_control(&tcp_testcubic);
  return 0;
}

static void __exit tcp_testcubic_unregister(void)
{
  printk(KERN_INFO "Exiting testcubic\n");
  tcp_unregister_congestion_control(&tcp_testcubic);
}

/* calculate the cubic root of x using a table lookup followed by one
 * Newton-Raphson iteration.
 * Avg err ~= 0.195%
 */
static u32 cubic_root(u64 a)
{
	u32 x, b, shift;
	/*
	 * cbrt(x) MSB values for x MSB values in [0..63].
	 * Precomputed then refined by hand - Willy Tarreau
	 *
	 * For x in [0..63],
	 *   v = cbrt(x << 18) - 1
	 *   cbrt(x) = (v[x] + 10) >> 6
	 */
	static const u8 v[] = {
		/* 0x00 */    0,   54,   54,   54,  118,  118,  118,  118,
		/* 0x08 */  123,  129,  134,  138,  143,  147,  151,  156,
		/* 0x10 */  157,  161,  164,  168,  170,  173,  176,  179,
		/* 0x18 */  181,  185,  187,  190,  192,  194,  197,  199,
		/* 0x20 */  200,  202,  204,  206,  209,  211,  213,  215,
		/* 0x28 */  217,  219,  221,  222,  224,  225,  227,  229,
		/* 0x30 */  231,  232,  234,  236,  237,  239,  240,  242,
		/* 0x38 */  244,  245,  246,  248,  250,  251,  252,  254,
	};

	b = fls64(a);
	if (b < 7) {
		/* a in [0..63] */
		return ((u32)v[(u32)a] + 35) >> 6;
	}

	b = ((b * 84) >> 8) - 1;
	shift = (a >> (b * 3));

	x = ((u32)(((u32)v[shift] + 10) << b)) >> 6;

	/*
	 * Newton-Raphson iteration
	 *                         2
	 * x    = ( 2 * x  +  a / x  ) / 3
	 *  k+1          k         k
	 */
	x = (2 * x + (u32)div64_u64(a, (u64)x * (u64)(x - 1)));
	x = ((x * 341) >> 10);
	return x;
}

module_init(tcp_testcubic_register);
module_exit(tcp_testcubic_unregister);

MODULE_AUTHOR("Srinivas Narayana");
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("TCP Test Cubic");
