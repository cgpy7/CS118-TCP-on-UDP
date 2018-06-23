/* A server in the internet domain using UDP and implementing rdt
   The port number and hostname are passed as an argument
*/
#include <stdio.h>
#include <sys/types.h>   // definitions of a number of data types used in socket.h and netinet/in.h
#include <sys/socket.h>  // definitions of structures needed for sockets, e.g. sockaddr
#include <netinet/in.h>  // constants and structures needed for internet domain addresses, e.g. sockaddr_in
#include <unistd.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>  /* signal name macros, and the kill() prototype */

#include <chrono>
#include <vector>
#include <list>
#include <dirent.h>
#include <iostream>
#include <string>
#include <cctype>
#include <ctime>
#include <fstream>
#include <algorithm>
#include <queue>
using namespace std;

/* Some Constants */
const string SERVER = "JASON MAO MBP localhost";
const int MAX_SEQ_NUM = 30720;
const int MAX_PACKET_SIZE = 1024;

//may or may not remain constants
const double rto = 500; //in msec
const int WINDOW_SIZE = 5120; // in bytes






int gettime() {
  auto time = chrono::system_clock::now().time_since_epoch();
  int timeval = chrono::duration_cast<chrono::milliseconds>(time).count();

  return timeval;
}

string getDate ()
{
  time_t rawtime;
  struct tm * timeinfo;

  time (&rawtime);
  timeinfo = localtime (&rawtime);
  string time = asctime(timeinfo);
  time.pop_back();
  time += " (UTC)";

  return time;
}
/*
string getDate() {
  time_t tm = time(NULL);
  
  string temp = asctime( localtime(&tm) );
  temp = temp.substr(0, temp.length()-1);
  temp += string(" PST");

  return temp;
}
*/

void error(const char *msg)
{
  perror(msg);
  exit(1);
}



struct file {
  string name;  //file name, an invalid file name will be sanitized to empty string
  string ext;   //file extension

  file() {};
  file(string fname, string fext = "html"): name(fname), ext(fext) { 
    if(name=="") {
      ext = "";
    } 
  };

  string toString() const {return name+(ext!=""?"."+ext:"");}
  operator bool() const { return (name == "")?false:true; }

  bool operator==(const file& other) const {
    return (name == other.name) && (ext == other.ext);
  }

};


file sanitize(string fname) {
  //cout << "SANITIZE " << fname << endl;

  //remove trailing garbage
  while(!isalnum(fname.back())) {
    fname.pop_back();
  }

  //fname should be case insensitive and handle spaces which are represented by %20
  //fname should NOT include the / character, as we are only supporting file in the server main directory
  string f, e;
  for(int i=0; i<fname.length(); i++) {
    char current = tolower(fname[i]);

    if(current == '%') {
      if(fname.length() > i+2 && fname[i+1] == '2' && fname[i+2] == '0') {
	//space found in file name
	f += ' ';
	i += 2;
	continue;
      }
    }
    if(current == '/') {
      throw string("invalid / in file name, only supporting files in main directory at this time");
    }


    if(current == '.') {  //start of file extension
      for(int j = i+1; j< fname.length(); j++) {
	char e_curr = tolower(fname[j]);  //file extensions should not have spaces or special characters, allow only alphanumerics
	if(!isalnum(e_curr)) {
	  throw string("invalid character found in file extension" + e_curr);
	}

	e += e_curr;
      }
      break;
    }

    f += current;
 }
  
  return file(f, e);
}

