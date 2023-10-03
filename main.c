/*
    Actor simulator
*/



#define _GNU_SOURCE
#include <assert.h>
#include <sched.h> /* getcpu */
#include <stdio.h> 
#include <stdlib.h> 
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>   
#include <pthread.h> 
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <sys/syscall.h>

#include "workq.h"

#define BILLION  1000000000L

#define ACTORS_MAX  32
typedef struct actorThreadContext_s {
    workq_t workq_in;
    workq_t workq_out;
    int srcId;
    int instance;
    int state;
    char name[32];
    pthread_t   thread_id;
    int setaffinity;

} actorThreadContext_t;


actorThreadContext_t   g_contexts[ACTORS_MAX + 1];

#define DIR_INSTANCES_MAX 8
typedef struct directoryEntry_s {
    char name[32];
    int instanceCnt;
    actorThreadContext_t *p_instance[DIR_INSTANCES_MAX];
} directoryEntry_t;


#define DIR_ENTRY_MAX 8
typedef struct directory_s {
    int enrtyCnt;
    int srcIdCnt;
    directoryEntry_t entry[DIR_ENTRY_MAX];
} directory_t;


directory_t directory;

int dir_getCount(){
    return directory.enrtyCnt;
}

int dir_register(actorThreadContext_t *p_context){
    int rtc = -1;
    int i;

    for (i =0; i < directory.enrtyCnt; i++) {
        if (strcmp(p_context->name, directory.entry[i].name) == 0) {
            // match add instance
            //printf("match %s\n", p_context->name);
            directory.entry[i].p_instance[directory.entry[i].instanceCnt ] = p_context;
            p_context->instance = directory.entry[i].instanceCnt ;
            p_context->srcId = directory.srcIdCnt;
            p_context->state = 1;        //mark active
            directory.srcIdCnt++;
            directory.entry[i].instanceCnt ++;
            rtc = 0;
            break;
        }
    }
    if (rtc < 0) {
        //new entry
        memcpy(directory.entry[i].name, p_context->name, 32);
        // add instance
       // printf("new entry %s\n",directory.entry[i].name );
        directory.entry[i].instanceCnt = 1;
        directory.entry[i].p_instance[0] = p_context;
        p_context->instance = 0;
        p_context->srcId = directory.srcIdCnt;
        p_context->state = 1;        //mark active
        directory.srcIdCnt++;
        directory.enrtyCnt++;
        rtc = 0;
    }
    return rtc;
}

int dir_lookup(int next, char *p_name ){
    int dst = -1;
    int i;

    for (i= 0; i < directory.enrtyCnt; i++) {
        if (strcmp(p_name, directory.entry[i].name) == 0) {
            if (next < directory.entry[i].instanceCnt ) {
                if( directory.entry[i].p_instance[next]->state ==1){
                    dst = directory.entry[i].p_instance[next]->srcId;
                    break;
                }
            }
        }
    }
    return dst;
}

void dir_print(){
    int i,j;


    printf("DIR:  cnt %d\n", directory.enrtyCnt);
    for (i = 0; i < directory.enrtyCnt; i++) {
        printf("%02d %s  %d\n", i, directory.entry[i].name, directory.entry[i].instanceCnt );
        for (j = 0; j < directory.entry[i].instanceCnt ; j++) {
            printf("                      %2d  %2d\n", j, directory.entry[i].p_instance[j]->srcId);
        }
    }
}


void usage();
void *th_ib_read(void *p_arg);
void *th_ib_write(void *p_arg);
void *th_io_gen(void *p_arg);
void *th_em(void *p_arg);
void *th_ag(void *p_arg);

// commands

#define CMD_CTL_INIT        1
#define CMD_CTL_READY   2
#define CMD_CTL_START    3
#define CMD_CTL_STOP    4
#define CMD_CTL_CLEAR    5
#define CMD_REQ_LAST     8
#define CMD_REQ                  9
#define CMD_REQ_ACK        10

// queue for CLI control thread
workq_t g_workq_cli;

/**
 * 
 * 
 * @author martin (10/2/23)
 * @brief start
 * @param argc 
 * @param argv 
 * 
 * @return int 
 */
