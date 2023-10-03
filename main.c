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

extern int  menuInit();
extern void menuLoop();
extern int  menuAddItem(char *name, int (*cbFn)(int, char *argv[]) , char *help);

int cbGetStats(int argc, char *argv[] );

#define BILLION  1000000000L

#define ACTORS_MAX  32
#define EM_REQ_MAX 8

typedef struct actorThreadContext_s {
    workq_t workq_in;
    workq_t workq_out;
    int srcId;
    int instance;
    int state;
    char name[32];
    pthread_t   thread_id;
    int setaffinity;

    //stats
    unsigned int msg_read[ACTORS_MAX];
    unsigned int msg_write[ACTORS_MAX];
    unsigned int errors;

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
    return directory.srcIdCnt;
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
            p_context->setaffinity = 2 + directory.srcIdCnt;
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
        p_context->setaffinity = 2 + directory.srcIdCnt;
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
    int total_send = 1000000;
    int first_count, sec_count;

    //init directory
    directory.enrtyCnt = 0;
    directory.srcIdCnt = 0;

    menuInit();
    menuAddItem("stats", cbGetStats, "get stats");

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
        g_contexts[i_contextNext].setaffinity = 6 + i;
        //g_contexts[i_contextNext].setaffinity = 6 + i;
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_ib_read, (void *) &g_contexts[i_contextNext]);
    }

    //ib_write
    for (i = 0; i < 3; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "ib_write");
        dir_register(&g_contexts[i_contextNext]);
        g_contexts[i_contextNext].setaffinity = 18 + i;
        //g_contexts[i_contextNext].setaffinity = 2 + i;
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_ib_write, (void *) &g_contexts[i_contextNext]);
    }

    //io_gen
    for (i = 0; i < 8; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "io_gen");
        dir_register(&g_contexts[i_contextNext]);
 /*       if (i < 4) {
            g_contexts[i_contextNext].setaffinity = 2 + i;
        }
        else {
            g_contexts[i_contextNext].setaffinity = (14 - 4) + i;
        }
        */
        switch(i){                                                                                     
            case 0: g_contexts[i_contextNext].setaffinity =2; break;           // 14; break;                                 
            case 1: g_contexts[i_contextNext].setaffinity =3; break;           // 15; break;                                 
            case 2: g_contexts[i_contextNext].setaffinity =4; break;           // 16; break;                                 
            case 3: g_contexts[i_contextNext].setaffinity =5; break;           // 18; break;                                 
            case 4: g_contexts[i_contextNext].setaffinity =14; break;           // 19; break;                                 
            case 5: g_contexts[i_contextNext].setaffinity =15; break;           // 20; break;                                 
            case 6: g_contexts[i_contextNext].setaffinity =16; break;           // 21; break;                                 
            case 7: g_contexts[i_contextNext].setaffinity =17; break;             // 9; break;                                
            default: break;                                                                         
        }
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_io_gen, (void *) &g_contexts[i_contextNext]);
    }

     //emulator
    for (i = 0; i < 2; i++, i_contextNext++) {
        sprintf(g_contexts[i_contextNext].name, "em");
        dir_register(&g_contexts[i_contextNext]);
        //g_contexts[i_contextNext].setaffinity = 21 + i;
        switch (i){
        case 0: g_contexts[i_contextNext].setaffinity = 9; break;                  //5; break;       
        case 1: g_contexts[i_contextNext].setaffinity = 10; break;               //10; break;    
            default: break;
        }
        pthread_create(&g_contexts[i_contextNext].thread_id, NULL, th_em, (void *) &g_contexts[i_contextNext]);
    }

    // aggregator
    sprintf(g_contexts[ACTORS_MAX].name, "ag");
    //build an init workq for ag
    workq_init(&g_contexts[ACTORS_MAX].workq_in);
    g_contexts[ACTORS_MAX].instance = 0;
    g_contexts[ACTORS_MAX].srcId = ACTORS_MAX;
    g_contexts[ACTORS_MAX].setaffinity = 11;
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
             //printf("%2d from %d \n", msg.cmd, msg.src);
            if(msg.cmd == CMD_CTL_STOP) {
                i++;
                if (i == 3) {
                    clock_gettime(CLOCK_REALTIME, &end);
                    break;
                }
            }
        }
       // menuLoop();
    }


    accum = ( end.tv_sec - start.tv_sec ) + (double)( end.tv_nsec - start.tv_nsec ) / (double)BILLION;
    printf( "%lf\n", accum );

    while (1) {
        menuLoop();
    }

    return 0;
}

