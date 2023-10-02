#include <stddef.h>
#include <stdio.h>
#include <pthread.h>
#include <string.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "workq.h"



int workq_init(workq_t *p_q) {
    p_q->head = 0;
    p_q->tail = 0;
    //pthread_mutex_init(&p_q->lock, NULL);
    pthread_spin_init(&p_q->lock, PTHREAD_PROCESS_SHARED);
    return 0; 
}


inline int workq_available(workq_t *p_q)
{
	if (p_q->tail < p_q->head)
		return p_q->head - p_q->tail - 1;
	else
		return p_q->head + (FIFO_DEPTH_MAX - p_q->tail);
}


int workq_write(workq_t *p_q, msg_t *p_msg){
    int rc = 0;
    //pthread_mutex_lock(&p_q->lock);
    pthread_spin_lock(&p_q->lock);
    if (workq_available(p_q) == 0) // when queue is full
    {
        //printf("xQueue is full\n");
        rc = 1;
    }
    else
    {
        //memcpy(&p_q->event[p_q->tail],p_msg, sizeof(msg_t));  //avoid library calls and simd usage
        p_q->event[p_q->tail].cmd     = p_msg->cmd;
        p_q->event[p_q->tail].src        = p_msg->src;
        p_q->event[p_q->tail].length = p_msg->length;

        //printf("=>%s write event[%d]", p_q->name, p_q->tail);
        (p_q->tail)++;
        (p_q->tail) %= FIFO_DEPTH_MAX;
    }
    //pthread_mutex_unlock(&p_q->lock);
    pthread_spin_unlock(&p_q->lock);
    return rc;
}   



int workq_read(workq_t *p_q, msg_t *p_msg){
    int rtc = 0;
    //pthread_mutex_lock(&p_q->lock);
    pthread_spin_lock(&p_q->lock);
    if (p_q->head != p_q->tail){

        //memcpy(p_msg,&p_q->event[p_q->head], sizeof(msg_t));
        p_msg->cmd =      p_q->event[p_q->head].cmd     ; 
        p_msg->src =         p_q->event[p_q->head].src        ; 
        p_msg->length = p_q->event[p_q->head].length ; 
        //printf("<=%s read event[%d]", p_q->name, p_q->head);
        (p_q->head)++;
        (p_q->head) %= FIFO_DEPTH_MAX;
        rtc = 1;
    }
   //pthread_mutex_unlock(&p_q->lock);
   pthread_spin_unlock(&p_q->lock);
   return rtc;

}


   