int main(int argc, char **argv) {
    //int opt;
    int i_contextNext = 0;
    msg_t                  msg;

    int i;
    unsigned cpu, numa;
    cpu_set_t my_set;        /* Define your cpu_set bit mask. */
    int cliAffinity = 1;

    struct timespec start;
    struct timespec end;
    double accum;
    int total_send = 100;
    int first_count, sec_count;

    //init directory
    directory.enrtyCnt = 0;
    directory.srcIdCnt = 0;


    getcpu(&cpu, &numa);
    printf("CLI %u %u\n", cpu, numa);

    CPU_ZERO(&my_set); 
    if (cliAffinity >= 0) {
        CPU_SET(cliAffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }

    getcpu(&cpu, &numa);
    printf("CLI %u %u\n", cpu, numa);

    workq_init(&g_workq_cli);

    // build actor contexts
    for (i = 0; i < ACTORS_MAX; i++) {
        g_contexts[i].state = 0;
        workq_init(&g_contexts[i].workq_in);
        workq_init(&g_contexts[i].workq_out);
    }

    //ib_read
    for (i = 0; i < 3; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "ib_read");
        dir_register(&g_contexts[i_contextNext]);
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_ib_read, (void *) &g_contexts[i_contextNext]);
    }

    //ib_write
    for (i = 0; i < 3; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "ib_write");
        dir_register(&g_contexts[i_contextNext]);
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_ib_write, (void *) &g_contexts[i_contextNext]);
    }

    //io_gen
    for (i = 0; i < 8; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "io_gen");
        dir_register(&g_contexts[i_contextNext]);
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_io_gen, (void *) &g_contexts[i_contextNext]);
    }

     //emulator
    for (i = 0; i < 2; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "em");
        dir_register(&g_contexts[i_contextNext]);
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_em, (void *) &g_contexts[i_contextNext]);
    }

    // aggregator
    sprintf(g_contexts[ACTORS_MAX].name, "ag");
    //build an init workq for ag
    workq_init(&g_contexts[ACTORS_MAX].workq_in);
    g_contexts[ACTORS_MAX].instance = 0;
    g_contexts[ACTORS_MAX].srcId = ACTORS_MAX;
    pthread_create(&g_contexts[ACTORS_MAX].thread_id, NULL, th_ag, (void *) &g_contexts[ACTORS_MAX]);

    dir_print();

    // send init as all threads registered
    // share queues before aggtrator takes over
  for(i = 0; i <ACTORS_MAX; i++){
      msg.cmd = CMD_CTL_INIT;
      msg.src = -1;
      msg.length = 0;
      if(workq_write(&g_contexts[i].workq_in, &msg)){
          printf("%d q is full\n", g_contexts[i].srcId);
      }
   }
  msg.cmd = CMD_CTL_INIT;
  msg.src = -1;
  msg.length = 0;
  if(workq_write(&g_contexts[ACTORS_MAX].workq_in, &msg)){
      printf("%d q is full\n", g_contexts[ACTORS_MAX].srcId);
  }

  //wait for initialization to complete on all threads
  i = 0;
  while (1) {
         if(workq_read(&g_workq_cli, &msg)){
             if(msg.cmd == CMD_CTL_READY) {
                 i++;
                 printf("thread %d %s ready\n", msg.src, g_contexts[msg.src].name);
             }
          if (i  >= (directory.srcIdCnt +1)) {
              break;
          }
      }
  }

  printf("all threads ready\n");
     sec_count = (int)(total_send / 3);
      first_count = total_send - (sec_count* (2));
  clock_gettime(CLOCK_REALTIME, &start);
  for (i = 0; i < 3; i++) {
      msg.cmd = CMD_CTL_START;
      msg.src = -1;
      if (i == 0) {
          msg.length = first_count;
      }
      else {
          msg.length = sec_count;
      }
      msg.dst = dir_lookup(i, "ib_read");
      if(workq_write(&g_contexts[msg.dst].workq_in, &msg)){
          printf("%d q is full\n", g_contexts[msg.dst].srcId);
      }
  }
  i = 0;
    while (1) {
        //wait for STOP
        if(workq_read(&g_workq_cli, &msg)){
            if(msg.cmd == CMD_CTL_STOP) {
                i++;
                printf("STOP from %d \n", msg.src);
                if (i == 3) {
                    clock_gettime(CLOCK_REALTIME, &end);
                    break;
                }
            }
        }
    }


    accum = ( end.tv_sec - start.tv_sec ) + (double)( end.tv_nsec - start.tv_nsec ) / (double)BILLION;
    printf( "%lf\n", accum );


    return 0;
}

