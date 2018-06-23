#include <iostream>
#include <stdio.h>
#include <sys/types.h>   // definitions of a number of data types used in socket.h and netinet/in.h
#include <sys/socket.h>  // definitions of structures needed for sockets, e.g. sockaddr
#include <netinet/in.h>  // constants and structures needed for internet domain addresses, e.g. sockaddr_in
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>  /* signal name macros, and the kill() prototype */

#include <netdb.h>      // define structures like hostent

#include <chrono>
#include <vector>
#include <dirent.h>
#include <iostream>
#include <string>
#include <cctype>
#include <ctime>
#include <fstream>
#include <algorithm>
#include <unordered_map>
#include<deque>
#include <fstream>
using namespace std;

const string RECEIVEDFILENAME = "received.data";
//const string RECEIVEDFILENAME = "received.txt";
const int MAX_SEQ_NUM = 30720;
const string localClientIP = "127.0.0.1";
const int rto = 500; //in msec
const int TIME_WAIT = 2 * rto;// FIN/FINACK wait mechanism


int gettime() {
  auto time = chrono::system_clock::now().time_since_epoch();
  int timeval = chrono::duration_cast<chrono::milliseconds>(time).count();

  return timeval;
}

void error(char *msg)
{
    perror(msg);
    exit(0);
}


string extract_line(int& start, char* buffer, int buf_size) {
  string temp;
  for(int i=start; i<buf_size; i++) {
    if(buffer[i] == '\n') {
      start = i+1;
      return temp;
    }
    temp.push_back(buffer[i]);
  }
  start = buf_size;
  return temp;
}

//packet struct
struct packet{
  string serverip, clientip;
  int serverport, clientport;
  string type;
  int seq, ack;
  string body;

  int status = 0; //0 = unsent, 1 = send+unacked, 2 = acked
  int expected_ack;
  double timeout;

  packet() {}
  
  packet(string cip, int cport, string sip, int sport, string t, int s, int a, string b, int d):
    serverip(sip), clientip(cip), serverport(sport), clientport(cport), type(t), seq(s), ack(a), body(b) {
    if(d == -1) {
      timeout = 999999999; //infinite timeout
    } else {
      timeout = gettime() + d; 
    }
    
    expected_ack = (seq + body.length()) % MAX_SEQ_NUM;
    if(type == "SYN" || type == "FIN" || type == "SYNACK" || type == "FINACK")
      expected_ack++;
  }


  //build packet obj from received char buffer
  packet(char* buffer, int str_size, int delay = -1) {
    //parse client packet format
    try {
      int index = 0;
      clientip = extract_line(index, buffer, str_size);
      clientport = stoi(extract_line(index, buffer, str_size));
      serverip = extract_line(index, buffer, str_size);
      serverport = stoi(extract_line(index, buffer, str_size));
      
      type = extract_line(index, buffer, str_size);
      seq = stoi(extract_line(index, buffer, str_size));
      ack = stoi(extract_line(index, buffer, str_size));
      int temp_str_size = str_size - index;

      body = "";
      index += 1;

      while(index < str_size) {
	body.push_back(buffer[index]);
	index++;
      }

    }
    catch(invalid_argument e) {
      serverip = clientip = type = body ="";
      serverport = clientport = seq = ack = 0;
    }
    if(delay == -1) {
      timeout = 999999999; //infinite timeout
    } else {
      timeout = gettime() + delay; 
    }

    expected_ack = (seq+body.length()) % MAX_SEQ_NUM;
    if(type == "SYN" || type == "FIN" || type == "SYNACK" || type == "FINACK")
      expected_ack++;
    
  }

  void set_timeout(int delay) {
    if(delay == -1) {
      timeout = 999999999; //infinite timeout
    } else {
      timeout = gettime() + delay; 
    }
    return;
  }


  string toString_send() {
    string temp =  //flipped
      serverip + '\n' +
      to_string(serverport) + '\n' +
      clientip + '\n' +
      to_string(clientport) + '\n' +
      type + '\n' +
      to_string(seq) + '\n' +
      to_string(ack);

    if(body!= "")
      temp += "\n\n"+body;

    return temp;
  }

