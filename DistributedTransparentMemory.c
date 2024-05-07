#include <arpa/inet.h>
#include <errno.h>
#include <limits.h>
#include <memory.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <stdbool.h>
#include <unistd.h>

/*
 * Distributed Transparent Memory Project
 *
 * Note that this project is written just for learning purpose.
 *
 * Simulate a ring topology by running 6 nodes process at the same time on same machine.
 * Each process will listen on IP 127.0.0.1 and bind to UDP and TCP port numbers.
 * Node i knows the node j's ip address and UDP port no, where j = i + 1.
 * Originator is a node on which user has issued PUT/GET request, while target node
 * is the one which is responsible for storing K and X pair in its local hash, based
 * on the value of the find_node macro.
 *
 * User can perform two operations on each node:
 *
 * (1) PUT K X where K and X are two integers, K = key, X = value
 *
 * If the originator node is the target node, then it stores the K and X pair in its local hash table.
 * Otherwise, the originator transfers the UDP message 'PUT_FORWARD' to a successor node. Other nodes
 * repeat this process until it reaches the target node who should store the pair. When the target
 * node receives UDP message and clarifies the value should be processed by the node then
 * it establishes TCP connection with the originator of PUT request by 'WHAT_X' message in order to
 * ask the X value. The originator sends X value directly as TCP reply 'PUT_REPLY_X'.
 * Then the target node stores the pair in the local hash table and closes the TCP connection.
 *
 * (2) GET K where K is an integer, returns X for K
 *
 * If the originator node knows the corresponding value for K in its local hash entry, then just
 * return the stored X value to the user. Otherwise, send UDP msg 'GET_FORWARD' until the sequence
 * of UDP messages arrive at the target node. The arrival of the message makes the target node
 * establish TCP connection with the originator of GET request by 'GET_REPLY_X' message and
 * the target node shows the fetched X value to the user.
 *
 * An example of DTM configuration setup is described below.
 *
 * From left to right, column means
 * a. own node IP,
 * b. own node UDP port number,
 * c. successor node UDP port number and
 * d. own node TCP port number.
 *
 * | --------- | ----- | ----- | ----- |
 * | 127.0.0.1 | 60000 | 60001 | 50000 |
 * | 127.0.0.1 | 60001 | 60002 | 50001 |
 * | 127.0.0.1 | 60002 | 60003 | 50002 |
 * | 127.0.0.1 | 60003 | 60004 | 50003 |
 * | 127.0.0.1 | 60005 | 60006 | 50004 |
 * | 127.0.0.1 | 60006 | 60000 | 50005 |
 * |-----------------------------------|
 *
 * Note that the last node points to the first node
 * UDP port number.
 */
typedef enum MsgID {
    /* UDP */
    PUT_FORWARD = 1,
    GET_FORWARD,
    /* TCP */
    WHAT_X,
    PUT_REPLY_X,
    GET_REPLY_X
} MsgID;

typedef enum UserChoice {
    PUT = 1,
    GET = 2
} UserChoice;

typedef struct ApplyMsg {
    unsigned int msg_id;
    unsigned int k;
    unsigned int x;

    /* Originator info */
    unsigned int address; /* IP address */
    /*
     * Note : unsigned int address can be encoded as
     * unsigned integer equivalent of IP address.
     */
    unsigned int tcp_port;
} ApplyMsg;

static char*
dtm_get_apply_string(MsgID val){
    switch(val){
	case PUT_FORWARD:
	    return "PUT_FORWARD";
	case GET_FORWARD:
	    return "GET_FORWARD";
	case WHAT_X:
	    return "WHAT_X";
	case PUT_REPLY_X:
	    return "PUT_REPLY_X";
	case GET_REPLY_X:
	    return "GET_REPLY_X";
	default:
	    fprintf(stderr, "invalid MsgID has been detected\n");
	    exit(-1);
	    break;
    }
}