void usage(){
    printf("-h     help\n");
}

/**
 * 
 * 
 * @author martin (10/2/23) 
 *  
 * @brief ib_read thread 
 * 
 * @param p_arg 
 * 
 * @return void* 
 */
#define EM_REQ_MAX 4
void *th_ib_read(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    //cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
    unsigned  int send_cnt = 100;

    int destStatus[ACTORS_MAX];     //track credits

    int i;
    int IOGenCnt = 0;
    int IOGenNext = 0;
    int IOGenDst[ACTORS_MAX];
    int emCnt = 0;
 //   int emNext = 0;
   int emDst[ACTORS_MAX];

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);

/*
    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }
*/

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     //printf("%s%d init now\n", this->name, this->instance);
     //build  destination list ems
     for (i=0; i < ACTORS_MAX; i++) {
         emDst[i] = dir_lookup(i, "em");
         if (emDst[i] >= 0) {
             emCnt++;
         }
         else {
             break;
         }
     }

     //build  io_genn list 
     for (i=0; i < ACTORS_MAX; i++) {
         IOGenDst[i] = dir_lookup(i, "io_gen");
         if (IOGenDst[i] >= 0) {
             IOGenCnt++;
         }
         else {
             break;
         }
     }


    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    // wait for start cmd
    while (1) {
        if(workq_read(&this->workq_in, &msg)){
           if(msg.cmd == CMD_CTL_START){
               send_cnt = msg.length;
               printf("%s_%d START\n", this->name, this->instance);
               break;
           }
        }
    }
    i = 0;
    while (1){
        //only recieves acks
        if(workq_read(&this->workq_in, &msg)){
            if (msg.cmd == CMD_REQ_ACK) {
                if (destStatus[msg.src] > 0) {
                    destStatus[msg.src]  --;
                    printf("ACK from %d  credit %d\n", msg.src, destStatus[msg.src]);
                }
            }
        }

        msg.cmd = CMD_REQ;
        msg.src = this->srcId;
        if (send_cnt > 0) {
            //if(i++ < 20)  printf("select %u\n", send_cnt & 0x0000003 );
            switch (send_cnt & 0x0000003) {
            case 0:
                //use em_0 direct
                msg.dst = emDst[0];
                msg.length = 10;
                break;
            case 1:
                //use em_1 direct
                msg.dst = emDst[1];
                msg.length = 11;
                break;
            case 2:
                //use em_0 via io_gen
                msg.length = 10;
                msg.dst = IOGenDst[IOGenNext];
                IOGenNext++;
                break;
            case 3:
                //use em_1 via io_gen
                msg.length = 11;
                msg.dst = IOGenDst[IOGenNext];
                IOGenNext++;
                break;
            default:
                break;
            }
            if (IOGenNext >=  IOGenCnt ) {
                IOGenNext = 0;
            }
            if (destStatus[msg.dst] < EM_REQ_MAX) {
                //send a work item
                msg.src = this->srcId;
                if (send_cnt == 1) {
                    msg.cmd = CMD_REQ_LAST;
                }
                if (workq_write(&this->workq_out, &msg) == 0) {
                    printf("%s_%d sent %d cmd %d to %d len %d\n",
                           this->name,
                           this->instance,
                           send_cnt,
                           msg.cmd,
                           msg.dst,
                           msg.length);
                    destStatus[msg.dst] ++;
                    send_cnt--;
                }
            }
        }
    }
}



/**
 * 
 * 
 * @author martin (10/2/23) 
 *  
 * @brief ib_write thread 
 * 
 * @param p_arg 
 * 
 * @return void* 
 */