void usage(){
    printf("-h     help\n");
}
/**
 * 
 * 
 * @author martin (10/3/23)
 * 
 * @param argc  ignored
 * @param argv ignored
 * 
 * @return int  ignored
 */
typedef struct {
    unsigned int msg_read[ACTORS_MAX];
    unsigned int msg_write[ACTORS_MAX];
    unsigned int errors;
} statsIntance_t;

statsIntance_t work_stats[ACTORS_MAX +1];

int cbGetStats(int argc, char *argv[] )
{
    //actorThreadContext_t   *p_workContext;
    int i;
    int row;
    int active = dir_getCount();
    //g_contexts[ACTORS_MAX + 1];

    //get stats
    for (i =0; i < active; i++) {
        memcpy((void *)&work_stats[i], (void *)&g_contexts[i].msg_read[0], sizeof(statsIntance_t));
    }
    memcpy((void *)&work_stats[ACTORS_MAX ], (void *)&g_contexts[ACTORS_MAX] .msg_read[0], sizeof(statsIntance_t));
    //re format
    printf("RX         ");
    for (i = 0; i < active; i++) {
        printf("  %7s", g_contexts[i].name);
    }
    printf("  %7s\n", g_contexts[ACTORS_MAX].name);

    for (row = 0; row < active; row++) {
        printf("%2d  %8s: ", row, g_contexts[row].name);
        for (i = 0; i < active; i++) {
            printf("  %7u",  work_stats[i].msg_read[row]);
        }
        printf("  %7u\n",  work_stats[ACTORS_MAX].msg_read[row]);
    }
    printf("Totals     ib_read  %u  ", 
           work_stats[0].msg_read[14]  + work_stats[1].msg_read[14]  + work_stats[2].msg_read[14]  +
           work_stats[0].msg_read[15]  + work_stats[1].msg_read[15]  + work_stats[2].msg_read[15] 
            );

    printf("   ib_write %u  ", 
           work_stats[3].msg_read[14]  + work_stats[4].msg_read[14]  + work_stats[5].msg_read[14]  +
           work_stats[3].msg_read[15]  + work_stats[4].msg_read[15]  + work_stats[5].msg_read[15]  
            );

    printf("    em %u  \n", 
           work_stats[14].msg_read[0]  + work_stats[15].msg_read[0]    +
           work_stats[14].msg_read[1]  + work_stats[15].msg_read[1]    +
           work_stats[14].msg_read[2]  + work_stats[15].msg_read[2]    
            );

    printf("TX         \n");

    for (row = 0; row < active; row++) {
        printf("%2d  %8s: ", row, g_contexts[row].name);
        for (i = 0; i < active; i++) {
            printf("  %7u",  work_stats[i].msg_write[row]);
        }
        printf("  %7u\n",  work_stats[ACTORS_MAX].msg_write[row]);
    }

    printf("    %8s: ",    "errors");
    for (i = 0; i < active; i++) {
        printf("  %7u",  work_stats[i].errors);
    }
    printf("  %7u\n",  work_stats[ACTORS_MAX].errors);

    return 0;
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

void *th_ib_read(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
    unsigned  int send_cnt = 100;

    int destStatus[ACTORS_MAX];     //track credits

    int i;
    int IOGenCnt = 0;
    int IOGenNext = 0;
    int IOGenDst[ACTORS_MAX];
    int emCnt = 0;
    int indirectDst;
 //   int emNext = 0;
   int emDst[ACTORS_MAX];

   printf("Thread_%d PID %d %d %s %d %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance, this->setaffinity);


    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }


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
            this->msg_read[msg.src]++;
            if (msg.cmd == CMD_REQ_ACK) {
                if (destStatus[msg.src] > 0) {
                    destStatus[msg.src]  --;
                   // printf("ACK from %d  credit %d\n", msg.src, destStatus[msg.src]);
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
                indirectDst = emDst[0];
                msg.length = 10;
                break;
            case 1:
                //use em_1 direct
                msg.dst = emDst[1];
                indirectDst = emDst[1];
                msg.length = 11;
                break;
            case 2:
                //use em_0 via io_gen
                msg.length = 10;
                msg.dst = IOGenDst[IOGenNext];
                indirectDst = emDst[0];
                IOGenNext++;
                break;
            case 3:
                //use em_1 via io_gen
                msg.length = 11;
                msg.dst = IOGenDst[IOGenNext];
                indirectDst = emDst[1];
                IOGenNext++;
                break;
            default:
                break;
            }
            if (IOGenNext >=  IOGenCnt ) {
                IOGenNext = 0;
            }
            if (destStatus[indirectDst] < EM_REQ_MAX) {
                //send a work item
                msg.src = this->srcId;
                if (send_cnt == 1) {
                    msg.cmd = CMD_REQ_LAST;
                    //printf("%s%d CMD_REQ_LAST\n ", this->name, this->instance);
                }
                if (workq_write(&this->workq_out, &msg) == 0) {
                    this->msg_write[msg.dst]++;
  /*                  printf("%s_%d sent %d cmd %d to %d len %d\n",
                           this->name,
                           this->instance,
                           send_cnt,
                           msg.cmd,
                           msg.dst,
                           msg.length);
                           */
                    destStatus[indirectDst] ++;
                    send_cnt--;
                }
                else {
                    this->errors++;
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
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     msg_t                  msg_ack;
     //int send_cnt = 0;
     //int emOutstandingRequests = 0;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);


    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }


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
            this->msg_read[msg.src]++;
            if (msg.cmd == CMD_REQ_LAST ) {
                //tell controller
                //printf("%s%d CMD_REQ_LAST\n", this->name, this->instance);
                msg_ack.cmd = CMD_CTL_STOP  ;
                msg_ack.src = this->srcId;
                msg_ack.length = 0;
                if(workq_write(&g_workq_cli, &msg_ack)){
                    //printf("%d q is full\n", this->srcId);
                    this->errors++;
                }
            }
            //send ack
            msg_ack.cmd = CMD_REQ_ACK ;
            msg_ack.dst = msg.src ;
            msg_ack.src = this->srcId;
           // printf("%s%d ACK dst %d\n", this->name, this->instance, msg_ack.dst);
            if (workq_write(&this->workq_out, &msg_ack)) {
                //printf("%d q is full\n", this->srcId);
                this->errors++;
            }
            else{
                this->msg_write[msg_ack.dst]++;
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
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     //int send_cnt = 0;
     //int emOutstandingRequests = 0;
     int emDst[ACTORS_MAX];
     int  ib_writeDst[ACTORS_MAX];
     int i;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);


    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }


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
            this->msg_read[msg.src]++;
           // printf("%s%d  read cmd %d src %d dst %d  len %d\n", this->name, this->instance, msg.cmd, msg.src, msg.dst, msg.length);
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
                //printf("%s%d send cmd %d  dst %d\n", this->name, this->instance, msg.cmd, msg.dst);
                if (workq_write(&this->workq_out, &msg)) {
                    //printf("%d q is full\n", this->srcId);
                    this->errors++;
                }
                else{
                    this->msg_write[msg.dst]++;
                }
            }
            else {
                //printf("%s%d  cmd %d src %d dst %d for em\n", this->name, this->instance, msg.cmd, msg.src, msg.dst);
                //req from IB_READ for EM_n (coded in length)
                //leave the src as IB_READ (for ack)
                //change the dst to EM_n
                if (msg.length == 10) {
                    msg.dst = emDst[0];
                }
                else{
                    msg.dst = emDst[1];
                }
                 //printf("  -> %d\n", msg.dst);
                if (workq_write(&this->workq_out, &msg)) {
                    //printf("%d q is full\n", this->srcId);
                    this->errors++;
                }
                else{
                    this->msg_write[msg.dst]++;
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
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     msg_t                  msg_ack;
     unsigned int sent = 0;
     int IOGenCnt = 0;
     int IOGenNext = 0;
     int IOGenDst[ACTORS_MAX];
     int ib_writeCnt = 0;
//     int ib_writeNext = 0;
     int ib_writeDst[ACTORS_MAX];
     int destStatus[ACTORS_MAX];
     int i;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);


    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }


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

     //printf("%s%d init now\n", this->name, this->instance);

    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }
 

    while (1){

        if(workq_read(&this->workq_in, &msg)){
            this->msg_read[msg.src]++;
            // req or req_last from ib_read
            // req or req_last vi io_gen
            // acks from req's to IB_WRITE
            if (msg.cmd == CMD_REQ_ACK) {
               // printf("em recvd CMD_REQ_ACK from  %d\n ", msg.src);
                if (destStatus[msg.src] > 0) {
                    destStatus[msg.src]  --;
                }
            }
            else {
                //assume req
                //send ack
 /*               printf("%s_%d cmd %d from %d\n",
                       this->name,
                       this->instance,
                       msg.cmd,
                       msg.src);
                       */
                msg_ack.src = this->srcId;
                msg_ack.dst = msg.src;
                msg_ack.cmd = CMD_REQ_ACK;
                if(workq_write(&this->workq_out, &msg_ack)){
                    //printf("err1\n");
                    this->errors++;
                }
                else{
                    this->msg_write[msg_ack.dst]++;
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
                if (IOGenNext >= IOGenCnt) {
                    IOGenNext = 0;
                }
                msg.src = this->srcId;
                //printf("%s%d send to ib_w cmd %d src %d dst %d len %d sent %d\n", this->name, this->instance, msg.cmd, msg.src, msg.dst, msg.length, sent);
                if(workq_write(&this->workq_out, &msg)){
                    //printf("err\n");
                    this->errors++;
                }
                else{
                    this->msg_write[msg.dst]++;
                     destStatus[msg.dst] ++;
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
    cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
     //int send_cnt = 0;
     //int emOutstandingRequests = 0;
     int i;
     int activeWorkQs = 0;
     workq_t *p_workin_qs[ACTORS_MAX];
     workq_t *p_workout_qs[ACTORS_MAX];
     workq_t *p_workq_out;

   printf("Thread_%d PID %d %d %s %d\n", this->srcId, getpid(), gettid(),  this->name, this->instance);


    CPU_ZERO(&my_set); 
    if (this->setaffinity >= 0) {
        CPU_SET(this->setaffinity, &my_set);
        sched_setaffinity(0, sizeof(cpu_set_t), &my_set);
    }


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
            this->msg_read[msg.src]++;
            // use dst
            p_workq_out = p_workout_qs[msg.dst];
            //printf("ag src %d dst %d cmd %d\n", msg.src, msg.dst, msg.cmd);
            if(workq_write(p_workq_out , &msg)){
                //printf("%d q is full\n", msg.dst);
                this->errors++;
            }
            else{
                this->msg_write[msg.dst]++;
            }
        }
       i++;
       if (i >=  activeWorkQs ) {
            i = 0;
       }
    }
}



