//include libraries
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <signal.h>
#include <sys/time.h>
#include <time.h>
#include <assert.h>
#include <math.h>

#include"packet.h"
#include"common.h"

#define STDIN_FD 0
#define ALPHA 0.125
#define BETA 0.25

#define MAX(a,b) (((a)>(b))?(a):(b))


struct timeval cwnd_changed;
struct timeval time_init;

FILE *fpt; //file pointer for output

int next_seqno = 0;
int send_base = 0;
float congestion_window = 1.0; //congestion window initially set to 1

int control_phase = 1; // initialized to slow
float ssthresh = 64.0; //ssthresh set to a high value of 64

int timer_running = 0; // boolean variable to check if the timer is on or not
int arrayIndex = 0; // variable to keep track of index in the window array
int eof = 0; // variable to keep track of file ended or not
int duplicateACK = 0; //variable to keep count of acks, to resend with 3 duplicate acks

int retransmitted[32]; //checks to see if a packet has been retransmitted or not (for karns algorithm)
struct timeval start_times[32]; //contains the sent times of all the packets sent
struct timeval end_times[32]; //contains all thee received times of the packets received

int lastUnacked = 0; // sequence number of last unacked packet
int num_timeouts = 0; // number of timeouts for last unacked packet (for exponential backoff)

int sockfd, serverlen;
struct sockaddr_in serveraddr;
struct itimerval timer;
tcp_packet *sndpkt;
tcp_packet *recvpkt;
sigset_t sigmask;

//initialize values based on required parameters
unsigned long rto = 3000; //3 seconds or 3000 miliseconds
double estimated_rtt = 0;
double dev_rtt = 0;
double sample_rtt = 0;
double differenceRTT;

void resend_packets(int sig);

void init_timer(int delay, void (*sig_handler)(int))
{  
    signal(SIGALRM, resend_packets);
    timer.it_interval.tv_sec = delay / 1000;   // sets an interval of the timer
    timer.it_interval.tv_usec = (delay % 1000) * 1000;
    timer.it_value.tv_sec = delay / 1000;       // sets an initial value
    timer.it_value.tv_usec = (delay % 1000) * 1000;

    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGALRM);
}


tcp_packet* window[32]; // stores packets that are unacknowledged

float timedifference_msec(struct timeval t0, struct timeval t1)
{
    return fabs((t1.tv_sec - t0.tv_sec) * 1000.0f + (t1.tv_usec - t0.tv_usec) / 1000.0f);
}

int find_index(int base)  //find the index of the packet received
{
  int index = (int)ceil(base / DATA_SIZE);
  index = index % 32; // mod 32 for wrap around

  return index;
}

// function to start the timer
void start_timer()
{
    sigprocmask(SIG_UNBLOCK, &sigmask, NULL);
    setitimer(ITIMER_REAL, &timer, NULL);
}

// function to stop the running timer
void stop_timer()
{
    sigprocmask(SIG_BLOCK, &sigmask, NULL);
}

// function to resend packet on a timeout
void resend_packets(int sig)
{
    if (sig == SIGALRM)
    {
        // resend the packet that has been lost
        VLOG(INFO, "TIMEOUT");
        printf("Send Base: %d, Next Seq No: %d\n", send_base, next_seqno);

        //exponential backoff
        if (num_timeouts >= 1) //if a packet has been retransmitted, then 
        {
          if (rto * 2 <= 240000) //the maximum value of RTO should be 240 seconds
          {
            rto *= 2; //increase the RTO by doubling it
          } else 
          {
            rto = 240000; //the maximum value of RTO
          }
          printf("RTO VALUE: %lu\n", rto);

        }

        init_timer(rto, resend_packets);

        num_timeouts += 1; //increase the number of timeouts if the packet has been retransmitted

        // use send_base to get the index of the packet that has to be resent
        int packetIndex = find_index(send_base);


        //change the value so that we know that the packet has been retransmitted
        retransmitted[packetIndex] = 1;
        gettimeofday(&start_times[packetIndex], NULL);

        gettimeofday(&cwnd_changed, NULL);
        fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);
        
        control_phase = 1; // congestion phase goes to slow start
        ssthresh = MAX(congestion_window/2, 2); //ssthresh goes to half the window
        congestion_window = 1.0; //for slow start window goes to 1

        gettimeofday(&cwnd_changed, NULL);
        fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);

        // sends the packet to the receiver again
        if(sendto(sockfd, window[packetIndex], TCP_HDR_SIZE + get_data_size(window[packetIndex]), 0,
                    ( const struct sockaddr *)&serveraddr, serverlen) < 0)
        {
            error("sendto");
        }


        VLOG(DEBUG, "Sending packet %d to %s", send_base, inet_ntoa(serveraddr.sin_addr));

        // stop old timer, reset the timer and set timer_running to 1
        stop_timer();
        start_timer();
        timer_running = 1;
    }
}