void *th_ib_write(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    //cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     msg_t                  msg_ack;
     //int send_cnt = 0;
     //int emOutstandingRequests = 0;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);

/*
    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }
*/

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here

     //printf("%s%d init now\n", this->name, this->instance);



    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){
        //just recives requests
        if(workq_read(&this->workq_in, &msg)){
            if (msg.cmd == CMD_REQ_LAST ) {
                //tell controller
                msg_ack.cmd = CMD_CTL_STOP  ;
                msg_ack.src = this->srcId;
                msg_ack.length = 0;
                if(workq_write(&g_workq_cli, &msg_ack)){
                    printf("%d q is full\n", this->srcId);
                }
            }
            //send ack
            msg_ack.cmd = CMD_REQ_ACK ;
            msg_ack.dst = msg_ack.src ;
            msg_ack.src = this->srcId;
            if (workq_write(&this->workq_out, &msg_ack)) {
                printf("%d q is full\n", this->srcId);
            }
        }
    }
}

 /**
 * 
 * 
 * @author martin (10/2/23) 
 *  
 * @brief  io_gen thread 
 * 
 * @param p_arg 
 * 
 * @return void* 
 */
void *th_io_gen(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    //cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     //int send_cnt = 0;
     //int emOutstandingRequests = 0;
     int emDst[ACTORS_MAX];
     int  ib_writeDst[ACTORS_MAX];
     int i;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);

/*
    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }
*/

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here

     //printf("%s%d init now\n", this->name, this->instance);
     for (i=0; i < ACTORS_MAX; i++) {
         ib_writeDst[i] = dir_lookup(i, "ib_write");
         if ( ib_writeDst[i] >= 0) {
         } else {
             break;
         }
     }

     //build  destination list ems
     for (i=0; i < ACTORS_MAX; i++) {
         emDst[i] = dir_lookup(i, "em");
         if (emDst[i] >= 0) {
         }
         else {
             break;
         }
     }

    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){


        if(workq_read(&this->workq_in, &msg)){
            // req from ib_read
            //req from em for ib_write
            if (msg.src == emDst[0] ||
                msg.src == emDst[1]  ) {
                //assume IB_WRITE
                //from EM, leave src (for ack)
                //dst to IO_WRITE encoded in length
                if (msg.length == 20) {
                    msg.dst =  ib_writeDst[0];
                }
                else if (msg.length == 21){
                    msg.dst =  ib_writeDst[1];
                 }
                else {
                    // assume 22
                    msg.dst =  ib_writeDst[2];
                }
                if (workq_write(&this->workq_out, &msg)) {
                    printf("%d q is full\n", this->srcId);
                }
            }
            else {
                //req from IB_READ for EM_n (coded in length)
                //leave the src as IB_READ (for ack)
                //change the dst to EM_n
                if (msg.length == 10) {
                    msg.dst = emDst[0];
                }
                else{
                    msg.dst = emDst[1];
                }

                if (workq_write(&this->workq_out, &msg)) {
                    printf("%d q is full\n", this->srcId);
                }
            }
        }
    }
}


/**
 * 
 * 
 * @author martin (10/2/23) 
 *  
 * @brief emulator thread 
 * 
 * @param p_arg 
 * 
 * @return void* 
 */
void *th_em(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    //cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     msg_t                  msg_ack;
     long sent = 0;
     int IOGenCnt = 0;
     int IOGenNext = 0;
     int IOGenDst[ACTORS_MAX];
     int ib_writeCnt = 0;
//     int ib_writeNext = 0;
     int ib_writeDst[ACTORS_MAX];
     int destStatus[ACTORS_MAX];
     int i;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);

