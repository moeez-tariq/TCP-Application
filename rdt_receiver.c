//include all libraries
#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/time.h>
#include <assert.h>
#include <math.h>

#include "common.h"
#include "packet.h"

#define WINDOW_SIZE 32 //define the window size

//the packets that are received and the acknowledgements sent
tcp_packet *recvpkt;
tcp_packet *sndpkt;
tcp_packet* receiveWindow[WINDOW_SIZE];

int ind = 0;
int arrayIndex = 0;

int sockfd; /* socket */
int portno; /* port to listen on */
int clientlen; /* byte size of client's address */
struct sockaddr_in serveraddr; /* server's addr */
struct sockaddr_in clientaddr; /* client addr */
int optval; /* flag value for setsockopt */
FILE *fp; //file pointer
char buffer[MSS_SIZE];
struct timeval tp;

int sequenceNumberNext = 0; //sequence number

void inOrderPacket(); //function that handles in order packets
void lastPacketHandler(); //function that handles the last packet

int main(int argc, char **argv) {

    sndpkt = make_packet(0); //make packet
    sndpkt->hdr.ackno = 0; //number = 0
    sndpkt->hdr.ctr_flags = ACK; //tell that its acknowlegement

    sequenceNumberNext = 0; //expected sequence number for packet tracking
    
    /*
     * check command line arguments
     */
    if (argc != 3) {
        fprintf(stderr, "usage: %s <port> FILE_RECVD\n", argv[0]);
        exit(1);
    }
    portno = atoi(argv[1]);

    fp  = fopen(argv[2], "w");
    if (fp == NULL) {
        error(argv[2]);
    }

    /*
     * socket: create the parent socket
     */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* setsockopt: Handy debugging trick that lets
     * us rerun the server immediately after we kill it;
     * otherwise we have to wait about 20 secs.
     * Eliminates "ERROR on binding: Address already in use" error.
     */
    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR,
            (const void *)&optval , sizeof(int));

    /*
     * build the server's Internet address
     */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    serveraddr.sin_addr.s_addr = htonl(INADDR_ANY);
    serveraddr.sin_port = htons((unsigned short)portno);

    /*
     * bind: associate the parent socket with a port
     */
    if (bind(sockfd, (struct sockaddr *) &serveraddr,
                sizeof(serveraddr)) < 0)
        error("ERROR on binding");

    /*
     * main loop: wait for a datagram, then echo it
     */
    VLOG(DEBUG, "epoch time, bytes received, sequence number");

    int i = 0;
    while (1) //for filling up the array with packets
    {
        if (i >= WINDOW_SIZE) //since window size is 10, so will fill array of 10 elements
        {
            break;
        }
        receiveWindow[i] = make_packet(DATA_SIZE);
        receiveWindow[i]->hdr.ackno = -1; //-1 so we know these indexes are not filled
        i++;
    }

    clientlen = sizeof(clientaddr);

    while (1) 
    {
        //Receiving the packet from the sender
        if (recvfrom(sockfd, buffer, MSS_SIZE, 0,
                (struct sockaddr *) &clientaddr, (socklen_t *)&clientlen) < 0) {
            error("ERROR in recvfrom");
        }
        recvpkt = (tcp_packet *) buffer;

        sndpkt->hdr.seqno = recvpkt->hdr.seqno;


        // sndpkt->retransmitted = recvpkt->retransmitted;
        // sndpkt->start_time = recvpkt->start_time;
        // sndpkt->end_time = recvpkt->end_time;

        ind = recvpkt->hdr.seqno/DATA_SIZE; //find the index of the sequence number. will later modulus with WINDOW_SIZE for wrap around
        ind = (int)(ceil(ind));

        printf("INDEX: %d\n", ind);

        //store the packet in the array at the right index
        receiveWindow[(ind % WINDOW_SIZE)] = (tcp_packet *) buffer;

        printf("PACKET NUM: %d, DATA SIZE: %d\n", recvpkt->hdr.seqno, recvpkt->hdr.data_size);

        assert(get_data_size(recvpkt) <= DATA_SIZE);
        

        if (!(recvpkt->hdr.data_size == 0)) //if the packet size is not 0 means that the packet is not the last packet
        {
            arrayIndex = sequenceNumberNext/DATA_SIZE; //find the index based on the sequence number
            arrayIndex = (int)(ceil(arrayIndex));  //later to be modulus with 10 for wrap around the array

            // When the packet received is in order

            if (recvpkt->hdr.seqno < sequenceNumberNext) //if the packet received has a sequence number lower than the next sequence number means that duplicate packet
            {
                printf("Duplicate Packet %d Discarded.\n", recvpkt->hdr.seqno);

                // discard the duplicate packet and send the previously made sndpkt
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                        (struct sockaddr *) &clientaddr, clientlen) < 0) {
                    error("ERROR in sendto");
                }

                printf("Next Packet Should Be : %d\n",sequenceNumberNext);
            }
            else if (recvpkt->hdr.seqno > sequenceNumberNext) //if the packet number is greater than the expected packet
            {
                //then send the expected packet acknowledgement
                if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0,
                        (struct sockaddr *) &clientaddr, clientlen) < 0) {
                    error("ERROR in sendto");
                }
            }
            else if(recvpkt->hdr.seqno == sequenceNumberNext) //if the packet is in order
            {
                gettimeofday(&tp, NULL);
                VLOG(DEBUG, "%lu, %d, %d", tp.tv_sec, recvpkt->hdr.data_size, recvpkt->hdr.seqno);
                inOrderPacket(); //handle it accordingly
            }
        }
        else //if the packet has a size of 0
        {
            lastPacketHandler(); //then handle it accordingly
            VLOG(INFO, "End Of File");
            break; //break since no more packets since that was last packet
        }
    }

    return 0;
}


