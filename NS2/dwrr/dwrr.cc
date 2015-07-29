#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <float.h>
#include "flags.h"
#include "dwrr.h"

#define max(arg1,arg2) (arg1>arg2 ? arg1 : arg2)
#define min(arg1,arg2) (arg1<arg2 ? arg1 : arg2)


/* Insert a queue to the tail of an active list. Return true if insert succeeds */
static void InsertTailList(PacketDWRR* list, PacketDWRR *q)
{
	if(q!=NULL && list !=NULL)
	{
		PacketDWRR* tmp=list;
		while(true)
		{
			/* Arrive at the tail of this list */
			if(tmp->next==NULL)
			{
				tmp->next=q;
				q->next=NULL;
				return;
			}
			/* Move to next node */
			else
			{
				tmp=tmp->next;
			}
		}
	}
}

/* Remove and return the head node from the active list */
static PacketDWRR* RemoveHeadList(PacketDWRR* list)
{
	if(list!=NULL)
	{
		PacketDWRR* tmp=list->next;
		if(tmp!=NULL)
		{
			list->next=tmp->next;
			return tmp;
		}
		/* This list is empty */
		else
		{
			return NULL;
		}
	}
	else
	{
		return NULL;
	}
}

static class DWRRClass : public TclClass 
{
	public:
		DWRRClass() : TclClass("Queue/DWRR") {}
		TclObject* create(int argc, const char*const* argv) 
		{
			return (new DWRR);
		}
} class_dwrr;

DWRR::DWRR()
{		
	queues=new PacketDWRR[MAX_QUEUE_NUM];
	activeList=new PacketDWRR();
	init=false;
	round_time=0;
	quantum_sum=0;
	quantum_sum_estimate=0;
	
	total_qlen_tchan_=NULL;
	qlen_tchan_=NULL;
	
	/* bind variables */
	bind("queue_num_", &queue_num_);
	bind("mean_pktsize_", &mean_pktsize_);
	bind("port_thresh_",&port_thresh_);
	bind("marking_scheme_",&marking_scheme_);
	bind("estimate_round_alpha_",&estimate_round_alpha_);
	bind("estimate_quantum_alpha_",&estimate_quantum_alpha_);
	bind_bool("estimate_round_filter_",&estimate_round_filter_);
	bind_bw("link_capacity_",&link_capacity_);
	bind_bool("debug_",&debug_);
}

DWRR::~DWRR() 
{
	delete activeList;
	delete [] queues;
}

/* Get total length of all queues in bytes */
int DWRR::TotalByteLength()
{
	int result=0;
	for(int i=0;i<queue_num_;i++)
	{
		result+=queues[i].byteLength();
	}
	return result;
}

/* Get total length of all queues in packets */
int DWRR::TotalLength()
{
	int result=0;
	for(int i=0;i<queue_num_;i++)
	{
		result+=queues[i].length();
	}
	return result;
}


int DWRR::TotalQuantum()
{
	int result=0;
	for(int i=0;i<queue_num_;i++)
	{
		result+=queues[i].quantum;
	}
	return result;
}