  string toString() {
    string temp = //flipped
      clientip + '\n' +
      to_string(clientport) + '\n' +
      serverip + '\n' +
      to_string(serverport) + '\n' +
      type + '\n' +
      to_string(seq) + '\n' +
      to_string(ack);

    if(body!= "")
      temp += "\n\n"+body;

    return temp;
  }
  
  void set_status(int i) {
    status = i;
    return;
  }
  
};

void print_recv( packet& in) // match spec format
{ 
  cout << "Receiving packet " << in.ack << endl;
  //cout << "___________________________________________" << endl;
  //cout << in.toString_send() << endl;
  return;
}

void print_send( packet & in, int win = 5120)
{
  cout << "Sending packet " << in.seq;
  if(in.status == 1)
    cout << " " << "Retransmission";

  if(in.type == "SYN" || in.type == "SYNACK")
    cout << " " << "SYN";
  else if(in.type == "FIN" || in.type == "FINACK")
    cout << " " << "FIN";

  cout << endl;
  //cout << "___________________________________________" << endl;
  //cout << in.toString_send() << endl;
  return;
}



// command line format: ./client [hostname] [port] [filename]
int main(int argc, char *argv[])
{
    if (argc < 3) {
       cerr << "Insufficient command line arguments! \n";
       exit(0);
    }

    int state = 0;

    int sockfd;  // socket descriptor
    int portno, n;
    int LOCALCLIENTPORT;
 
    string serverip = argv[1];
    struct sockaddr_in serv_addr;
    socklen_t servlen = sizeof(serv_addr);
    struct hostent *server;  // contains tons of information, including the server's IP address

    int init_seq = 100;
    int init_ack = 0;
    int ack_progress = -1; //value of the highest ack received from client (cummulative ack)
    int seq_progress = -1; //value of next expected seq from client
    
    char buffer[2000];

    sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // create a new socket, UDP
    if (sockfd < 0)
        error("ERROR opening socket");
    portno = atoi(argv[2]);
    string filename = argv[3];

    //  ./client [hostname] [port] [filename]
    server = gethostbyname(argv[1]);
    if (server == NULL) {
      cout << "?" << endl;
      fprintf(stderr, "ERROR, no such host\n");
      exit(0);
    }
    
        
    //get local port number
    struct sockaddr_in sin;
    socklen_t addrlen = sizeof(sin);
    if(getsockname(sockfd, (struct sockaddr *)&sin, &addrlen) == 0 &&
       sin.sin_family == AF_INET &&
       addrlen == sizeof(sin))
      {
        LOCALCLIENTPORT = ntohs(sin.sin_port);
      }
    
        
    memset((char *) &serv_addr, 0, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET; //initialize server's address
    bcopy((char *)server->h_addr, (char *) &serv_addr.sin_addr.s_addr, server->h_length);
    serv_addr.sin_port = htons(portno);
    

    //set up some things to use select
    struct timeval z; //zero, just do one poll and move one. 
    z.tv_sec = 0;     //for more effeciency later on, can change this
    z.tv_usec = 0;    //to the shorest current timeout val, and wait that long

    fd_set readfds;
    FD_ZERO(&readfds);
    FD_SET(sockfd, &readfds);

    // connection successful if program gets to here.
    //cout << "connection successful" << endl;


    //establish file to accept packets

    ofstream myfile(RECEIVEDFILENAME);
    

    /* test connection */
    /*
    strcpy(buffer, "hello world");
    n = sendto(sockfd, buffer, strlen(buffer), 0, (struct sockaddr*) &serv_addr, servlen);
    */

    deque<packet> msg_q;  
    while(1)
    {
     // select a new message?
		
      //use select to see if any new incoming msg
      FD_SET(sockfd, &readfds); // add socket to a set
      if(select(sockfd+1, &readfds, NULL, NULL, &z) == -1) 
	      cout << "ERROR IN SELECT";

      //debug output
      

      bool arrived = FD_ISSET(sockfd, &readfds);
      packet incoming;

      if(arrived) {
        n = recvfrom(sockfd, buffer, 2000, 0, (struct sockaddr*) &serv_addr, &servlen);
        incoming = packet(buffer, n); 

        print_recv(incoming);

	      //cout << "RECEIVING PACKET:" << endl;
        //cout << incoming.toString() << endl;

        //int temp;
        //cin >> temp;
      }

      //if packet is a FIN, respond to it and then go to state 4
      if(arrived && incoming.type == "FIN") {
        arrived = false; // why?
        //construct FINACK packet and send it immediately
        packet finack(incoming.clientip, incoming.clientport, serverip, portno, string("FINACK"), 
          (incoming.ack)%MAX_SEQ_NUM, incoming.seq+1, string(), rto);
        ack_progress = incoming.ack;
        seq_progress = (incoming.seq+1)%MAX_SEQ_NUM;
        sendto(sockfd, finack.toString_send().c_str(), finack.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
        print_send(finack);
        state = 4;
      }

	//cout << "here" << "case is " << state << endl;

  
      //begin three way handshake:
      /*
            Host A sends a TCP SYNchronize packet to Host B

            Host B receives A's SYN

            Host B sends a SYNchronize-ACKnowledgement

            Host A receives B's SYN-ACK

            Host A sends ACKnowledge

            Host B receives ACK. 
            TCP socket connection is ESTABLISHED.
            
      */
      
      switch(state)
      {
        case 0: //make SYN packet for the first handshake
        {

            //packet(string cip, int cport, string sip, int sport, string t, int s, int a, string b, double d):
            packet syn(localClientIP, LOCALCLIENTPORT, serverip, portno, string("SYN"), init_seq, init_ack, string(), rto);

            msg_q.push_back(syn);
            //sendto(sockfd, syn.toString_send().c_str(), syn.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
            //print_send(syn);
            state = 1;
		
        } 
            break;
        case 1: // SYN sent, waiting for SYNACK
        {
	  /*
            cout << "case 1:" << endl;
            cout << "incoming.clientip = " << incoming.clientip << endl;
            cout << "localClientIP = " << localClientIP << endl;
            cout << "incoming.clientport = " << incoming.clientport << endl;
            cout << "LOCALCLIENTPORT = " << LOCALCLIENTPORT << endl; 
            cout << "serverip = " << incoming.serverip  << endl;
            cout << "incoming.serverport =" << incoming.serverport << endl;
            cout << "portno = " << portno << endl;
            cout << "incoming.ack = " << incoming.ack << endl;
            cout << "init_seq+1 = " << init_seq+1 << endl;
            //int temp;
            //cin >> temp;
	    */

            if(arrived && incoming.type == "SYNACK" && incoming.clientip == localClientIP 
               && incoming.clientport == LOCALCLIENTPORT && incoming.serverip == serverip && incoming.serverport == portno && incoming.ack == init_seq+1) // WINDOW?
            {
              msg_q.front().set_status(2);
              //send the ACK packet (third handshake)
              ack_progress = (incoming.ack)%MAX_SEQ_NUM; //init_seq+1 //101
              seq_progress = incoming.expected_ack; // incoming.seq+1  //1803

              //int temp;
              //cin >> temp;
              packet ack(localClientIP, LOCALCLIENTPORT, serverip, portno, string("ACK"), ack_progress, seq_progress, string(), rto);
              
              
              sendto(sockfd, ack.toString_send().c_str(), ack.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
              print_send(ack);
              state = 2;
            }
        }
            break; // THis break is not needed
        
        case 2: // Last Handshake ACK is sent, sending file request for the first time, ack_num is the same as the third handshake
        {
            packet request(localClientIP,LOCALCLIENTPORT, serverip, portno, string("REQ"), ack_progress, seq_progress, filename, rto);
            msg_q.push_back(request);
            //sendto(sockfd, request.toString_send().c_str(), request.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
            //print_send(request);
            //cout << "below is the request message:" << endl;
            //cout << request.toString_send().c_str() << endl;
            seq_progress = incoming.expected_ack;
            ack_progress = incoming.ack;
            state = 3;
        }
            break;
        case 3: // getting file pieces from the server
        {
            //cout << "here" << endl;
            // cout << "incoming.type = " << incoming.type << endl;
            // cout << "ack_progress = " << ack_progress << endl;
            // cout << "incoming.ack = " << incoming.ack << endl;
            // cout << "seq_progress = " << seq_progress << endl;
            // cout << "incoming.seq = " << incoming.seq << endl;
            
            //if receive a SYNACK again, it means ACK/REQ is lost
            if(arrived && incoming.type == "SYNACK" && incoming.clientip == localClientIP 
            && incoming.clientport == LOCALCLIENTPORT && incoming.serverip == serverip && incoming.serverport == portno && incoming.ack == init_seq+1)
            {
              //state = 1;
              packet ack(localClientIP, LOCALCLIENTPORT, serverip, portno, string("ACK"), ack_progress, seq_progress, string(), rto);
              
              
              sendto(sockfd, ack.toString_send().c_str(), ack.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
              print_send(ack);
              break;
            }            
            //cout << "out side here" << endl;
            
            if(arrived && incoming.type == "RES"  && incoming.clientip == localClientIP && incoming.clientport == LOCALCLIENTPORT && incoming.serverip == serverip && incoming.serverport == portno) // if not ordered??
            {
            //  cout << " inside RES here" << endl;
              if(!msg_q.empty())
              {
                msg_q.front().set_status(2);
              }
              
   
              if(incoming.seq == seq_progress)
              {
                //write into the file
                
                for(auto c: incoming.body)
                {
                  myfile << c;
                }

                //cout << "CAN I SEE HERE PLEASE" << endl;
              
                ack_progress = (incoming.ack) % MAX_SEQ_NUM;
                seq_progress = incoming.expected_ack;
                //return an ack to the sender
                // stall sending ack
                // cout << "stall..." << endl;
                // int temp;
                // cin >> temp;
                // cout << "sending" << endl;
                packet ack(localClientIP,LOCALCLIENTPORT, serverip, portno, string("ACK"), ack_progress%MAX_SEQ_NUM, seq_progress%MAX_SEQ_NUM, filename, rto);
                sendto(sockfd, ack.toString_send().c_str(), ack.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
                print_send(ack);
              }
              else
              {

                packet ack(localClientIP,LOCALCLIENTPORT, serverip, portno, string("ACK"), ack_progress%MAX_SEQ_NUM, seq_progress%MAX_SEQ_NUM, filename, rto);
                sendto(sockfd, ack.toString_send().c_str(), ack.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
                print_send(ack);

              }
            }
        }
            break;

        case 4: //after receiving fin from the server, send a fin request to the server
        {
            packet fin(localClientIP,LOCALCLIENTPORT, serverip, portno, string("FIN"), (ack_progress+1)%MAX_SEQ_NUM, (seq_progress+1)%MAX_SEQ_NUM, string(), rto);
            msg_q.push_back(fin);
            //sendto(sockfd, fin.toString_send().c_str(), fin.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
            //print_send(fin);
            state = 5;
        }
            break;

        case 5: // after sending FIN, wait for FINACK from the server 
        {
            if(arrived && incoming.type == "FINACK" && incoming.clientip == localClientIP && incoming.clientport == LOCALCLIENTPORT && incoming.serverip == serverip && incoming.serverport == portno)
            {
              msg_q.front().set_status(2);
              state = 7;
            }
        }
            break;



        case 7: //TIME-WAIT mechanism 
        {
            double currentTime = gettime();
            while(gettime() - currentTime < double(TIME_WAIT))
            {
              //do nothing
            }
            state = -1; //breaking the while loop then close connection
        }
            break;
      }// end with switch

      /*
      cout <<"IN STATE" << state << endl;
      int temp;
      cin >> temp;
      */
      //check if ack'ed
      while(!msg_q.empty() && msg_q.front().status == 2) {
        ack_progress = msg_q.front().expected_ack;
        msg_q.pop_front();
      }


      if(state == -1)
        break;

      for(auto& msg:msg_q) {
        //check if unsent or timed out
        if(msg.status == 0 || (msg.status == 1 && msg.timeout <= gettime())) {
          //cout << "printing send below:" << endl;
          //print sending msg
          print_send(msg);
          //set status to 1
          msg.set_status(1);
          msg.set_timeout(rto);
          //cout << "  expected ack " << msg.expected_ack << endl;
  
          //send msg
           sendto(sockfd, msg.toString_send().c_str(), msg.toString_send().length(), 0, (struct sockaddr*) &serv_addr, servlen);
          }
      } //finished sending msgs


    }// end with while loop


    close(sockfd);
    return 0;
}

// TODO: 
//       Figure out window(do I need a window? I think not) and sequence number? expected_ack????
// jason needs a break after case 3;
// issues: server sends a fin before client does, different from the book/power point. how do I handle this case?