static void
dtm_set_apply_msg(ApplyMsg* msg, unsigned int msg_id,
		  unsigned int k, unsigned int x,
		  unsigned int address, unsigned int tcp_port){
    msg->msg_id = msg_id;
    msg->k = k;
    msg->x = x;
    msg->address = address;
    msg->tcp_port = tcp_port;
}

typedef struct HashTblEntry {
    unsigned int key;
    unsigned int value;
} HashTblEntry;

/*
 * Each node is a TCP server/UDP server and TCP client/UCP client
 * at the same time. Therefore, we should open TCP master socket
 * and monitor both in select system call.
 */
static int monitored_fd_set[32];

/* Node management */
#define MAX_NODE_NUM 6
#define find_node(key) (key % MAX_NODE_NUM + 1)
static int my_node_id = -1;
static in_addr_t own_node_ip;
/* Put below three variables into network byte order */
static int own_node_udp_port;
static int next_node_udp_port;
static int own_node_tcp_port;

/* Fixed-length Hash Table variables */
#define HASH_SIZE 10
static HashTblEntry my_hash[HASH_SIZE];
static int my_hash_index = 0;

/*
 * When user inputs one new pair of data, the originator node
 * keeps the 'x' value until the target node stores the value.
 *
 * Both to get the user input and to send the reply for 'WHAT_X' message
 * are conducted in different iterations of select system call.
 * Then, this value should be global.
 */
static int what_x_rep = -1;

#define BUF_SIZE 256

static int
dtm_search_from_hash(int key){
    int i;

    for (i = 0; i < HASH_SIZE; i++){
	if (my_hash[i].key == key){
	    printf("Got the value '%d' from hash search\n",
		   my_hash[i].value);
	    return my_hash[i].value;
	}
    }

    return -1;
}

static void
dtm_store_hash_entry(int key, int value){
    if (my_hash_index < HASH_SIZE){
	my_hash[my_hash_index].key = key;
	my_hash[my_hash_index++].value = value;
	printf("Stored the pair key = '%d' and value = '%d'\n",
	       key, value);
    }else{
	fprintf(stderr,
		"The hash table has been filled up already."
		"Can't add any new entry\n");
    }
}

static void
dtm_initialize_monitor_fd_set(){
    int i = 0;

    for (; i < MAX_NODE_NUM; i++)
	monitored_fd_set[i] = -1;
}

static void
dtm_add_to_monitored_fd_set(int skt_fd){
    int i = 0;

    for (; i < MAX_NODE_NUM; i++){
	if (monitored_fd_set[i] != -1)
	    continue;
	monitored_fd_set[i] = skt_fd;
	break;
    }
}

/* memo */
/*
static void
dtm_remove_from_monitored_fd_set(int skt_fd){
    int i = 0;

    for(; i < MAX_NODE_NUM; i++){
	if (monitored_fd_set[i] != skt_fd)
	    continue;
	monitored_fd_set[i] = -1;
	break;
    }
}
*/

static void
dtm_re_init_readfds(fd_set *fd_set_ptr){
    int i = 0;

    FD_ZERO(fd_set_ptr);
    for(; i < MAX_NODE_NUM; i++){
	if (monitored_fd_set[i] != -1){
	    FD_SET(monitored_fd_set[i], fd_set_ptr);
	}
    }
}

static int
dtm_get_max_fd(){
    int i = 0, max = -1;

    for (; i < MAX_NODE_NUM; i++){
	if (monitored_fd_set[i] > max)
	    max = monitored_fd_set[i];
    }
    return max;
}

/* Remove input sitting in standard input */
static void
dtm_clear_stdin_stream_chars(){
    int c;

    while((c = getchar()) != EOF && c != '\n');
}