//takes a file requested by client and a vector of files on server
//returns a string with the matching filename, empty string if none matched
string matchFiles(const file& req, vector<string>& files) {
  //cout << "matching file " << req.toString() << endl;
  for(string fn: files) {
    try{
      file toMatch = sanitize(fn);
      if(toMatch == req)
	return fn;

    }
    catch(string s) {
      cout << "IMPROPPER NAME:" << s << endl;
      continue;
    }
  }
  return "";
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
  int timeout;

  int len() {
    return serverip.length() + clientip.length() + to_string(serverport).length()
      + to_string(clientport).length() + type.length() + to_string(seq).length() 
      + to_string(ack).length() + body.length() + 7;
  }
  
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
    expected_ack = (expected_ack+1) % (MAX_SEQ_NUM);


  }


  //build packet obj from received char buffer
  packet(char* buffer, int str_size, int delay = -1) {
    //parse client packet format
    try {
      int index = 0;
      serverip = extract_line(index, buffer, str_size);
      serverport = stoi(extract_line(index, buffer, str_size));
      clientip = extract_line(index, buffer, str_size);
      clientport = stoi(extract_line(index, buffer, str_size));
      
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
      expected_ack = (expected_ack+1) % (MAX_SEQ_NUM);
    
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
    string temp = 
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

  string toString() {
    string temp = 
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
  
  void set_status(int i) {
    status = i;
    return;
  }
  


};


/**************************/
/*****Print Recv Send******/
/**************************/
void print_recv( packet& in) {
  int acknum = in.seq + in.body.length();
  if(in.type == "SYN" || in.type == "SYNACK" || in.type == "FIN" || in.type == "FINACK") 
    acknum++;
  cout << "Receiving packet " << acknum << endl;

  //cout << "___________________________________________" << endl;
  //cout << in.toString_send() << endl;
  return;
}

void print_send( packet& in, int win = 5120) {
  cout << "Sending packet " << in.seq << " " << win;

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





/**************************/
/*****SERVER MAIN FUNC*****/
/**************************/
int main(int argc, char *argv[])
{
  int state = 0;

  int sockfd, newsockfd, portno;
  string hostip;
  struct sockaddr_in serv_addr, cli_addr;
  socklen_t clilen = sizeof(cli_addr);


  /*
  vector<string> files;
  //get list of all files in current directory
  DIR* dir = opendir(".");
  dirent* entry = readdir(dir);
  while(entry) {
    string temp_name = string(entry->d_name);
    if(temp_name != "." && temp_name != ".." && entry->d_type != DT_DIR)
      files.push_back(temp_name);
    entry = readdir(dir);
  }

  cout << "Files found on server main directory:" << endl;
  for(int i=0; i<files.size(); i++){
  cout << "  "<< files[i] << endl;
  }
  cout << endl;
  */

  if (argc < 3) {
    cerr << "Improper usage:\n  ./Server [hostname] [portno]" << endl;
    cerr << "  [hostname] - ip of host. Will most likely be localhost, 127.0.0.1" << endl;
    cerr << "  [portno] - use a high port number, e.g. 8080" << endl;
    error("Require all parameters");
  }
  else {
    hostip = argv[1];
    portno = atoi(argv[2]);
  }

  sockfd = socket(AF_INET, SOCK_DGRAM, 0);  // create socket
  if (sockfd < 0) {
    error("ERROR opening socket");
  }
  memset((char *) &serv_addr, 0, sizeof(serv_addr));  // reset memory
  serv_addr.sin_family = AF_INET;   // fill in address info
  serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
  serv_addr.sin_port = htons(portno);

  if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
    error("ERROR on binding");

  //set up some things to use select
  struct timeval z; //zero, just do one poll and move one. 
  z.tv_sec = 0;     //for more effeciency later on, can change this
  z.tv_usec = 0;    //to the shorest current timeout val, and wait that long

  fd_set readfds;
  FD_ZERO(&readfds);
  FD_SET(sockfd, &readfds);

  //connection information
  string clientip = ""; //initially, no connection
  int clientport = -1;
  int init_seq = -1; //initial sequence number
  int n; //to store length of incoming msg
  int ack_progress = -1; //value of the highest ack received from client
  int seq_progress = -1; //value of next expected seq from client

  //message queue for requested file  
  queue<packet> msg_q;
  //list for window of files
  list<packet> window;  
  //buffer for incoming msg
  char buffer[2000];
  bzero(buffer, 2000);

  while(1) {
    //use select to see if any new incoming msg
    FD_SET(sockfd, &readfds);
    if(select(sockfd+1, &readfds, NULL, NULL, &z) == -1) cout << "ERROR IN SELECT";

    //debug output
    

    bool arrived = FD_ISSET(sockfd, &readfds);
    packet incoming;

    
    if(arrived) {
      n = recvfrom(sockfd, buffer, 2000, 0, (struct sockaddr*) &cli_addr, &clilen);
      if(n>2000) n = 2000;
      incoming = packet(buffer, n); 
      print_recv(incoming);
    }

 


    //if packet is a FIN, respond to it and then discard it
    if(arrived && incoming.type == "FIN") {
      arrived = false;
      //construct FINACK packet and send it immediately
      packet finack(incoming.clientip, incoming.clientport, hostip, portno, string("FINACK"), 
		    incoming.ack, incoming.seq+1, string(), rto);

      print_send(finack);
      sendto(sockfd, finack.toString_send().c_str(), finack.toString_send().length(), 0, (struct sockaddr*) &cli_addr, clilen);
    }



    switch(state) {
    case 0: //waiting for SYN packet
      if(arrived && incoming.type == "SYN") { //make sure is SYN packet
	//if SYN, set up connection details
	//save client ip and port
	clientip = incoming.clientip;
	clientport = incoming.clientport;
	
	//randomize initial sequence number
	//for convenience, choose somewhere [1, 3000]
	//init_seq = (rand() % 3001);  
	init_seq = 1802;

	//construct ack msg and put in msg_q
	packet synack(clientip, clientport, hostip, portno, string("SYNACK"), init_seq, incoming.seq+1, string(), rto);
	//cout << "SENDING SYNACK\n" << synack.toString_send() << endl;
	//cout << "LENGTH: " << synack.toString_send().length() << endl;
	
	msg_q.push(synack);
	
	state = 1; //move to state 1, syn recvd and awaiting final synack
      } //otherwise discard
      break;
      
      
    case 1: //send synack, waiting for last ack of handshake
      //check if synack with matching ack number.
      if(arrived && incoming.type == "ACK" && incoming.ack == window.front().expected_ack 
	 && incoming.clientip == clientip && incoming.clientport == clientport) {
	//set that msg to acked, move to state 4 expecting file req
	window.front().set_status(2);

	//initiallize ack progress
	ack_progress = incoming.ack;
	seq_progress = incoming.seq;
	state = 5;
	
	//during testing, echo to temp client doesn't complain
	//sendto(sockfd, buffer, strlen(buffer)+1, 0, (struct sockaddr*) &cli_addr, clilen);
      }//otherwise discard
      
      break;
      
    case 2: //finished transmission, send FIN, move to state 3 FINACK
      if(true) {
	packet fin(clientip, clientport, hostip, portno, string("FIN"), ack_progress, seq_progress, string(), rto);
	msg_q.push(fin);
	state = 3;
      }
      break;
      
      
    case 3: //finished sending fin, waiting for FINACK


      if(arrived && incoming.type == "FINACK" && incoming.ack == window.front().expected_ack 
	 && incoming.clientip == clientip && incoming.clientport == clientport) {
	//finack arrived, move to state 0 awaiting connection
	//also close connection
	clientip = "";
	clientport = -1;
	seq_progress = -1;
	ack_progress = -1;
	
	window.clear();
	while(!msg_q.empty())
	  msg_q.pop();
	
	state = 0;
      }//otherwise discard

    case 5: //waiting for file request from client
      //cout << seq_progress <<"    "<< ack_progress << endl;

      if(arrived && incoming.type == "REQ" && incoming.seq == seq_progress && incoming.ack == ack_progress
	 && incoming.clientip == clientip && incoming.clientport == clientport) {

	//cout << "GOT REQ FROM CLIENT" << endl;

	ack_progress = incoming.ack;
	seq_progress = incoming.seq + incoming.body.length();

	vector<string> files;
	//get list of all files in current directory
	DIR* dir = opendir(".");
	dirent* entry = readdir(dir);
	while(entry) {
	  string temp_name = string(entry->d_name);
	  if(temp_name != "." && temp_name != ".." && entry->d_type != DT_DIR)
	    files.push_back(temp_name);
	  entry = readdir(dir);
	}
	
	// cout << "Files found on server main directory:" << endl;
	// for(int i=0; i<files.size(); i++){
	//   cout << "  "<< files[i] << endl;
	// }
	// cout << endl;
	
	string fname;
	//try to find the file requested
	try {
	  fname = matchFiles(sanitize(incoming.body), files);
	}
	catch (string s) {
	  cout << "ERROR: " << s << endl;
	}
	//cout << "FILE REQ:" << incoming.body << endl;
	//cout << "FILE FOUND:" << fname << endl;

	ifstream is(fname);

	if(!is.is_open()) {
	  state = 6;
	  break;
	}

	//if file found, create queue of packets to send
	int seq_counter = ack_progress; //the first packet should have seq num = the most recent ack num
	packet temp_pac(clientip, clientport, hostip, portno, string("RES"), seq_counter, seq_progress, string(), rto);
	char temp_c;
	
	while(is.get(temp_c)) { //for every character
	  temp_pac.body += temp_c; //add it to a packet
	  if(temp_pac.toString_send().length() >= MAX_PACKET_SIZE) {
	    //if the packet is at its size limit
	    //set the expected ack number
	    temp_pac.expected_ack = temp_pac.seq + temp_pac.body.length();
	    //push the packet into the q
	    msg_q.push(temp_pac);
	    
	    //advance seq_counter
	    seq_counter += temp_pac.body.length();
	    seq_counter = seq_counter % MAX_SEQ_NUM;
	    
	    //reset the packet to an empty body
	    temp_pac.body = "";
	    temp_pac.seq = seq_counter;
	  }
	}

	//check if remaining packet is empty or not
	if(temp_pac.body != "") {
	  temp_pac.expected_ack = temp_pac.seq + temp_pac.body.length();
	  msg_q.push(temp_pac);
	}

	//advance to state 6
	state = 6;
      }
      break;
      

    case 6: //file divided into chuncks, ack appropiate packets in window when possible
      if(arrived && incoming.type == "ACK"
	 && incoming.clientip == clientip && incoming.clientport == clientport) {
	//cout << "RECEIVED ACK " << incoming.ack << endl;

	//currently using non-cumulative ack
	//can change by itterating backwards and propogating acks
	bool flag = false;
	for(auto it = window.rbegin(); it != window.rend(); it++) { 
	  //cout << "  expecting ack num:" << msg.expected_ack << endl;
	  if(flag == true || incoming.ack == (*it).expected_ack) {
	    (*it).set_status(2);
	    flag = true;
	  } 
	}

      }

      if(msg_q.empty() && window.empty()) { //if everything is complete, proceed to FIN
	state = 2;
      }
      break;
      
    } //end switch case
    
    //cout << "IN STATE " << state << endl;    
    
    //get rid of msgs in the beginning of the window that are acked
    while(!window.empty() && window.front().status == 2) {
      ack_progress = window.front().expected_ack;
      window.pop_front();
    }


    //fill window if needed
    int counter = 0;
    for(auto& msg: window) {
      counter += msg.toString().length();
    }
    while(!msg_q.empty()) {
      int top_msg_size = msg_q.front().toString().length();
      if(counter+top_msg_size > WINDOW_SIZE)
	break;
      counter += top_msg_size;
      window.push_back(msg_q.front());
      msg_q.pop();
    }

    //send msgs in window if needed
    for(auto& msg:window) {
      //check if unsent or timed out
      if(msg.status == 0 || (msg.status == 1 && msg.timeout <= gettime())) {
  //print sending msg
	print_send(msg);
	//set status to 1
	msg.set_status(1);
	msg.set_timeout(rto);
	//cout << "  expected ack " << msg.expected_ack << endl;

	//send msg
	sendto(sockfd, msg.toString_send().c_str(), msg.toString_send().length(), 0, (struct sockaddr*) &cli_addr, clilen);
      }
    } //finished sending msgs

  }
  
  close(sockfd);

  return 0;
}
