# actor_sim
Actor pattern simulator

#Brief

Normally an Actor has a single input or event queue that is accesable by all other actors depending on the application graph. 
As the Actors are repicated and the number of event sources grows the contention for the single shared input event queue grows. 
System topology can also affect contention delays due to routing distances, even different path delays.
In this simulator, the share input queue is mediated by a sw switch. Each Actor has a single output queue and the message destination 
address is used by the SW switch instaed of the Actor contending for access to all target queues. All queues are single producer:single consumer. 
Scaling and distance have little affect on sending Actor or consuming Actor delay to access and use its queues. A simple per peer credit system is used to prevent over subscribing any Actor.
A single CPU/thread can switch all of the events for a large applicatio (30+ Threads/Actors).
Event copies and even load balancing can be hidden in the SW switch, futher improving performance.

#how it works

Simulator divides number of requests for completion measurement evenly between IB_read threads. IB_read threads send half the requests directly to the emulator threads (round robin (2)) and 
half via the io_gen threads (8 round robin) to emulator threads. Emulator threads on reciving a request acknowledge it and then sends it on to the IB_write threads. 
It sends roughly half directly to IB_write threads (3) and the rest via IO_gen treads (8) all round robin.
The IB_write threads acknowledge all received messages.
On the last request (message) sent by each IB_read thread they change the request to indicate the last message. When IB_write recives the a last message type it signals the CLI thread.
When the CLI thread has reacieved all three notifications it calaculates the total run time.

#operation

This program requires at least 14 CPU/threads  (CPU can be a Core if SMT disabled). Generally the AG, EM's and CLI are on seperate Cores (even with SMT enable (just asign the first context)) and
IO_GEN, IB_READ and IB_WRITE are on seperate CPU's (SMT enables ok).

the actor simulator commandline options to set CPU affinity, try to avoid Core/cpu 0, leave for linux tasks (other)
-c        cli cpu
-a       message switch Core, recommend that it is co-loacated (same CCD) with emulators
-e       emulator Cores (2), comma seperated list ;   10,11  recommend cores but cpus may be used
-i        io_gen cpus (8), comma seperator list, they can be anywhere; 2,3,4,5,14,15,16,17
-r       ib_read cpus (3), comma seperator list, they can be anywhere; 6,7,8
-w     ib_write cpus (3), comma seperator list, they can be anywhere; 18,19,20

Zen3 workstaion example: actor -c 1 -a 11 -e 9,10 -i 2,3,4,5,14,15,16,17 -r 6,7,8 -w 18,19,20   (default with no arguments)

The simulator stops after running, type "stats" to see counters.
Or type "exit" to quit.

#output - stats

```
all threads ready     //all thraed ready
ib_read_0 START      // send start test to IB_read threads
ib_read_1 START
ib_read_2 START
1.039050                    //completion time for 1M requests
stats                              //stats cli command
RX           ib_read  ib_read  ib_read  ib_write  ib_write  ib_write   io_gen   io_gen   io_gen   io_gen   io_gen   io_gen   io_gen   io_gen       em       em       ag
 0   ib_read:         0        0        0        0        0        0    20787    21075    21002    20787    20740    20785    20762    20729   166667   166667   500001
 1   ib_read:         0        0        0        0        0        0    20761    20890    20986    20942    21001    20592    20598    20896   166666   166667   499999
 2   ib_read:         0        0        0        0        0        0    20686    20994    21135    20695    20759    20773    20811    20813   166666   166667   499999
 3  ib_write:         0        0        0        0        0        0        0        0        0        0        0        0        0        0   187500   187501   375001
 4  ib_write:         0        0        0        0        0        0        0        0        0        0        0        0        0        0   187499   187500   374999
 5  ib_write:         0        0        0        0        0        0        0        0        0        0        0        0        0        0   125000   125000   250000
 6    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
 7    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
 8    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
 9    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
10    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
11    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
12    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
13    io_gen:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
14        em:    166667   166666   166666   187500   187499   125000    23438    23438    23438    23438    23437    23437    23437    23437        0        0  1187498
15        em:    166667   166667   166667   187501   187500   125000    23438    23438    23438    23438    23437    23437    23437    23437        0        0  1187502
Totals     ib_read  1000000     ib_write 1000000      em 1000000  
TX         
 0   ib_read:         0        0        0        0        0        0        0        0        0        0        0        0        0        0   166667   166667   333334
 1   ib_read:         0        0        0        0        0        0        0        0        0        0        0        0        0        0   166666   166667   333333
 2   ib_read:         0        0        0        0        0        0        0        0        0        0        0        0        0        0   166666   166667   333333
 3  ib_write:         0        0        0        0        0        0    15626    15626    15624    15626    15624    15624    15626    15624   125000   125001   375001
 4  ib_write:         0        0        0        0        0        0    15624    15626    15626    15624    15626    15624    15624    15626   124999   125000   374999
 5  ib_write:         0        0        0        0        0        0    15626    15624    15626    15626    15624    15626    15624    15624    62500    62500   250000
 6    io_gen:     20787    20761    20686        0        0        0        0        0        0        0        0        0        0        0    23438    23438   109110
 7    io_gen:     21075    20890    20994        0        0        0        0        0        0        0        0        0        0        0    23438    23438   109835
 8    io_gen:     21002    20986    21135        0        0        0        0        0        0        0        0        0        0        0    23438    23438   109999
 9    io_gen:     20787    20942    20695        0        0        0        0        0        0        0        0        0        0        0    23438    23438   109300
10    io_gen:     20740    21001    20759        0        0        0        0        0        0        0        0        0        0        0    23437    23437   109374
11    io_gen:     20785    20592    20773        0        0        0        0        0        0        0        0        0        0        0    23437    23437   109024
12    io_gen:     20762    20598    20811        0        0        0        0        0        0        0        0        0        0        0    23437    23437   109045
13    io_gen:     20729    20896    20813        0        0        0        0        0        0        0        0        0        0        0    23437    23437   109312
14        em:     83333    83333    83333   187500   187499   125000    31152    31307    31623    31077    31382    31085    31152    31222        0        0   999998
15        em:     83334    83334    83334   187501   187500   125000    31082    31652    31500    31347    31118    31065    31019    31216        0        0  1000002
      errors:         0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0        0
```
