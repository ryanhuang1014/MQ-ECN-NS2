#ifndef ns_wf2q_h
#define ns_wf2q_h

#include "queue.h"
#include "config.h"

/*By default, we use 8 queuess */
#define WF2Q_QUEUES 8	

struct QueueState
{
	PacketQueue* q_;	/* packet queue associated to the corresponding service */
	double weight;	/* Weight of the service */
    long double S;	/* Starting time of the queue , not checked for wraparound*/
    long double F;	/* Ending time of the queue, not checked for wraparound */
};

class WF2Q : public Queue 
{
	public:
		WF2Q();
		~WF2Q();
		virtual int command(int argc, const char*const* argv);
		
	protected:
		Packet *deque(void);
		void enque(Packet *pkt);
		int TotalByteLength();	/* Get total length of all queues in bytes */
		int WeightedThresh();	/* return ECN marking threshold for weight '1' */
		
		/* Variables */
		struct QueueState *qs;	/* underlying multi-FIFO (CoS) queues and their states */
		long double V;	/* Virtual time , not checked for wraparound!*/
		int queue_num_;	/*Number of queues */
		int port_ecn_marking_;	/* Enable per-port ECN marking or not */
		int mean_pktsize_;	/* MTU in bytes */
		int port_low_thresh_;	/* The low per-port ECN marking threshold (pkts)*/
		int port_high_thresh_;	/* The high per-port ECN marking threshold (pkts)*/
		int queue_thresh_;	/* The per-queue ECN marking threshold (pkts)*/
};

#endif