static int
dtm_strtol(char *str, bool *success){
    long result;
    errno = 0;
    *success = false;

    result = strtol(str, NULL, 10);
    if(errno != 0){
	fprintf(stderr, "failed to convert the input value\n");
	return 0;
    }else if (result == LONG_MIN || result == LONG_MAX){
	fprintf(stderr, "input value exceeds the range of long\n");
	return 0;
    }else if (result <= INT_MIN || result >= INT_MAX){
	fprintf(stderr, "input value exceeds the range of integer\n");
	return 0;
    }

    *success = true;
    return (int)result;
}

static int
dtm_get_positive_integer(char *prompt){
    char buf[BUF_SIZE];
    bool success = false;
    int val;

    printf("%s", prompt);

    while(!success){
	fgets(buf, sizeof(buf), stdin);
	buf[sizeof(buf) - 1] = '\0';

	val = dtm_strtol(buf, &success);
	if (val < 0){
	    fprintf(stderr, "input value must be greater than 0\n");
	    success = false;
	}
    }

    return val;
}

static void
dtm_process_user_input(UserChoice *op, int *k, int *v){
    do{
	*op = dtm_get_positive_integer("Enter the number 1 or 2.\n"
				       "1: PUT <key> <value>\n"
				       "2: GET <key>\n");
    }while((*op != PUT) && (*op != GET));

    *k = dtm_get_positive_integer("<key> : ");

    /* PUT */
    if (*op == PUT)
	*v = dtm_get_positive_integer("<value> : ");
}

static void
dtm_send_udp_message(int udp_socket_fd, ApplyMsg *udp_msg){
    struct sockaddr_in next_node;
    int udp_server = sizeof(struct sockaddr_in);

    /* The credential of next node */
    next_node.sin_family = AF_INET;
    next_node.sin_port = next_node_udp_port;
    next_node.sin_addr.s_addr = own_node_ip;

    if (sendto(udp_socket_fd,
	       (char *)udp_msg,
	       sizeof(ApplyMsg),
	       0,
	       (struct sockaddr *)&next_node,
	       udp_server) < 0){
	perror("sendto");
	exit(-1);
    }else{
	printf("Sent '%s' over UDP from the originator (port = %d)\n",
	       dtm_get_apply_string(udp_msg->msg_id), ntohs(udp_msg->tcp_port));
    }
}

/* Send 'WHAT_X' msg and receive the orignator's 'PUT_REPLY_X' msg */
static void
dtm_ask_what_x(ApplyMsg *udp_msg){
    struct sockaddr_in tcp_dest;
    ApplyMsg client_data, *x_answer;
    char tcp_buf[BUF_SIZE];
    int sockfd;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    tcp_dest.sin_family = AF_INET;
    tcp_dest.sin_port = udp_msg->tcp_port;
    tcp_dest.sin_addr.s_addr = udp_msg->address;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(sockfd,
	    (struct sockaddr *)&tcp_dest,
		sizeof(struct sockaddr)) == -1){
	perror("connect");
	exit(-1);
    }

    client_data.msg_id = WHAT_X;
    client_data.k = udp_msg->x;
    if (sendto(sockfd,
	       &client_data,
	       sizeof(ApplyMsg),
	       0,
	       (struct sockaddr *)&tcp_dest,
	       sizeof(struct sockaddr)) < 0){
	perror("sendto");
	exit(-1);
    }

    addr_len = sizeof(struct sockaddr_in);
    if (recvfrom(sockfd,
		 (char *)&tcp_buf,
		 sizeof(tcp_buf),
		 0,
		 (struct sockaddr *)&tcp_dest,
		 &addr_len) < 0){
	perror("recvfrom");
	exit(-1);
    }

    x_answer = (ApplyMsg *)tcp_buf;
    dtm_store_hash_entry(udp_msg->k, x_answer->x);
    close(sockfd);
}

