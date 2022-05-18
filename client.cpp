//Rolling alphabet tables
//Client
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <sys/types.h>
#include <netinet/tcp.h>
#include <sys/timeb.h>
#include <fcntl.h>
#include <stdarg.h>
#include <time.h>
#include <signal.h>
#include <cstring>
#include <pthread.h>
#include <poll.h>
#include <string>
#include <queue>
#include <iostream>
#define MAX_FILE_SIZE 10000000
#define MAX_MESSAGE_SIZE 256
#define MAX_SEND_DATA 500000
typedef unsigned char BYTE;
typedef unsigned int DWORD;
typedef unsigned short WORD;
FILE *user_input;
int maxSize = 500;
int connection_established = 0;
struct pollfd peer;
struct pollfd peers[2];
char *user_name;
char svrIP[50];
int svrPort = 0;
int svrPortFile = 4068;
char filename[50];
int sockFD;
int sockFDs[2];
int setupFD = 0;
int port_file = 0;
int port_message = 0;
struct CONN_STAT{
    std::queue<BYTE *>sending_message_queue;
    std::queue<int> send_message_size;
    std::queue<char*> filenames;
    int nSent;
    int received_message_size = 0;
    BYTE *received_message = NULL;
    int nRecv = 0;
    int maintain_connection = 0;
    int receiving_stage=0;
};
struct CONN_STAT connStats[2];
void Error(const char * format, ...) {
	char msg[4096];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "Error: %s\n", msg);
	exit(-1);
}

void Log(const char * format, ...) {
	char msg[2048];
	va_list argptr;
	va_start(argptr, format);
	vsprintf(msg, format, argptr);
	va_end(argptr);
	fprintf(stderr, "%s\n", msg);
}
int shutdown_listen = 0;
//Function used to send file in non blocking manner, that means, file is only allowed
//to be sent with MAX_SEND_DATA, to avoid blocking of sending message
int Send_NonBlocking(int sockFD, const BYTE * data, int len, struct CONN_STAT* pStat, struct pollfd *pPeer) {
    int sent = 0;
    //Only allow a maximum file chunks of 200000 to be sent to avoid blocking for sending message	
	while (pStat->nSent < len) {
		//pStat keeps tracks of how many bytes have been sent, allowing us to "resume" 
		//when a previously non-writable socket becomes writable. 
        int sending_size = MAX_SEND_DATA;
        if (sending_size > len - pStat->nSent){
            sending_size = len - pStat->nSent;
        }
		int n = send(sockFD, data + pStat->nSent, sending_size, 0);
        sent = sent + n;
		if (n >= 0) {
			pStat->nSent += n;
            if (sent >= MAX_SEND_DATA){
                return 0;
            }
		} else if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) {
			Log("Connection closed.");
			close(sockFD);
            connection_established = -1;
			return -1;
		} else {
			Error("Unexpected send error %d: %s", errno, strerror(errno));
		}
        
	}
	return 0;
}
//Function used to send message in blocking manner, used to send messages as their size < 256 bytes
int Send_Blocking(int sockFD, const BYTE * data, int len) {
	int nSent = 0;
	while (nSent < len) {
		int n = send(sockFD, data + nSent, len - nSent, 0);
		if (n >= 0) {
			nSent += n;
		} else if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) {
			Log("Connection closed.");
			close(sockFD);
            connection_established = -1;
			return -1;
		} else {
			Error("Unexpected error %d: %s.", errno, strerror(errno));
		}
	}
	return 0;
}

