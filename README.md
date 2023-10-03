# actor_sim
Actor pattern simulator

Normally an Actor has a single input or event queue that is accesable by all other actors depending on the application graph. 
As the Actors are repicated and the number of event sources groups the contention for the single shared input event queue groups. 
System topology can also affect contention delays due to routing distances, even different apth delays.
In this simulator the share input queue is mediated by a sw switch. Each Actor has a single output queue and the message destination 
address is used by the SW switch. All queue are single producer:single consumer. 
Scaling and distance have little affect in sending Actor or consuming Actor delay to access and use its queues. A simple per peer credit system is used to prent over subscribing any Actor.
A single CPU/thread can switch all of the events for a large applicatio (30+ Threads/Actors).
Event copies and even load balancing can be hidden in the SW switch, futher improving performance.