static void
dtm_send_stored_local_x(ApplyMsg *udp_msg){
    struct sockaddr_in tcp_dest;
    ApplyMsg client_data;
    int sockfd;

    tcp_dest.sin_family = AF_INET;
    tcp_dest.sin_port = udp_msg->tcp_port;
    tcp_dest.sin_addr.s_addr = udp_msg->address;

    sockfd = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (connect(sockfd,
		(struct sockaddr *)&tcp_dest,
		sizeof(struct sockaddr_in)) == -1){
	perror("connect");
	exit(-1);
    }

    client_data.msg_id = GET_REPLY_X;
    client_data.k = udp_msg->k;
    client_data.x = dtm_search_from_hash(udp_msg->k);
    if (sendto(sockfd,
	       &client_data,
	       sizeof(ApplyMsg),
	       0,
	       (struct sockaddr *)&tcp_dest,
	       sizeof(struct sockaddr_in)) < 0){
	perror("sendto");
	exit(-1);
    }

    close(sockfd);
}

static void
dtm_process_read_comm_socket(int master_socket){
    struct sockaddr_in client_addr;
    int comm_socket_fd;
    char buf[BUF_SIZE];
    ApplyMsg *received_msg, x_answer;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    comm_socket_fd = accept(master_socket,
			    (struct sockaddr *)&client_addr,
			    &addr_len);
    if (comm_socket_fd < 0){
	perror("accept");
	exit(-1);
    }

    printf("Connection accepted from client : %s:%u\n",
	   inet_ntoa(client_addr.sin_addr),
	   client_addr.sin_port);

    addr_len = sizeof(struct sockaddr_in);
    if (recvfrom(comm_socket_fd,
		 (char *)buf,
		 sizeof(buf),
		 0,
		 (struct sockaddr *)&client_addr,
		 &addr_len) < 0){
	perror("recvfrom");
	exit(-1);
    }

    received_msg = (ApplyMsg *)buf;
    switch(received_msg->msg_id){
	case WHAT_X:
	    dtm_set_apply_msg(&x_answer, PUT_REPLY_X,
			      0, what_x_rep,
			      0, 0);
	    if (sendto(comm_socket_fd,
		       (char *)&x_answer,
		       sizeof(ApplyMsg),
		       0,
		       (struct sockaddr *)&client_addr,
		       sizeof(struct sockaddr)) < 0){
		perror("sendto");
		exit(-1);
	    }
	    /* Reset the 'x' value for next iteration */
	    what_x_rep = -1;
	    break;
	case PUT_REPLY_X:
	    dtm_store_hash_entry(received_msg->k, received_msg->x);
	    break;
	case GET_REPLY_X:
	    printf("Stored data : key (%d) and value(%d)\n",
		   received_msg->k, received_msg->x);
	    break;
	default:
	    fprintf(stderr, "Got unkown MsgId from communication file descriptor\n");
	    exit(-1);
	    break;
    }

    close(comm_socket_fd);
}

static void
dtm_read_received_udp_msg(int udp_socket_fd, ApplyMsg *msg){
    char udp_buf[BUF_SIZE];
    struct sockaddr_in server_addr;
    socklen_t addr_len = sizeof(struct sockaddr_in);

    if (recvfrom(udp_socket_fd,
		 (char *)udp_buf,
		 sizeof(udp_buf),
		 0,
		 (struct sockaddr *)&server_addr,
		 &addr_len) < 0){
	fprintf(stderr, "Couldn't receive any data\n");
	exit(-1);
    }else{
	ApplyMsg *tmp;

	tmp = (ApplyMsg *)udp_buf;
	dtm_set_apply_msg(msg, tmp->msg_id,
			  tmp->k, tmp->x,
			  tmp->address, tmp->tcp_port);
    }
}

static int
dtm_get_own_node_id(char *exec_name){
    FILE *fp;
    char buf[BUF_SIZE],
	grep_running_node[] = "ps -ax | grep \"%s\" | grep -v \"grep\" | wc -l";

    /* Abstract command name from "./<exec>" format input */
    if (exec_name[0] == '.' && exec_name[1] == '/')
	exec_name += 2;

    sprintf(buf, grep_running_node, exec_name);

    /* How many DTM nodes are running ? */
    if ((fp = popen(buf, "r")) == NULL){
	perror("popen");
	exit(1);
    }

    fgets(buf, sizeof(buf), fp);

    pclose(fp);

    return atoi(buf);
}