/* Determine whether we need to mark ECN where q is current queue number. Return 1 if it requires marking */
int DWRR::MarkingECN(int q)
{	
	if(q<0||q>=queue_num_)
	{
		fprintf (stderr,"illegal queue number\n");
		exit (1);
	}
	
	/* Per-queue ECN marking */
	if(marking_scheme_==PER_QUEUE_MARKING)
	{
		if(queues[q].byteLength()>queues[q].thresh*mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* Per-port ECN marking */
	else if(marking_scheme_==PER_PORT_MARKING)
	{
		if(TotalByteLength()>port_thresh_*mean_pktsize_)
			return 1;
		else
			return 0;
	}
	/* MQ-ECN for any packet scheduling algorithms */
	else if(marking_scheme_==MQ_MARKING_GENER)
	{
		if(queues[q].byteLength()>=queues[q].thresh*mean_pktsize_&&quantum_sum>0)
		{
			quantum_sum_estimate=quantum_sum_estimate*estimate_quantum_alpha_+quantum_sum*(1-estimate_quantum_alpha_);
			if(debug_)
				printf("sample quantum sum: %d smooth quantum sum: %f\n",quantum_sum,quantum_sum_estimate);
			double thresh=min(queues[q].quantum/quantum_sum_estimate,1)*port_thresh_;
			if(queues[q].byteLength()>thresh*mean_pktsize_)
				return 1;
			else
				return 0;
		}
		else
		{
			return 0;
		}
	}
	/* MQ-ECN for round robin packet scheduling algorithms */
	else if(marking_scheme_==MQ_MARKING_RR)
	{	
		if(queues[q].byteLength()>=queues[q].thresh*mean_pktsize_&&round_time>0.000000001&&link_capacity_>0)
		{			
			double weightedFairRate=queues[q].quantum*8/round_time;
			double thresh=min(weightedFairRate/link_capacity_,1)*port_thresh_;
			//For debug
			//printf("round time: %f threshold: %f\n",round_time, thresh);
			if(queues[q].byteLength()>thresh*mean_pktsize_)
				return 1;
			else
				return 0;
		}
		else
		{
			return 0;
		}
	}
	/* Unknown ECN marking scheme */
	else
	{
		fprintf (stderr,"Unknown ECN marking scheme\n");
		return 0;
	}
}

/* 
 *  entry points from OTcL to set per queue state variables
 *   - $q set-quantum queue_id queue_quantum (quantum is actually weight)
 *   - $q set-thresh queue_id queue_thresh
 *   - $q attach-total file
 *	  - $q attach-queue file
 *
 *  NOTE: $q represents the discipline queue variable in OTcl.
 */
int DWRR::command(int argc, const char*const* argv)
{
	if(argc==3)
	{
		// attach a file to trace total queue length
		if (strcmp(argv[1], "attach-total") == 0) 
		{
			int mode;
			const char* id = argv[2];
			Tcl& tcl = Tcl::instance();
			total_qlen_tchan_=Tcl_GetChannel(tcl.interp(), (char*)id, &mode);
			if (total_qlen_tchan_==0) 
			{
				tcl.resultf("DWRR: trace: can't attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
		else if (strcmp(argv[1], "attach-queue") == 0) 
		{
			int mode;
			const char* id = argv[2];
			Tcl& tcl = Tcl::instance();
			qlen_tchan_=Tcl_GetChannel(tcl.interp(), (char*)id, &mode);
			if (qlen_tchan_==0) 
			{
				tcl.resultf("DWRR: trace: can't attach %s for writing", id);
				return (TCL_ERROR);
			}
			return (TCL_OK);
		}
	}
	else if (argc == 4) 
	{
		if (strcmp(argv[1], "set-quantum")==0) 
		{
			int queue_id=atoi(argv[2]);
			if(queue_id<queue_num_&&queue_id>=0)
			{
				int quantum=atoi(argv[3]);
				if(quantum>0)
				{
					queues[queue_id].quantum=quantum;
					return (TCL_OK);
				}
				else
				{
					fprintf (stderr,"illegal quantum value %s for queue %s\n", argv[3], argv[2]);
					exit (1);
				}
			}
			/* Exceed the maximum queue number or smaller than 0*/
			else
			{
				fprintf (stderr,"no such queue %s\n",argv[2]);
				exit (1);
			}
		}
		else if(strcmp(argv[1], "set-thresh")==0)
		{
			int queue_id=atoi(argv[2]);
			if(queue_id<queue_num_&&queue_id>=0)
			{
				double thresh=atof(argv[3]);
				if(thresh>=0)
				{
					queues[queue_id].thresh=thresh;
					return (TCL_OK);
				}
				else
				{
					fprintf (stderr,"illegal thresh value %s for queue %s\n", argv[3],argv[2]);
					exit (1);
				}
			}
			/* Exceed the maximum queue number or smaller than 0*/
			else
			{
				fprintf (stderr,"no such queue %s\n",argv[2]);
				exit (1);
			}
		}
	}
	return (Queue::command(argc, argv));	
}

/* Receive a new packet */
void DWRR::enque(Packet *p)
{
	hdr_ip *iph=hdr_ip::access(p);
	int prio=iph->prio();
	hdr_flags* hf=hdr_flags::access(p);
	int pktSize=hdr_cmn::access(p)->size();
	int qlimBytes=qlim_*mean_pktsize_;
	queue_num_=min(queue_num_,MAX_QUEUE_NUM);
	
	//For debug
	//printf("%f",link_capacity_);
	if(!init)
	{
		round_time=TotalQuantum()*8/link_capacity_;
		init=true;
	}
	
	/* The shared buffer is overfilld */
	if(TotalByteLength()+pktSize>qlimBytes)
	{
		drop(p);
		//printf("Packet drop\n");
		return;
	}
	
	if(prio>=queue_num_||prio<0)
		prio=queue_num_-1;

	queues[prio].enque(p); 	
	/* if queues[prio] is not in activeList */
	if(queues[prio].active==false)
	{
		queues[prio].deficitCounter=0;
		queues[prio].active=true;
		queues[prio].current=false;
		queues[prio].start_time=Scheduler::instance().clock();	//Start time of this round
		InsertTailList(activeList, &queues[prio]);
		quantum_sum+=queues[prio].quantum;
	}
	
	/* Enqueue ECN marking */
	if(MarkingECN(prio)>0&&hf->ect())	
		hf->ce() = 1;
	
	trace_qlen();
	trace_total_qlen();
}

Packet *DWRR::deque(void)
{
	PacketDWRR *headNode=NULL;
	Packet *pkt=NULL;
	Packet *headPkt=NULL;
	int pktSize;
	
	/*At least one queue is active, activeList is not empty */
	if(TotalByteLength()>0)
	{
		/* We must go through all actives queues and select a packet to dequeue */
		while(1)
		{
			headNode=activeList->next;	//Get head node from activeList					
			if(headNode==NULL)
				fprintf (stderr,"no active flow\n");
			
			/* if headNode is not empty */
			if(headNode->length()>0)
			{
				/* headNode has not been served yet in this round */
				if(headNode->current==false)
				{
					headNode->deficitCounter+=headNode->quantum;
					headNode->current=true;
				}
				headPkt=headNode->head();
				pktSize=hdr_cmn::access(headPkt)->size();
				
				/* if we have enough quantum to dequeue the head packet */
				if(pktSize<=headNode->deficitCounter)
				{
					pkt=headNode->deque();
					headNode->deficitCounter-=pktSize;
					//printf("deque a packet\n");
					
					/* After dequeue, headNode becomes empty queue */
					if(headNode->length()==0)
					{
						/* If we enable estimate_round_filter_, we only sample when this queue exactly uses up its quantum */
						if((headNode->deficitCounter==0&&estimate_round_filter_)||(!estimate_round_filter_))
						{
							/* The packet has not been sent yet */
							double round_time_sample=Scheduler::instance().clock()-headNode->start_time+pktSize*8/link_capacity_;
							round_time=round_time*estimate_round_alpha_+round_time_sample*(1-estimate_round_alpha_);
							if(debug_&&marking_scheme_==MQ_MARKING_RR)	
								printf("sample round time: %.9f round time: %.9f\n",round_time_sample,round_time);
						}
						quantum_sum-=headNode->quantum;
						headNode=RemoveHeadList(activeList);	//Remove head node from activeList
						headNode->deficitCounter=0;
						headNode->active=false;
						headNode->current=false;
					}
					break;
				}
				/* if we don't have enough quantum to dequeue the head packet and the queue is not empty */
				else
				{
					headNode=RemoveHeadList(activeList);	
					headNode->current=false;
					double round_time_sample=Scheduler::instance().clock()-headNode->start_time;
					//printf ("now %f start time %f\n", Scheduler::instance().clock(),headNode->start_time);
				  	round_time=round_time*estimate_round_alpha_+round_time_sample*(1-estimate_round_alpha_);
					if(debug_&&marking_scheme_==MQ_MARKING_RR)	
						printf("sample round time: %.9f round time: %.9f\n",round_time_sample,round_time);
					headNode->start_time=Scheduler::instance().clock();	//Reset start time 
					InsertTailList(activeList, headNode);
				}
			}
		}	
	}
	
	return pkt;
}

/* routine to write total qlen records */
void DWRR::trace_total_qlen()
{
	if (total_qlen_tchan_) 
	{
		char wrk[500]={0};
		int n;
		double t = Scheduler::instance().clock();
		sprintf(wrk, "%g, %d", t,TotalByteLength());
		n=strlen(wrk);
		wrk[n] = '\n'; 
		wrk[n+1] = 0;
		(void)Tcl_Write(total_qlen_tchan_, wrk, n+1);
	}
}

/* routine to write per-queue qlen records */
void DWRR::trace_qlen()
{
	if (qlen_tchan_) 
	{
		char wrk[500]={0};
		int n;
		double t = Scheduler::instance().clock();
		sprintf(wrk, "%g", t);
		n=strlen(wrk);
		wrk[n]=0; 
		(void)Tcl_Write(qlen_tchan_, wrk, n);
		
		for(int i=0;i<queue_num_; i++)
		{
			sprintf(wrk, ", %d",queues[i].byteLength());
			n=strlen(wrk);
			wrk[n]=0; 
			(void)Tcl_Write(qlen_tchan_, wrk, n);
		}
		(void)Tcl_Write(qlen_tchan_, "\n", 1);
	}
}