void lastPacketHandler() //handles last packet
{
    fclose(fp); //close file since the file transfer is complete

    
    sndpkt = make_packet(0); //make a packet to send acknowledgement 
    sndpkt->hdr.ctr_flags = ACK; 
    int tempSize = 0;
    tempSize = recvpkt->hdr.data_size;
    tempSize = tempSize + recvpkt->hdr.seqno;
    sndpkt->hdr.ackno = tempSize; //acknowlegement of the next packet

    int j = 0;
    while (1)
    {
        if (j >= 500) //send the packet 500 times just in case some packets get dropped
        {
            break; //ensures the last acknowlegement is received on the other end
        }

        if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *) &clientaddr, clientlen) < 0) //sends the packet
        {
            error("ERROR in sendto");
        }
        j++;
    }
}


void inOrderPacket() //handles in order packets
{
    fseek(fp, recvpkt->hdr.seqno, SEEK_SET); //go to the appropriate place in the file

    int indexState = 0; //tells about if the array slot is empty or not (-1 or not)
    int dataSize = 0; //size of the data

    while (1)
    {
        indexState = receiveWindow[(arrayIndex % WINDOW_SIZE)]->hdr.ackno; //if it is not -1, then means that the data is yet to be written to file
        if (indexState == -1) //if it is -1
        {
            break; //then break because then all the previous would have been written
        }
        dataSize = receiveWindow[arrayIndex % WINDOW_SIZE]->hdr.data_size; //calculate the datasize of the data in the current index
        fwrite(receiveWindow[(arrayIndex % WINDOW_SIZE)]->data, 1, dataSize, fp); //then write it in the file
        sequenceNumberNext += dataSize; //increment the sequence number to get the next packet
        receiveWindow[(arrayIndex % WINDOW_SIZE)]->hdr.ackno = -1; //set this to -1 because now we have already written it in the file
        arrayIndex = arrayIndex + 1; //check the next index
    }
    sndpkt = make_packet(0); //after all writeable packets have been written
    sndpkt->hdr.ctr_flags = ACK; //make the packet
    sndpkt->hdr.ackno = sequenceNumberNext; //and assign appropriate sequence number

    // sndpkt->retransmitted = recvpkt->retransmitted;
    // sndpkt->start_time = recvpkt->start_time;
    // sndpkt->end_time = recvpkt->end_time;

    if (sendto(sockfd, sndpkt, TCP_HDR_SIZE, 0, (struct sockaddr *) &clientaddr, clientlen) < 0) //and then the acknowledgement for the next packet
    {
        error("ERROR in sendto");
    }
    printf("Next Packet Should Be : %d\n",sequenceNumberNext);
}