/*
 * Convert CLAs and save them to global variables.
 * Use ntohs() for readability.
 */
static void
dtm_configure_CLA(int argc, char **argv){
    bool success;

    if (argc != 5){
	fprintf(stderr,
		"Each node requires four command line arguments\n"
		"argv[0] : executable\n"
		"argv[1] : own node IP\n" /* Expect 127.0.0.1 only now */
		"argv[2] : own node UDP port number\n"
		"argv[3] : successor node UDP port number\n"
		"argv[4] : own node TCP port number\n");
	exit(-1);
    }

    /* Six nodes should be set up for this project */
    my_node_id = dtm_get_own_node_id(argv[0]);
    if (my_node_id > MAX_NODE_NUM){
	fprintf(stderr, "too many nodes are running for requirement");
	exit(-1);
    }

    printf("Configure .... node_id = %d has been fetched\n", my_node_id);

    own_node_ip = inet_addr(argv[1]);
    if (own_node_ip == INADDR_NONE){
	fprintf(stderr, "invalid ip address\n");
	exit(-1);
    }

    own_node_udp_port = htons(dtm_strtol(argv[2], &success));
    if (success == false){
	fprintf(stderr, "invalid own node UDP port number\n");
	exit(-1);
    }

    printf("Configure .... own node UDP port = %d\n",
	   ntohs(own_node_udp_port));

    next_node_udp_port = htons(dtm_strtol(argv[3], &success));
    if (success == false){
	fprintf(stderr, "invalid successor node UDP port number\n");
	exit(-1);
    }

    printf("Configure .... successor node UDP port = %d\n",
	   ntohs(next_node_udp_port));

    own_node_tcp_port = htons(dtm_strtol(argv[4], &success));
    if (success == false){
	fprintf(stderr, "invalid own node TCP port number\n");
	exit(-1);
    }

    printf("Configure .... own node TCP port = %d\n",
	   ntohs(own_node_tcp_port));
}

static int
dtm_create_tcp_master_sock(void){
    struct sockaddr_in tcp_server_addr;
    int tcp_fd;

    if ((tcp_fd =
	 socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) == -1){
	perror("socket");
	exit(-1);
    }

    tcp_server_addr.sin_family = AF_INET; /* IP v4 */
    tcp_server_addr.sin_port = own_node_tcp_port; /* port number */
    tcp_server_addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(tcp_fd,
	     (struct sockaddr *)&tcp_server_addr,
	     sizeof(struct sockaddr_in)) == -1){
	perror("bind");
	exit(-1);
    }

    if (listen(tcp_fd, 5) < 0){
	perror("listen");
	exit(-1);
    }

    printf("Configure .... TCP socket has been created\n");

    return tcp_fd;
}
static int
dtm_create_udp_sock(void){
    struct sockaddr_in udp_server_addr;
    int udp_fd;

    if ((udp_fd =
	 socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP)) == -1){
	perror("socket");
	exit(-1);
    }

    udp_server_addr.sin_family = AF_INET; /* IP v4 */
    udp_server_addr.sin_port = own_node_udp_port;
    udp_server_addr.sin_addr.s_addr = own_node_ip;
    if (bind(udp_fd, (struct sockaddr *)&udp_server_addr,
	     sizeof(udp_server_addr)) < 0){
	perror("bind");
	exit(-1);
    }

    printf("Configure .... UDP socket has been created\n");

    return udp_fd;
}

