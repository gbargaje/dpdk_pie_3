#include <stdio.h>
#include <rte_random.h>
#include <rte_timer.h>

#define TIMER_RESOLUTION_CYCLES 20000000ULL /* around 10ms at 2 Ghz */
static struct rte_timer timer0;
#include <rte_pie.h>

int
rte_pie_config_init(struct rte_pie_config *config,
		uint32_t target_delay,		//Target Queue Delay
		uint32_t t_update,			//drop rate calculation period
		uint32_t mean_pkt_size,		//Mean packet size in number of packets.
		uint32_t max_burst){

	if(config == NULL)
		return -1;
	if(target_delay == 0)
		return -2;
	if(max_burst == 0)
		return -3;
	if(t_update == 0)
		return -4;
	if(mean_pkt_size == 0)
		mean_pkt_size = 2;

	uint64_t cycles 		= rte_get_timer_hz();		//returns number of cycles in one second.

	config->target_delay 	= target_delay  * cycles / 1000u;		//15ms or number of cycles in 15ms
	config->t_update		= t_update * cycles / 1000u;			//same as above
	config->alpha			= 2;						//0.125 of 2^5
	config->beta			= 20;						//1.25	of 2^5
	config->mean_pkt_size	= mean_pkt_size;			//(2)	Confirm how it should be
	config->max_burst		= max_burst * cycles / 1000u;		//number of cycles in max_burst milliseconds

	return 0;
}

int rte_pie_data_init(struct rte_pie *pie){

	if(pie == NULL)
		return -1;

	uint64_t cycles 		= rte_get_timer_hz();		//returns number of cycles in one second.

	pie->burst_allowance 	= 150 * cycles / 1000u;		//max burst
	pie->drop_prob 			= 0;
	pie->cur_qdelay 		= 0;
	pie->old_qdelay			= 0;
	pie->accu_prob			= 0;

	return 0;
}

/* timer0 callback */
static void
timer0_cb(__attribute__((unused)) struct rte_timer *tim,
      __attribute__((unused)) void *arg)
{
    static unsigned counter = 0;
    unsigned lcore_id = rte_lcore_id();
    printf("%s() on lcore %u\n", __func__, lcore_id);
    if ((counter ++) == 20)
    {
	printf("Stopping timer\n");
        rte_timer_stop(tim);
    }
}

static __attribute__((noreturn)) int
pie_lcore_mainloop(__attribute__((unused)) void *arg)
{
    uint64_t prev_tsc = 0, cur_tsc, diff_tsc;
    unsigned lcore_id;
    lcore_id = rte_lcore_id();
    printf("Starting timer mainloop on core %u\n", lcore_id);
    while (1) {
        cur_tsc = rte_rdtsc();
        diff_tsc = cur_tsc - prev_tsc;
        if (diff_tsc > TIMER_RESOLUTION_CYCLES) {
	    printf("Calling manage\n");
            rte_timer_manage();
            prev_tsc = cur_tsc;
        }
    }
}

void rte_pie_timer_init(void) {
	uint64_t hz;
	unsigned lcore_id;

	rte_timer_subsystem_init();

	rte_timer_init(&timer0);

	hz = rte_get_timer_hz();
	lcore_id = 4;
	
	rte_timer_reset(&timer0, hz, PERIODICAL, lcore_id, timer0_cb, NULL);

	rte_eal_remote_launch(pie_lcore_mainloop, NULL, lcore_id);
}

int rte_pie_drop(struct rte_pie_config *config, struct rte_pie *pie, uint32_t qlen) {

	//Safeguard PIE to be work conserving
	if((pie->cur_qdelay < config->target_delay/2 \
			&& pie->drop_prob < 858993459) \
			|| (qlen <= config->mean_pkt_size)){
		return ENQUEUE;
	}

	uint64_t u = rte_rand() >> 32; //get a 64 bit random number and concert it to 32 bit

	if(u < pie->drop_prob)
		return DROP;
	return ENQUEUE;
}

//Called on each packet arrival
int rte_pie_enqueue(struct rte_pie_config *config, struct rte_pie *pie, uint32_t qlen) {

	//burst allowance is multiple of t_update
	if (pie->burst_allowance == 0 && rte_pie_drop(config, pie, qlen) == DROP)
		return DROP;

	if (pie->drop_prob == 0 \
			&& pie->cur_qdelay < config->target_delay/2 \
			&& pie->old_qdelay< config->target_delay/2) {
		pie->burst_allowance = config->max_burst;
	}
	return ENQUEUE;
}
