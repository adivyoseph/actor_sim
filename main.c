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
        directory.srcIdCnt++;
        directory.enrtyCnt++;
        rtc = 0;
    }
    return rtc;
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
#define CMD_CTL_  STOP    4
#define CMD_CTL_CLEAR    5
#define CMD_CTL_DONE    6
#define CMD_REQ                  8
#define CMD_REQ_ACK        9

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
        sprintf(g_contexts[i_contextNext].name, "emultor");
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

    while (1) {
    }
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
void *th_ib_read(void *p_arg){
    actorThreadContext_t *this = (actorThreadContext_t*) p_arg;
    //cpu_set_t           my_set;        /* Define your cpu_set bit mask. */
     msg_t                  msg;
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
     printf("%s%d init now\n", this->name, this->instance);


    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){
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

     printf("%s%d init now\n", this->name, this->instance);

    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){
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

     printf("%s%d init now\n", this->name, this->instance);

    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){
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

     printf("%s%d init now\n", this->name, this->instance);

    msg.cmd = CMD_CTL_READY;
    msg.src = this->srcId;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", this->srcId);
    }

    while (1){
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

     printf("%s%d init now\n", this->name, this->instance);

    msg.cmd = CMD_CTL_READY;
    msg.src = ACTORS_MAX;
    msg.length = 0;
    if(workq_write(&g_workq_cli, &msg)){
        printf("%d q is full\n", ACTORS_MAX);
    }

    while (1){
    }
}