/*
    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }
*/

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     //build  ib_write list 
     for (i=0; i < ACTORS_MAX; i++) {
         ib_writeDst[i] = dir_lookup(i, "ib_write");
         if ( ib_writeDst[i] >= 0) {
             ib_writeCnt++;
         } else {
             break;
         }
     }
     //build  io_genn list 
     for (i=0; i < ACTORS_MAX; i++) {
         IOGenDst[i] = dir_lookup(i, "io_gen");
         if (IOGenDst[i] >= 0) {
             IOGenCnt++;
         }
         else {
             break;
         }
     }
     //build  io_genn list 
     for (i=0; i < ACTORS_MAX; i++) {
         IOGenDst[i] = dir_lookup(i, "io_gen");
         if (IOGenDst[i] >= 0) {
             IOGenCnt++;
         }
         else {
             break;
         }
     }
     //build  io_genn list 
     for (i=0; i < ACTORS_MAX; i++) {
         IOGenDst[i] = dir_lookup(i, "io_gen");
         if (IOGenDst[i] >= 0) {
             IOGenCnt++;
         }
         else {
             break;
         }
     }

     //printf("%s%d init now\n", this->name, this->instance);

    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){

        if(workq_read(&this->workq_in, &msg)){
            // req or req_last from ib_read
            // req or req_last vi io_gen
            // acks from req's to IB_WRITE
            if (msg.cmd == CMD_REQ_ACK) {
                if (destStatus[msg.src] > 0) {
                    destStatus[msg.src]  --;
                }
            }
            else {
                //assume req
                //send ack
                printf("%s_%d cmd %d from %d\n",
                       this->name,
                       this->instance,
                       msg.cmd,
                       msg.src);
                msg_ack.src = this->srcId;
                msg_ack.dst = msg.src;
                msg_ack.cmd = CMD_REQ_ACK;
                if(workq_write(&this->workq_out, &msg_ack)){
                }
                // send it on to IB_WRITE
                switch (sent & 0x0000007) {
                case  0:
                    //send to ib_write_0 direct
                    msg.length = 20;
                    msg.dst = ib_writeDst[0];
                    break;
                case 1:
                    //send to ib_write_1 direct
                    msg.length = 21;
                    msg.dst = ib_writeDst[1];
                    break;
                case 2:
                    //send to ib_write_2 direct
                    msg.length = 22;
                    msg.dst = ib_writeDst[2];
                    break;
                case 3:
                    //send to ib_write_0 via io_gen
                    msg.length = 20;
                    msg.dst = IOGenDst[IOGenNext];
                    IOGenNext++;  
                    break;
                case 4:
                    //send to ib_write_1 via io_gen
                    msg.length = 21;
                    msg.dst = IOGenDst[IOGenNext];
                    IOGenNext++;  
                    break;
                case 5:
                    //send to ib_write_2 via io_gen
                    msg.length = 22;
                    msg.dst = IOGenDst[IOGenNext];
                    IOGenNext++;  
                    break;
                case 6:
                    //send to ib_write_0 direct
                    msg.length = 20;
                    msg.dst = ib_writeDst[0];
                    break;
                case 7:
                    //send to ib_write_1 direct
                    msg.length = 21;
                    msg.dst = ib_writeDst[1];
                    break;
                default:
                    break;
                }
                msg.src = this->srcId;
                //msg.cmd = CMD_REQ_ACK;
                if(workq_write(&this->workq_out, &msg_ack)){
                }
                else{
                    sent++;
                }
            }
        }
    }
}


/**
 * 
 * 
 * @author martin (10/2/23) 
 *  
 * @brief aggregator thread 
 * 
 * @param p_arg 
 * 
 * @return void* 
 */
void *th_ag(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    //cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     //int send_cnt = 0;
     //int emOutstandingRequests = 0;
     int i;
     int activeWorkQs = 0;
     workq_t *p_workin_qs[ACTORS_MAX];
     workq_t *p_workout_qs[ACTORS_MAX];
     workq_t *p_workq_out;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);

/*
    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }
*/

     while (1) {
         if(workq_read(&this->workq_in, &msg)){
            if(msg.cmd == CMD_CTL_INIT){
                break;
            }
         }
     }

     //init code here
     //all active thread have been registered in the directory
     for (i = 0; i < ACTORS_MAX; i++) {
         if (g_contexts[i].state == 1) {
             //active
             p_workin_qs[activeWorkQs] = &g_contexts[i].workq_out;
             p_workout_qs[activeWorkQs] = &g_contexts[i].workq_in;
            activeWorkQs++;
         }
     }

    printf("%s%d init now activeqs %d\n", this->name, this->instance,  activeWorkQs);

    msg.cmd = CMD_CTL_READY;
    msg.src = ACTORS_MAX;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", ACTORS_MAX);
    }

    //no need to wait for start
    i = 0;
    while (1){
        if(workq_read(p_workin_qs[i], &msg)){
            printf("ag src %d dst %d\n", msg.src, msg.dst);
            // use dst
            p_workq_out = p_workout_qs[msg.dst];
            if(workq_write(p_workq_out , &msg)){
                printf("%d q is full\n", msg.dst);
            }
            i++;
            if (i >=  activeWorkQs ) {
                 i = 0;
            }
        }
    }
}