//Always received data in blocking manner
int Recv_Blocking(int sockFD, BYTE * data, int len) {
	int nRecv = 0;
	while (nRecv < len) {
		int n = recv(sockFD, data + nRecv, len - nRecv, 0);
		if (n > 0) {
			nRecv += n;
		} else if (n == 0 || (n < 0 && errno == ECONNRESET)) {
			Log("Connection closed.");
			close(sockFD);
            connection_established = -1;
			return -1;
		} else {
			Error("Unexpected error %d: %s.", errno, strerror(errno));
		}
	}
	return 0;
}
//Used to reset the receiving socket
void reset_receiving_socket(int message_size, int receiving_stage, int type){
    connStats[type].receiving_stage = receiving_stage;
    connStats[type].received_message_size = message_size;
    connStats[type].nRecv = 0;
    if (connStats[type].received_message != NULL){
        free(connStats[type].received_message);
    }
    connStats[type].received_message = (BYTE *)malloc(message_size);

}
//When message is fully received, then process the message
void processReceivedMessage(int type){
        struct CONN_STAT connStat = connStats[type];
        char *buf = (char*) connStat.received_message;
        int received_message_size = connStat.received_message_size;
        std::string buf_string = std::string(buf, received_message_size);
        int index = buf_string.find(" ");
        std::string message_type_string = buf_string.substr(0,index);
        std::string body = buf_string.substr(index+1, received_message_size);
        char *message_type = (char *)message_type_string.c_str();
        char *message_body = (char *)body.c_str();
        //If the case is text reply, then it's the message coming from the server itself, either a success or error message for some requests
        if (strcmp(message_type, "TEXT_REPLY") == 0){
            if (strcmp(message_body, "VALID_REGISTER") == 0){
                printf("Valid account registration\n");
                connection_established = 1;  
            }else if(strcmp(message_body, "VALID_LOGIN") == 0){
                printf("Hello %s\n\n", user_name);
                connection_established = 1;
            }else{
                if (strcmp(message_body,"INVALID_REGISTER") == 0){
                    printf("register failed\n\n");
                }else if(strcmp(message_body,"INVALID_LOGIN") == 0){
                    printf("Login failed\n\n");
                    connection_established = -1;
                }else{
                    printf("%s\n",message_body);
                }
            }
        }else if(strcmp(message_type, "TEXT_FORWARD") == 0){
            //Message coming from other clients, simply print it out
            printf("%s\n", message_body);
        }else if(strcmp(message_type, "FILE_FORWARD") == 0){
            //File coming from other clients, need to open file, and write
            int next_index = body.find(" ");
            received_message_size = received_message_size - index - 1;
            std::string sender_name = body.substr(0, next_index);
            char *sender = (char *)sender_name.c_str();
            std::string file_content = body.substr(next_index + 1, received_message_size);
            received_message_size = received_message_size - next_index - 1;
            next_index = file_content.find(" ");
            std::string filename_string = file_content.substr(0,next_index);
            std::string file_body_string = file_content.substr(next_index+1, received_message_size);
            char *file_body = (char *)file_body_string.c_str();
            char *filename = (char *)filename_string.c_str();
            FILE *newfile = fopen(filename, "wb");
            received_message_size = received_message_size - next_index - 1;
            fwrite(file_body, 1, received_message_size, newfile);
            fclose(newfile);
            printf("%s file %s of size %d\n\n", sender, filename, received_message_size);
        }

}
//Function runs in seperate thread that listens to incoming connection of server
void *ClientListen(void *args){
    //wait until connection has been set up
    while (setupFD == 0){

    }
    while(1){
        peers[0].events = POLLRDNORM|POLLHUP|POLLERR;
        peers[0].fd = sockFDs[0];
        peers[1].events = POLLRDNORM|POLLHUP|POLLERR;
        peers[1].fd = sockFDs[1];
        //Wake up every 5 seconds, or when poll time outs, poll time out is set to 5 to avoid the case when client shuts down sending thread
        poll(peers, 2, 5000);
        if (shutdown_listen == 1){
            return NULL;
        }
        int flag = 0;
        for (int i = 0; i < 2; i++){
            struct pollfd peer = peers[i];
            int sockFD = sockFDs[i];
            if (peer.revents & POLLHUP | peer.revents & POLLERR){
                printf("error with poll\n");
                flag = 1;
                break;
            }
            if (peers[i].revents & POLLRDNORM){
                //Receive the size first, then receive the message
                reset_receiving_socket(4,0,i);
                int received_message_size_net = 0;
                int result = Recv_Blocking(sockFD, (BYTE *)connStats[i].received_message, connStats[i].received_message_size);
                if (result < 0){
                    printf("Failed to receive message size from server\n");
                    flag = 1;
                    break;
                }
                if (connStats[i].receiving_stage == 0){
                    //Receive the message
                    int *received_message_size_ptr = (int *)connStats[i].received_message;
                    received_message_size_net = *received_message_size_ptr;
                    int received_message_size = ntohl(received_message_size_net);
                    reset_receiving_socket(received_message_size, 1,i);
                    result = Recv_Blocking(sockFD, (BYTE *)connStats[i].received_message,connStats[i].received_message_size);
                    if (result < 0){
                        printf("Failed to receive message from server\n");
                        flag = 1;
                        break;
                    }
                    
                }
                processReceivedMessage(i);
            }
            //continue to receive the message

        }
        if (flag == 1){
            break;
        }
        
    }
    return NULL;

}
//Will call Send_Blocking, handling more logic before the call
int sendMesageBlocking(char *message, long size, int type){
    int message_length_net = htonl(size);
    int *message_size_data = (int*)malloc(sizeof(int));
    *message_size_data = message_length_net;
    int sent = Send_Blocking(sockFDs[type],(BYTE*)message_size_data,sizeof(int));
    if (sent < 0){
            printf("Failed to send size of message\n");
            return -1;
    }
    connStats[type].nSent = 0;
    sent = Send_Blocking(sockFDs[type],(BYTE*)message, size);
    if (sent < 0){
            printf("Failed to send the information of user\n");
            return -1;
    }
    connStats[type].nSent = 0;
    return 0;

}
//Will call Send_NonBlock, this is specifically used for sending file only
void sendMessage(char *message, char *filename,long size, int type){
    if (connStats[type].sending_message_queue.empty()){
        //If it's the first file to be sent then proceed to send
        int message_length_net = htonl(size);
        int *message_size_data = (int*)malloc(sizeof(int));
        *message_size_data = message_length_net;
        //Push all essential information to queue before start sending (size, net size, the message itself, and the type, 0 for normal message, 1 for file)
        connStats[type].sending_message_queue.push((BYTE*)message_size_data);
        connStats[type].sending_message_queue.push((BYTE*)message);
        connStats[type].send_message_size.push(sizeof(int));
        connStats[type].send_message_size.push(size);
        connStats[type].filenames.push(filename);
        printf("Sending file %s\n\n", connStats[type].filenames.front());
        int sent = Send_NonBlocking(sockFDs[type],connStats[type].sending_message_queue.front(), connStats[type].send_message_size.front(), &connStats[type], &peers[type]);
        if (sent < 0){
            printf("Failed to send size of message\n");
        }else if (connStats[type].nSent < connStats[type].send_message_size.front()){
            return;
        }
        connStats[type].sending_message_queue.pop();
        connStats[type].send_message_size.pop();
        connStats[type].nSent = 0;
        sent = Send_NonBlocking(sockFDs[type],connStats[type].sending_message_queue.front(), connStats[type].send_message_size.front(), &connStats[type], &peers[type]);
        if (sent < 0){
            printf("Failed to send the information of user\n");
        }else if (connStats[type].nSent < connStats[type].send_message_size.front()){
            return;
        }
        printf("Sent file %s of size %ld bytes\n", connStats[type].filenames.front(),size);
        connStats[type].sending_message_queue.pop();
        connStats[type].send_message_size.pop();
        free(connStats[type].filenames.front());
        connStats[type].filenames.pop();
        connStats[type].nSent = 0;
    }else{
        //Else push the message to the waiting queue until its turn comes
        int message_length_net = htonl(size);
        int *message_size_data = (int*)malloc(sizeof(int));
        *message_size_data = message_length_net;
        connStats[type].sending_message_queue.push((BYTE*)message_size_data);
        connStats[type].sending_message_queue.push((BYTE*)message);
        connStats[type].send_message_size.push(sizeof(int));
        connStats[type].send_message_size.push(size);
        connStats[type].filenames.push(filename);
    }
    
}
//Check if the user name and password are valid for registration
int checkRegistration(std::string username_str, std::string password_str){
    int name_length = username_str.length();
    char *username = (char*)username_str.c_str();
    int password_length = password_str.length();
    char *password = (char*)password_str.c_str();
    if (name_length < 4 || name_length > 8 || password_length < 4 || password_length > 8){
        printf("Invalid information for registration, length and password must be within 4 to 8 characters\n");
        return 0;
    }
    for (int i = 0; i < name_length; i++){
        char c = username[i];
        if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)){
            continue;
        }else{
            printf("Invalid character found for username, please only use digit, or letter\n");
            return 0;
        }
    }
    for (int i = 0; i < password_length; i++){
        char c = password[i];
        if ((c >= 48 && c <= 57) || (c >= 65 && c <= 90) || (c >= 97 && c <= 122)){
            continue;
        }else{
            printf("Invalid character found for password, please only use digit, or letter\n");
            return 0;
        }
    }
    return 1;
}
//Will be called periodically to make sure data can be continue to written if there're still data left
int check_continue_write(int connection){
    //If the poll call returns from writenorm event, then proceed with the write logic
    if (!connStats[connection].sending_message_queue.empty()) {
                int size = connStats[connection].sending_message_queue.size();
                if (connStats[connection].nSent == 0 && (size&0)==0){
                    //start new round
                    printf("Sending file %s\n\n", connStats[connection].filenames.front());
                }
                //Proceding to send the message that's currently in sending progress but was blocked because buffer was full for the connection
                int send = Send_NonBlocking(peers[connection].fd, (BYTE*)connStats[connection].sending_message_queue.front(), connStats[connection].send_message_size.front(), &connStats[connection], &peers[connection]);
                //If client process terminates or logout, then remove the connection
                if ( send  < 0) {
                    printf("Lost connection with server\n\n");
		        }else if(connStats[connection].nSent == connStats[connection].send_message_size.front()){//finish sending message size
                        //If finish sending message then pop out that message from the sending queue
                        size = connStats[connection].sending_message_queue.size();
                        if ((size&1) == 1){
                            printf("Sent file %s of size %d\n\n", connStats[connection].filenames.front(),connStats[connection].send_message_size.front());
                            connStats[connection].filenames.pop();
                        }
                        connStats[connection].sending_message_queue.pop();
                        connStats[connection].send_message_size.pop();
                        connStats[connection].nSent = 0;
                        
                }
                return 1;

	}else{
        return 0;
    }

}
//Set up socket to send data to server, 1 for message channel, and 1 for file channel
void setUpSocketAndConnect(int type){	
	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(serverAddr));
	serverAddr.sin_family = AF_INET;
    if (type == 0){
        serverAddr.sin_port = htons((unsigned short) svrPort);
    }else{
        serverAddr.sin_port = htons((unsigned short) svrPortFile);
    }
	inet_pton(AF_INET, svrIP, &serverAddr.sin_addr);
    sockFD = socket(AF_INET, SOCK_STREAM, 0);
    if (sockFD == -1) {
			Error("Cannot create socket.");		
	}
    sockFDs[type] = sockFD;
    int connect_message_port = connect(sockFDs[type], (const struct sockaddr *) &serverAddr, sizeof(serverAddr));
    if (connect_message_port != 0){
        Error("Cannot connect to server ");
    }

}
//Main thread reading in user script and execute the command one by one
void *DoClient(void *args) {
	setUpSocketAndConnect(0);
    setUpSocketAndConnect(1);
    setupFD = 1;
    user_input = fopen(filename, "r");
    while (1){
        char *command_input = (char*)malloc(sizeof(char)*270);
        //read in commands from files
        check_continue_write(1);
        if (fgets(command_input, 500, user_input) != NULL){
            int l = strlen(command_input);
            std::string message_string = std::string(command_input);
            message_string = message_string.substr(0, message_string.length() - 1);
            //Special one word command
            if (message_string.compare("LIST") == 0){
                while (connection_established == 0){

                }if (connection_established == -1){
                    printf("REJECT: Connection has not been established with server, can not set file\n");
                    continue;
                }
                sendMesageBlocking(command_input,message_string.length(),0);
                continue;
            }else if(message_string.compare("LOGOUT") == 0){
                printf("Logging out\n");
                while (connection_established == 0){

                }if (connection_established == -1){
                    printf("REJECT: Connection has not been established with server, can not set file\n");
                    continue;
                }
                sendMesageBlocking(command_input,message_string.length(),0);
                close(sockFDs[0]);
                close(sockFDs[1]);
                shutdown_listen = 1;
                break;
            }
            int index = message_string.find(" ");
            std::string string_command = message_string.substr(0, index);
            std::string string_body = message_string.substr(index+1,l);
            char *command = (char*)string_command.c_str();
            char *body = (char*)string_body.c_str();
            //Register command
            if ((strcmp(command, "REGISTER") == 0 || strcmp(command, "LOGIN") == 0)){
                int index_name = string_body.find(" ");
                std::string name = string_body.substr(0,index_name);
                int body_length = string_body.length();
                std::string password = string_body.substr(index_name+1, body_length);
                //Needs to check for validation first before sending request to server
                if (checkRegistration(name, password) == 0){
                    continue;
                }
                user_name = (char *)name.c_str();
                free(command_input);
                command_input =  (char*) message_string.c_str();
                sendMesageBlocking(command_input, l,0);
                if (strcmp(command, "LOGIN") == 0){
                    printf("Started to login\n\n");
                    while (connection_established == 0){

                    }if (connection_established == -1){
                        printf("REJECT: Connection has not been established with server, can not set file\n");
                        connection_established = 0;
                        continue;
                    }else{
                        char *next_message = (char*)malloc(sizeof(char)*30);
                        sprintf(next_message,"SETUSER %s",user_name);
                        l = strlen(next_message);
                        sendMesageBlocking(next_message, l,1);
                        free(next_message);
                    }
                }
                
            }else if(strcmp(command, "DELAY") == 0){
                //Delay the next command, but still actively doing other jobs (continue to send next pended message)
                int time = atoi(body)*1000;
                int sleep_quatum = 1;
                int start = 0;
                if (sleep_quatum > time){
                    sleep_quatum = time;
                }
                while (start < time){
                    //sleep only for a small time quantum, then wake up and continue to do the job
                    usleep(sleep_quatum*1000);
                    check_continue_write(1);
                    start = start + sleep_quatum;
                    int remained = time -start;
                    if (remained < sleep_quatum){
                        sleep_quatum = remained;
                    }
                }
                

                
            }else if(strcmp(command, "SEND") == 0 || strcmp(command, "SEND2") == 0 || strcmp(command, "SENDA") == 0 || strcmp(command, "SENDA2") == 0){
                //send normal message
                while (connection_established == 0){
                }
                if (connection_established == -1){
                    printf("REJECT: Connection has not been established with server\n");
                    continue;
                }else{
                    if (strcmp("SEND",command) == 0 || strcmp("SENDA", command) == 0){
                        int message_length = string_body.length();
                        if (message_length > MAX_MESSAGE_SIZE){
                            printf("REJECT: Message size could not exceed 256 bytes");
                            continue;
                        }
                    }else if (strcmp("SEND2", command) == 0 || strcmp("SENDA2",command) == 0){
                        int next_split = string_body.find(" ");
                        int message_length = string_body.length() - next_split;
                        if (message_length > MAX_MESSAGE_SIZE){
                            printf("REJECT: Message size could not exceed 256 bytes");
                            continue;
                        }
                    }
                    sendMesageBlocking(command_input,l,0);
                }
            }else if(strcmp(command,"SENDF") == 0 || strcmp(command,"SENDF2") == 0){
                //send file case
                while (connection_established == 0){

                }
                if (connection_established == -1){
                    printf("REJECT: Connection has not been established with server\n");
                    continue;
                }
                int filename_length = string_body.length();
                FILE *file;
                char *sending_file_name;
                if (strcmp(command,"SENDF") == 0){
                    char *filename = (char *)string_body.c_str();
                    int length_filename = strlen(filename);
                    sending_file_name = (char *)malloc(sizeof(char)*length_filename);
                    strcpy(sending_file_name,filename);
                    file = fopen(filename, "rb");  
                }else if(strcmp(command, "SENDF2") == 0){
                    int receiver_filename_length = string_body.length();
                    int seperate_point = string_body.find(" ");
                    std::string filename_str = string_body.substr(seperate_point+1,receiver_filename_length);
                    char *filename = (char*)filename_str.c_str();
                    int length_filename = strlen(filename);
                    sending_file_name = (char *)malloc(sizeof(char)*length_filename);
                    strcpy(sending_file_name,filename);
                    file = fopen(filename, "rb");

                }
                if (file != NULL){
                        //If file is invalid, then print error message
                        fseek(file,0,SEEK_END);
                        long size = ftell(file);
                        if (size > MAX_FILE_SIZE){
                            printf("REJECT: File size could not exceed 10MB\n");
                            continue;
                        }
                        char *file_content = (char*)malloc(l+size);
                        char *copy = (char *)malloc(size);
                        strcpy(file_content, command_input);
                        file_content[l-1] = ' ';
                        BYTE *file_body = (BYTE *)(file_content + l);
                        fseek(file,0,SEEK_SET);
                        fread(file_body,size,1,file);
                        fclose(file);
                        sendMessage(file_content, sending_file_name,l+size,1);
                }else{
                    printf("REJECT: File does not exist\n");
                }
            }else{
                printf("Invalid command\n\n");
            }

        }
        
    }
    //When finish executing all commands, make sure all pended messages got sent before exist
    int still_write = check_continue_write(1);
    while (still_write){
        still_write = check_continue_write(1);
    }
    printf("Finished with sending message\n");
    shutdown_listen = 1;
    return NULL;	
}


int main(int argc, char * * argv) {	
    if (argc != 5) {
		Log("Usage: %s [server IP] [server Port message] [server Port file] [file name]", argv[0]);
		return -1;
	}
    strcpy(filename, argv[4]);
    svrPortFile = atoi(argv[3]);
    svrPort = atoi(argv[2]);
    strcpy(svrIP, argv[1]);
    pthread_t listen_thread;
    pthread_t send_thread;
	//Create client executing thread
    pthread_create(&send_thread,NULL,DoClient,NULL);
    sleep(1);
    //Create client listening thread
    pthread_create(&listen_thread,NULL,ClientListen, NULL);
    //Wait for both threads before exit
    pthread_join(send_thread,NULL);
    pthread_join(listen_thread,NULL);
	return 0;
}