static void
dtm_run_loop(int argc, char **argv){
    int tcp_master_fd, udp_socket_fd;
    fd_set readfds;

    /* Set up fd set */
    dtm_initialize_monitor_fd_set();
    tcp_master_fd = dtm_create_tcp_master_sock();
    udp_socket_fd = dtm_create_udp_sock();
    dtm_add_to_monitored_fd_set(0); /* console */
    dtm_add_to_monitored_fd_set(tcp_master_fd);
    dtm_add_to_monitored_fd_set(udp_socket_fd);

    while(1){
	dtm_re_init_readfds(&readfds);

	printf("Waiting for any file descriptors including console to be set on select()\n");

	if (select(dtm_get_max_fd() + 1, &readfds, NULL, NULL, NULL) < 0){
	    perror("select");
	    exit(-1);
	}

	sleep(1); /* for debugging */

	if (FD_ISSET(tcp_master_fd, &readfds)){
	    dtm_process_read_comm_socket(tcp_master_fd);
	}else if (FD_ISSET(udp_socket_fd, &readfds)){
	    ApplyMsg udp_msg;

	    dtm_read_received_udp_msg(udp_socket_fd, &udp_msg);
	    if (udp_msg.msg_id == PUT_FORWARD){
		if (find_node(udp_msg.k) == my_node_id){
		    dtm_ask_what_x(&udp_msg);
		}else{
		    ApplyMsg udp_forward_put;

		    dtm_set_apply_msg(&udp_forward_put, PUT_FORWARD,
				      udp_msg.k, 0,
				      udp_msg.address, udp_msg.tcp_port);
		    dtm_send_udp_message(udp_socket_fd,
					 &udp_forward_put);
		}
	    }else if(udp_msg.msg_id == GET_FORWARD){
		if (find_node(udp_msg.k) == my_node_id){
		    dtm_send_stored_local_x(&udp_msg);
		}else{
		    ApplyMsg udp_forward_get;

		    dtm_set_apply_msg(&udp_forward_get, GET_FORWARD,
				      udp_msg.k, 0,
				      udp_msg.address, udp_msg.tcp_port);
		    dtm_send_udp_message(udp_socket_fd,
					 &udp_forward_get);
		}
	    }
	}else if (FD_ISSET(0, &readfds)){
	    UserChoice user_op;
	    int k, v;

	    /*
	     * Clear the stdin stream characters.
	     * Otherwise, the next iteration of select() will
	     * get unblocked immediately by that characters.
	     */
	    dtm_clear_stdin_stream_chars();
	    dtm_process_user_input(&user_op, &k, &v);
	    switch(user_op){
		case PUT:
		    if (find_node(k) == my_node_id){
			dtm_store_hash_entry(k, v);
		    }else{
			ApplyMsg first_put_forward;

			/* Store 'x' until 'WHAT_X' arrives from target node */
			what_x_rep = v;
			dtm_set_apply_msg(&first_put_forward, PUT_FORWARD,
					  k, 0 /* no need */,
					  own_node_ip, own_node_tcp_port);
			dtm_send_udp_message(udp_socket_fd,
					     &first_put_forward);
		    }
		    break;
		case GET:
		    if (find_node(k) == my_node_id){
			int val;

			val = dtm_search_from_hash(k);
			if (val == -1)
			    printf("The data hasn't been registered yet\n");
			else
			    printf("The value for key = %d : %d\n", k, val);
		    }else{
			ApplyMsg first_get_forward;

			dtm_set_apply_msg(&first_get_forward,
					  GET_FORWARD, k, 0 /* no need */,
					  own_node_ip, own_node_tcp_port);
			dtm_send_udp_message(udp_socket_fd,
					     &first_get_forward);
		    }
		    break;
		default:
		    fprintf(stderr, "Invalid fd operation has been detected. Retry...\n");
	    }
	}else{
	    fprintf(stderr, "Unknown fd has been invoked\n");
	}
    }
}

int
main(int argc, char **argv){

    dtm_configure_CLA(argc, argv);
    dtm_run_loop(argc, argv);

    return 0;
}