/*
 * init_timer: Initialize timer
 * delay: delay in milliseconds
 * sig_handler: signal handler function for re-sending unACKed packets
 */

int main (int argc, char **argv)
{

    gettimeofday(&time_init, NULL);

    fpt = fopen("CWND.csv", "w+"); //opens file

    if (fpt == NULL) {
      printf("ERROR IN FILE OPENING"); //if file does not open, give error
      return -1;
    }

    gettimeofday(&cwnd_changed, NULL);
    fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);

    for (int i = 0; i < 32; i++) //initialize the arrays
    {
      retransmitted[i] = 0;
      start_times[i] = (struct timeval) {0};
      end_times[i] = (struct timeval) {0};
    }
    
    int portno, len;
    char *hostname;
    char buffer[DATA_SIZE];
    FILE *fp;

    /* check command line arguments */
    if (argc != 4) {
        fprintf(stderr,"usage: %s <hostname> <port> <FILE>\n", argv[0]);
        exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);
    fp = fopen(argv[3], "r");
    if (fp == NULL) {
        error(argv[3]);
    }

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");


    /* initialize server details */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serverlen = sizeof(serveraddr);

    /* covert host into network byte order */
    if (inet_aton(hostname, &serveraddr.sin_addr) == 0) {
        fprintf(stderr,"ERROR, invalid host %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_port = htons(portno);

    assert(MSS_SIZE - TCP_HDR_SIZE > 0);

    init_timer(rto, resend_packets);
    next_seqno = 0;
    send_base = 0;

    while (1)
    {
        while (next_seqno < send_base + (int) congestion_window && eof == 0){ // run a loop until all packets in window have been sent
          
          len = fread(buffer, 1, DATA_SIZE, fp); // read the data to be transmitted from the file
          
          if ( len <= 0) // if there are no more packets to be sent
          {
              VLOG(INFO, "End Of File Has Been Reached");
              eof = 1; // set eof to 1
              break;
          }
          else // if there are more packets to be sent
          {
            // calculate the index within 10 of the packet to be sent
            arrayIndex = find_index(next_seqno);
            
            window[arrayIndex] = make_packet(len); // make a packet of the size of the data that is read from the file
            memcpy(window[arrayIndex]->data, buffer, len); // copy the buffer into the 'data' of the packet
            window[arrayIndex]->hdr.seqno = next_seqno; // set the index of the packet appropriately

            retransmitted[arrayIndex] = 0; //since it is a new packet, the retransmitted goes to 0
            gettimeofday(&start_times[arrayIndex], NULL);

            printf("Packet number: %d \n", window[arrayIndex]->hdr.seqno);

            VLOG(DEBUG, "Sending packet %d to %s",
                    next_seqno, inet_ntoa(serveraddr.sin_addr));

            /*
             * If the sendto is called for the first time, the system will
             * will assign a random port number so that server can send its
             * response to the src port.
             */
            if(sendto(sockfd, window[arrayIndex], TCP_HDR_SIZE + get_data_size(window[arrayIndex]), 0,
                        ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }

            // start the timer if it is not already running
            if (timer_running==0){
              start_timer();
              timer_running = 1;
            }

            // increment the next sequence number by the size of the data read from the file
            next_seqno = next_seqno + len;
          }
        }

        // when a packet received
        if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
        {
            error("recvfrom");
        }

        // store the packet
        recvpkt = (tcp_packet *)buffer;

        int ackIndex;

        // sent 1, received ack 2
        // find index of 1 because finding end time of 1
        if (recvpkt->hdr.ackno == lastUnacked + DATA_SIZE) {
          ackIndex = find_index(lastUnacked);
        }

        // last unacked = 2
        // ack = 5
        // means 3 reached, 4 reached, 2 just reached after retransmission and we now want 5
        // subcondition: if retransmitted, don't count rtt, else count
        else if (recvpkt->hdr.ackno > lastUnacked + DATA_SIZE) {
          ackIndex = find_index(recvpkt->hdr.seqno);
        }

        // seqno 5 while ackno 2
        // find index of 5 (seqno)
        else if (recvpkt->hdr.ackno < recvpkt->hdr.seqno) 
        {
          ackIndex = find_index(recvpkt->hdr.seqno);
        }

        if (recvpkt->hdr.ackno >= lastUnacked + DATA_SIZE) 
        {
          num_timeouts = 0;
          lastUnacked = recvpkt->hdr.ackno;
        }

        if (retransmitted[ackIndex] == 0) //if the packet is not retransmitted (karns algorithm)
        {

          gettimeofday(&end_times[ackIndex], NULL);

          //do the calculations for estimated rtt, dev rtt etc and finally rto
          estimated_rtt = estimated_rtt * (1 - ALPHA);

          sample_rtt = ((double) end_times[ackIndex].tv_sec - (double) start_times[ackIndex].tv_sec)*1000;
          sample_rtt += ((double) end_times[ackIndex].tv_usec - (double) start_times[ackIndex].tv_usec)/1000;

          estimated_rtt = estimated_rtt + (sample_rtt * ALPHA);

          differenceRTT = sample_rtt - estimated_rtt;
          if(differenceRTT < 0)
          {
            differenceRTT = estimated_rtt - sample_rtt;
          }

          dev_rtt = (1 - BETA) * dev_rtt;
          dev_rtt = dev_rtt + (differenceRTT * BETA);
          rto = 4 * dev_rtt;
          rto = rto + estimated_rtt;

          if (rto < 1) //use the manual ceiling function since rto is an unsigned long
          {
            rto = 1;
          }

          printf("RTO VALUE: %lu\n", rto);

          init_timer((int)rto, resend_packets);

        }


        // slow start phase
        if(control_phase == 1)
        {
          printf("Slow Start Phase:\n");
          if(congestion_window >= ssthresh) //if the window goes above the ssthresh
          {
            control_phase = 2; //go to congestion avoidance phase
          }

          congestion_window += 1.0; //congestion window increases by 1 in slow start
          gettimeofday(&cwnd_changed, NULL);
          fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);
        }
        //congestion avoidance phase
        else if(control_phase == 2)
        {
          printf("Congestion Avoidance Phase: \n");
          congestion_window+=1.0/congestion_window; //increases by 1/cwnd in this phase
          gettimeofday(&cwnd_changed, NULL);
          fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);
        }

        // restart the timer because an ACK has been received (mentioned in instructions)
        stop_timer();
        start_timer();
        timer_running = 1;

        assert(get_data_size(recvpkt) <= DATA_SIZE);

        if(send_base == recvpkt->hdr.ackno)
        {
          // if duplicate ack but not received 3 times, increase ack count
          if(duplicateACK < 2)
          {
            duplicateACK++;
          }
          else // else if duplicate ack received 3 times
          {

            gettimeofday(&cwnd_changed, NULL);
            fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);

            control_phase = 1;
            // reset ssthresh to half the window
            ssthresh = MAX(congestion_window/2,2);
            // set window size to 1
            congestion_window = 1.0; //set the congestion window to 1
            gettimeofday(&cwnd_changed, NULL);
            fprintf(fpt, "%.6f,%.2f,%d\n", timedifference_msec(time_init, cwnd_changed), congestion_window, (int) ssthresh);

            // set duplicate ack count back to 0
            duplicateACK = 0;

            // get index of the packet that has received a duplicate ack thrice using send base
            int packetIndex = find_index(send_base);

            retransmitted[packetIndex] = 1; //change the retransmitted status to 1
            gettimeofday(&start_times[packetIndex], NULL);

            // resend packet
            if(sendto(sockfd, window[packetIndex], TCP_HDR_SIZE + get_data_size(window[packetIndex]), 0,
                ( const struct sockaddr *)&serveraddr, serverlen) < 0)
            {
                error("sendto");
            }
            stop_timer();
            start_timer();
            timer_running = 1;
          }
        }

        // increase send_base if ack received is the expected ack
        else
        {
          send_base = recvpkt->hdr.ackno;
          duplicateACK = 0;
        }

        printf("Send Base: %d, Next Seq No: %d \n", send_base, next_seqno);

        // if the file has ended, and all packets in the window have been sent
        if (eof == 1 && next_seqno == send_base)
        {
          // get the index where to store the packet in the array using next_seq no
          arrayIndex = find_index(next_seqno);
          printf("LAST PACKET: %d\n", arrayIndex);

          // make a packet with 0 size and send that as a signal to the receiver to end the transmission
          window[arrayIndex] = make_packet(0);
          window[arrayIndex]->hdr.seqno = next_seqno;
          sendto(sockfd, window[arrayIndex], TCP_HDR_SIZE,  0,
                (const struct sockaddr *)&serveraddr, serverlen);

          // the receiver sends back an acknowledgement (0) before it shuts down
          // finally, ensure that the final acknowledgement is received in case it gets missed
          if(recvfrom(sockfd, buffer, MSS_SIZE, 0,
                    (struct sockaddr *) &serveraddr, (socklen_t *)&serverlen) < 0)
          {
              error("recvfrom");
          }
          break;
        }
    
    }

    return 0;

}