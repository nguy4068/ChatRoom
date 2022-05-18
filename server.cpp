//Rolling alphabet tables
//Non-blocking server
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <errno.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/tcp.h>
#include <sys/timeb.h>
#include <fcntl.h>
#include <stdarg.h>
#include <poll.h>
#include <signal.h>
#include <string>
#include <queue>
#include <iostream>
#include <pthread.h>
#include <time.h>
typedef unsigned char BYTE;
typedef unsigned int DWORD;
typedef unsigned short WORD;
#define MAX_REQUEST_SIZE 10000000
#define MAX_CONCURRENCY_LIMIT 10
#define MAX_USERNAME_LENGTH 8
#define MESSAGE_PORT 4000
#define FILE_PORT 4001
int num_total_users = 0;
int size_name_storage = 0;
//struct use to store information of all registered users
struct USER{
    char *name;
    int online;
    int connFile;
    int connMessage;
};
//struct used to store arguments for concurrent write file function
struct WRITE_FILE_ARG{
    char *filename;
    char *message;
    int size;
};
//struct use to store information of sockets
struct CONN_STAT{
    std::queue<BYTE*> send_messages;//array to store all sending messages, when multiple message comming while the previous one has not been done sended, then push all of them to the queue
    std::queue<int> send_message_size;//store message size of each message
    std::queue<int> types;
    std::queue<char *>filenames;
    int received_message_size = 0;//store size of currently received message
    BYTE *received_message = NULL;//store received message
    int nRecv = 0;//number of bytes that have been received sucessfully to the client
    int nSent = 0;//number of bytes that have been sent successfully to the client
    BYTE *current_sending_message;//points to the message that's currently in sending process to the client
    int current_sending_message_size = 0;//size of the current sending message (text message)
    int sending_data_size = 0;//size of the current sending message (both text message, and the size of the text message)
    int maintain_connection = 0;//indicate whether an incoming message should be forwarded to this connection or not
    char *username;//user name that initiates this connection
    int receiving_stage=0;//receiving stage = 0 means it's receiving message size, receiving stage = 1 means it's receiving message
    int sending_stage=0;//sending_stage = 0 means it's transferring data size, sending stage = 1 means it's transfering the real message
    int address=0;
    int type = 0;//Message or file channel
    int port1=0;//port 1 listens to message channel
    int port2=0;//port 2 listen to file channel
};
//struct used to store arguments for concurrent forwarding message
struct SEND_MESSAGE_ARG{
    char *message;
    int connection;
    int size;
    int type;
    char *filename;
};
int nConns = 0;	//total # of data sockets
struct pollfd peers[(MAX_CONCURRENCY_LIMIT+1)*2];	//sockets to be monitored by poll()
struct pollfd peers_files[(MAX_CONCURRENCY_LIMIT+1)*2];
struct CONN_STAT connStat[(MAX_CONCURRENCY_LIMIT+1)*2];	//app-layer stats of the sockets
struct CONN_STAT connStatFile[(MAX_CONCURRENCY_LIMIT+1)*2];
//storing list of all registered users
USER *userlist;
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
//reset sending socket
void reset_sending_socket(int connection, int message_size, int sending_stage){
        connStat[connection].sending_stage = sending_stage;
        connStat[connection].sending_data_size = message_size;
        connStat[connection].nSent = 0;
}
//reset receiving socket
void reset_receiving_socket(int connection, int message_size, int receiving_stage){
    connStat[connection].receiving_stage = receiving_stage;
    connStat[connection].received_message_size = message_size;
    connStat[connection].nRecv = 0;
    /*if (connStat[connection].received_message != NULL){
        free(connStat[connection].received_message);
        connStat[connection].received_message = NULL;
    }*/
    connStat[connection].received_message = (BYTE *)malloc(message_size);

}
void logTimeStamp(){
    time_t t;
    time(&t);
    char *time_format = (char*)ctime(&t);
    int l = strlen(time_format);
    std::string time_format_cpp = std::string(time_format);
    time_format_cpp = time_format_cpp.substr(0,l-1);
    time_format = (char *)time_format_cpp.c_str();
    printf("[%s]\n", time_format);
}
//Sending data in non blocking manner to a connection
int Send_NonBlocking(int sockFD, const BYTE * data, int len, struct CONN_STAT * pStat, struct pollfd * pPeer) {	
	while (pStat->nSent < len) {
		//pStat keeps tracks of how many bytes have been sent, allowing us to "resume" 
		//when a previously non-writable socket becomes writable. 
		int n = send(sockFD, data + pStat->nSent, len - pStat->nSent, 0);
		if (n >= 0) {
			pStat->nSent += n;
		} else if (n < 0 && (errno == ECONNRESET || errno == EPIPE)) {
			Log("Connection closed.");
			close(sockFD);
			return -1;
		} else if (n < 0 && (errno == EWOULDBLOCK)) {
			//The socket becomes non-writable. Exit now to prevent blocking. 
			//OS will notify us when we can write
			pPeer->events |= POLLWRNORM; 
			return 0; 
		} else {
			Error("Unexpected send error %d: %s", errno, strerror(errno));
		}
	}
	pPeer->events &= ~POLLWRNORM;
	return 0;
}
//Receiving data in a non blocking manner for the socket
int Recv_NonBlocking(int sockFD, BYTE * data, int len, struct CONN_STAT * pStat, struct pollfd * pPeer) {
	//pStat keeps tracks of how many bytes have been rcvd, allowing us to "resume" 
	//when a previously non-readable socket becomes readable. 
	while (pStat->nRecv < len) {
		int n = recv(sockFD, data + pStat->nRecv, len - pStat->nRecv, 0);
		if (n > 0) {
			pStat->nRecv += n;
		} else if ((n < 0 && errno == ECONNRESET)) {
			close(sockFD);
			return -1;
		} else if (n==0){
            close(sockFD);
            return -1;
        }
        else if (n < 0 && (errno == EWOULDBLOCK)) { 
			//The socket becomes non-readable. Exit now to prevent blocking. 
			//OS will notify us when we can read
			return 0; 
		} else {
			Error("Unexpected recv error %d: %s.", errno, strerror(errno));
		}
	}
	
	return 0;
}

void SetNonBlockIO(int fd) {
	int val = fcntl(fd, F_GETFL, 0);
	if (fcntl(fd, F_SETFL, val | O_NONBLOCK) != 0) {
		Error("Cannot set nonblocking I/O.");
	}
}
void setUserOffline(char *name){
    for (int i = 0; i < num_total_users; i++){
        if (strcmp(name, userlist[i].name) == 0){
            userlist[i].online = 0;
            break;
        }
    }
}
//remove connection when user logout or terminates the client process
void RemoveConnection(int i) {
	close(peers[i].fd);	
    logTimeStamp();
	if (i <= nConns+1) {
        if (connStat[i].username != NULL){
            char *username = connStat[i].username;
            setUserOffline(username);
            printf("Remove connection for %d\n", i);
            
        }
		memmove(peers + i, peers + i + 1, (nConns+1-i) * sizeof(struct pollfd));
		memmove(connStat + i, connStat + i + 1, (nConns+1-i) * sizeof(struct CONN_STAT));
        nConns--;
	}
}
void checkPrintMessage(int connection){
    int size = connStat[connection].send_messages.size();
    if (size&1){
        if ( connStat[connection].types.front() == 1){
            logTimeStamp();
            printf("Sent file %s of size %d to user %s\n", connStat[connection].filenames.front(), connStat[connection].send_message_size.front(), connStat[connection].username);
            connStat[connection].filenames.pop();
        }else if (connStat[connection].types.front() == 0){
            logTimeStamp();
            printf("Sent message: '%s' to user %s\n", connStat[connection].send_messages.front(),connStat[connection].username);
        }else{
            logTimeStamp();
            printf("Sent %d bytes of offline message to user %s\n", connStat[connection].send_message_size.front(),connStat[connection].username);
        }
        connStat[connection].types.pop();
    }
    connStat[connection].send_messages.pop();
    connStat[connection].send_message_size.pop();
}
/**
    @param connection: connection that needs to be checked for write event
    This function will be invoked inside DoServer when the poll calls return
    for either RDNORM or WRNORM event
**/
void check_continue_write(int connection){
    //If the poll call returns from writenorm event, then proceed with the write logic
    if (peers[connection].revents & POLLWRNORM) {
                //Proceding to send the message that's currently in sending progress but was blocked because buffer was full for the connection
                int send = Send_NonBlocking(peers[connection].fd, (BYTE*)connStat[connection].send_messages.front(), connStat[connection].send_message_size.front(), &connStat[connection], &peers[connection]);
                //If client process terminates or logout, then remove the connection
                if ( send  < 0) {
                    logTimeStamp();
                    printf("Lost connection with client\n");
			        RemoveConnection(connection);
		        }else if(connStat[connection].nSent == connStat[connection].send_message_size.front()){//finish sending message size
                        //If finish sending message then pop out that message from the sending queue
                        checkPrintMessage(connection);
                        BYTE *current_sending_message = connStat[connection].send_messages.front();
                        connStat[connection].nSent = 0;
                        //proceed to send the next message in the queue
                        while (current_sending_message != NULL){
                            //check size of the sending message
                            
                            //Extract out the size of the sending message, and the sending message
                            int size = connStat[connection].send_messages.size();
                            if ((size&0) == 0){
                                if ( connStat[connection].types.front() == 1){
                                    logTimeStamp();
                                    printf("Sending file %s to user %s\n", connStat[connection].filenames.front(), connStat[connection].username);
                                }else{
                                    logTimeStamp();
                                    printf("Sending message to user %s\n",connStat[connection].username);
                                }


                            }
                            connStat[connection].current_sending_message = connStat[connection].send_messages.front();
                            int send = Send_NonBlocking(peers[connection].fd, (BYTE*)connStat[connection].send_messages.front(), connStat[connection].send_message_size.front(), &connStat[connection], &peers[connection]);
                            //If connection is lost then removed the connection
                            if ( send  < 0) {
                                printf("Lost connection with client\n");
			                    RemoveConnection(connection);
		                    }else if(connStat[connection].nSent < connStat[connection].send_message_size.front()){
                                break;
                            }
                            //If not then pop out the message, and proceding to send the next one
                            checkPrintMessage(connection);
                            current_sending_message = connStat[connection].send_messages.front();
                            connStat[connection].nSent = 0;

                        }
                        
                }

	}

}
/**
    @param connection: connection that the message will be sent to
    @param original_message: the original message
    @param size: size of the original message
    @return 1 if the message is finished sending without being blocked due to full buffer, 0 otherwise
**/
int sendMessage(int connection, char *original_message, int size, int type, char *filename){
    //make a copy of the message first
    
    char *message = (char*)malloc(sizeof(char)*size);
    memcpy(message,original_message,size);
    //if the sending queue is empty, then go ahead and send the message
    if (connStat[connection].send_messages.empty()){
            logTimeStamp();
            if (type == 0){
                if (connStat[connection].username != NULL){
                    printf("Start sending message for user %s\n", connStat[connection].username);
                }else{
                    printf("Start sending message for user\n");
                }
                
            }else if (type==1){
                printf("Sending file %s to %s\n", filename,connStat[connection].username);
                connStat[connection].filenames.push(filename);
            }else if(type == 2){
                printf("Sending saved offline message to %s\n", connStat[connection].username);
            }
            
            //Need to push sending messages for each forwarding messaging, one storing the size (network byte ordered) of the message, and one storing the message itself
            //Need to push 2 sending message sizes to the data size queue, one is 4 (size of sending message size), the other one is value of message size
            int *netsize_pointer = (int*)malloc(sizeof(int));
            int netsize = htonl(size);
            *netsize_pointer = netsize;
            connStat[connection].nSent = 0;
            connStat[connection].send_messages.push((BYTE*)netsize_pointer);
            connStat[connection].send_messages.push((BYTE*)message);
            connStat[connection].send_message_size.push(4);
            connStat[connection].send_message_size.push(size);
            connStat[connection].types.push(type);
            connStat[connection].current_sending_message = connStat[connection].send_messages.front();
            int sent = Send_NonBlocking(peers[connection].fd, (BYTE*)connStat[connection].current_sending_message, connStat[connection].send_message_size.front(), &connStat[connection], &peers[connection]);
            if (sent < 0) {
                logTimeStamp();
                printf("Lost connection with user\n");
			    RemoveConnection(connection);
                return -1;
		    }else if (connStat[connection].nSent == connStat[connection].send_message_size.front()){
                connStat[connection].nSent = 0;
                connStat[connection].send_messages.pop();
                connStat[connection].send_message_size.pop();
                connStat[connection].current_sending_message = connStat[connection].send_messages.front();
                int sent_next = Send_NonBlocking(peers[connection].fd, (BYTE*)connStat[connection].current_sending_message, connStat[connection].send_message_size.front(), &connStat[connection], &peers[connection]);
                if (sent_next < 0) {
                    logTimeStamp();
                    printf("Lost connection with user\n");
			        RemoveConnection(connection);
                    return -1;
		        }else if (connStat[connection].nSent == connStat[connection].send_message_size.front()){
                    logTimeStamp();
                    if (type == 0){
                        if (connStat[connection].username != NULL){
                            printf("Finish sending message for user %s\n",connStat[connection].username);
                        }else{
                            printf("Finish sending message for user\n");
                        }
                        
                    }else if (type == 1){
                        printf("Sent file of size %d to %s\n", connStat[connection].send_message_size.front(),connStat[connection].username);
                        connStat[connection].filenames.pop();
                    }else{
                        printf("Sent %d bytes of offline message to %s\n", connStat[connection].send_message_size.front(), connStat[connection].username);
                    }
                    connStat[connection].types.pop();
                    connStat[connection].send_messages.pop();
                    connStat[connection].send_message_size.pop();
                    connStat[connection].nSent = 0;
                    return 1;
                }
                return 0;
            }
    }else{
            //push the message to the queue
            int *netsize_pointer = (int*) malloc(sizeof(int));
            int netsize = htonl(size);
            *netsize_pointer = netsize;
            connStat[connection].send_messages.push((BYTE*)netsize_pointer);
            connStat[connection].send_messages.push((BYTE*)message);
            connStat[connection].send_message_size.push(4);
            connStat[connection].send_message_size.push(size);
            connStat[connection].types.push(type);
            if (type == 1){
                connStat[connection].filenames.push(filename);
            }
            return 0;
        }
    return 0;

}
//Function used to add user to the userlist, this will be invoked whenever a new user register successfully
void addUser(char *username){

    if (num_total_users == size_name_storage){
        //run out of space, need to copy and move
        size_name_storage = size_name_storage*2 + 1;
        USER *new_user_list = (USER*)malloc(sizeof(USER)*size_name_storage);
        for (int i = 0; i < num_total_users; i++){
            new_user_list[i].name = userlist[i].name;
            new_user_list[i].online = userlist[i].online;
        }
        free(userlist);
        userlist = new_user_list;
    }
    //add user information to the list
    num_total_users++;
    userlist[num_total_users-1].name = (char*)malloc(sizeof(char)*MAX_USERNAME_LENGTH);
    strcpy(userlist[num_total_users-1].name, username);
    userlist[num_total_users-1].online = 0;
}
//Function used to send message concurrently to each target users
void *sendMessageParallel(void *arg){
    SEND_MESSAGE_ARG *args = (SEND_MESSAGE_ARG *)arg;
    int connection = args->connection;
    char *message = args->message;
    int size = args->size;
    int type = args->type;
    char *filename = args->filename;
    sendMessage(connection, message, size,type, filename);
    return NULL;
}
//function used to save the sending message concurrently for each offline user
void *writeFileThread(void *arg){
    WRITE_FILE_ARG *args = (WRITE_FILE_ARG *)arg;
    char *filename = args->filename;
    char *message = args->message;
    int size = args->size;
    FILE *storage_file = fopen(filename, "a+");
    fprintf(storage_file, " %d ", size);
    fwrite(message,1,size,storage_file);
    fclose(storage_file);
    logTimeStamp();
    printf(" Finish saving messages for offline user %s\n", filename);
    return NULL;
}

//Function to check if there's any offline user, or if the intended receiver is offline, if they're offline, then save file for them
int checkOffline(char *message, int size, char *target){
    pthread_t threads[num_total_users];
    WRITE_FILE_ARG threads_args[num_total_users];
    int count = 0;
    int valid_target = 0;
    for (int i = 0; i < num_total_users; i++){
        if (target != NULL && userlist[i].online == 0 && strcmp(userlist[i].name,target)==0){
            logTimeStamp();
            printf("Found offline target to write send message to\n");
            valid_target = 1;
            int saved = count;
            count++;
            char *storage_file_name = userlist[i].name;
            threads_args[saved].filename = storage_file_name;
            threads_args[saved].message = message;
            threads_args[saved].size = size;
            pthread_create(&threads[saved],NULL,writeFileThread,&threads_args[saved]);
            break;
        }
        else if(target == NULL && userlist[i].online == 0){
            logTimeStamp();
            printf("Found %s as an offline user, save file for them\n", userlist[i].name);
            char *storage_file_name = userlist[i].name;
            int saved = count;
            count++;
            threads_args[saved].filename = storage_file_name;
            threads_args[saved].message = message;
            threads_args[saved].size = size;
            pthread_create(&threads[saved],NULL,writeFileThread,&threads_args[saved]);
            
        }
    }
    void *res;
    for (int i = 0; i < count; i++){
            int s = pthread_join(threads[i], &res);
            if (s != 0){
                logTimeStamp();
                printf("failed to wait for thread\n");
            }else{
                free(res);
            }

    }
    logTimeStamp();
    printf("All saving offline message threads are done\n");
    return valid_target;

}
//Function used to send all stored offline message for valid users
void sendOfflineMessage(char *user, int connection){
    FILE *offline_storage = fopen(user, "r");
    if (offline_storage == NULL){
        return;
    }
    int size = 0;
    int ret = 0;
    ret = fscanf(offline_storage, " %d ", &size);
    if (ret > 0){
        char *message = (char*)malloc(sizeof(char) *size);
        fread(message,size,1,offline_storage);
        sendMessage(connection,message,size,0,NULL);
        free(message);
        while (ret != 0){
            ret = fscanf(offline_storage, " %d ", &size);
            if (ret == 0 || ret == EOF){
                break;
            }
            message = (char*)malloc(sizeof(char) *size);
            fread(message,size,1,offline_storage);
            sendMessage(connection,message,size,2,NULL);
            free(message);
        }
        logTimeStamp();
        printf(" Finish sending all of the offline message to user %s, now delete the storage\n", user);
        fclose(fopen(user,"w"));
    }

}
//Process incoming request of clients
void process_request(int connection){
    char *temp_message = (char *)connStat[connection].received_message;
    char *message = (char *)malloc(sizeof(char)*connStat[connection].received_message_size);
    int received_message_size = connStat[connection].received_message_size;
    memcpy(message,temp_message,received_message_size);
    reset_receiving_socket(connection,4,0);
    std::string message_string = std::string(message, received_message_size);
    int index = message_string.find(" ");
    std::string command = message_string.substr(0, index);
    std::string body = message_string.substr(index+1, received_message_size-index-1);
    char *body_cstr = (char*)body.c_str();
    logTimeStamp();
    //Registration case
    if (command.compare("REGISTER") == 0){
        printf("Register account: %s\n",body_cstr);
        int split = body.find(" ");
        int l = body.length();
        std::string user_string = body.substr(0, split);
        std::string password_string = body.substr(split+1, l-split-1);
        char *user = (char *)user_string.c_str();
        char *password = (char *)password_string.c_str();
        FILE* user_storage = fopen("storage.txt", "a+");
        char user_info[100];
        int flag = 0;
        char delimiter[] = " ";
        while (fgets(user_info,100,user_storage) != NULL){
            char *name = strtok(user_info,delimiter);
            if (strcmp(name,user)==0){
                logTimeStamp();
                printf("Invalid register, account %s has existed in database\n", user);
                flag = 1;
                break;
            }
        }
        char message[30];
        if (flag){
            strcpy(message, "TEXT_REPLY INVALID_REGISTER");
            fclose(user_storage);
        }else{
            logTimeStamp();
            printf("Valid registration, add user %s to database\n", user);
            strcpy(message, "TEXT_REPLY VALID_REGISTER");
            fprintf(user_storage, "%s %s\n",user,password);
            fclose(user_storage);
            FILE* name_storage = fopen("users.txt", "a+");
            fprintf(name_storage,"%s ",user);
            fclose(name_storage);
            addUser(user);
        }
        int size = strlen(message);
        sendMessage(connection,message,size,0,NULL);
        
    }else if(command.compare("SETUSER") == 0){
        //This is specifically for file channel to acquire its user's owner information
        int name_length = body.length();
        char *name = (char*)malloc(sizeof(char)*name_length);
        char *body_cstr = (char*)body.c_str();
        strcpy(name,body_cstr);
        connStat[connection].username = name;
        connStat[connection].maintain_connection = 1;

    }else if (command.compare("LOGIN") == 0){
        //Login case
        printf("Login account: %s\n", body_cstr);
        int split = body.find(" ");
        int l = body.length();
        std::string user_string = body.substr(0, split);
        std::string password_string = body.substr(split+1, l-split-1);
        char *user = (char *)user_string.c_str();
        char *password = (char *)password_string.c_str();
        char *nl =  strchr(password,'\n');
        if (nl){
            *nl = '\0';
        }
        FILE* user_storage = fopen("storage.txt", "r");
        char user_info[100];
        int flag = 1;
        while (fgets(user_info,100,user_storage) != NULL){
            char delimiter[] = " ";
            char * newline = strchr(user_info,'\n');
            if (newline){
                *newline = '\0';
            }
            char *name = strtok(user_info,delimiter);
            char *stored_password = strtok(NULL, delimiter);
            if (strcmp(name,user)==0 && strcmp(stored_password, password) == 0){
                flag = 0;
                break;
            }
        }
        char message[30];
        int valid = 0;
        fclose(user_storage);
        if (flag){
            printf("Invalid login for user %s\n", user);
            strcpy(message, "TEXT_REPLY INVALID_LOGIN");
        }else{
            connStat[connection].username = (char *)malloc((strlen(user) + 1)*sizeof(char));
            strcpy(connStat[connection].username, user);
            connStat[connection].maintain_connection = 1;
            strcpy(message, "TEXT_REPLY VALID_LOGIN");
            for (int i = 0; i < num_total_users; i++){
                char *user_name = userlist[i].name;
                if (strcmp(user_name,user) == 0){
                    //set the online status to true for the specific user
                    logTimeStamp();
                    printf(" User %s is now online\n", user);
                    userlist[i].online = 1;
                    valid = 1;
                    break;
                }
            }
        }
        int size = strlen(message);
        sendMessage(connection,message,size,0,NULL);
        if (valid){
            sendOfflineMessage(user,connection);
        }
    }else if(command.compare("LOGOUT") == 0){
        //Log out case
        char *username = connStat[connection].username;
        printf("Logout for user: %s\n", username);
        RemoveConnection(connection);
        for (int i=2; i <= nConns+1; i++){
            if (connStat[i].username != NULL && strcmp(username, connStat[i].username) == 0){
                RemoveConnection(i);
                break;
            }
        }
    }else if(command.compare("SEND") == 0 || command.compare("SENDA") == 0){
        //Send anonymous message to everyone
        int username_length = strlen(connStat[connection].username);
        char *username = connStat[connection].username;
        char *message_body = (char*)body.c_str();
        printf("Receive message from %s with content '%s', send to everyone\n",username, message_body);
        int content_length = strlen(message_body);
        char *message;
        int message_size = 0;
        if (command.compare("SEND") == 0){
            message = (char*) malloc(sizeof(char)*(username_length+content_length+15));
            sprintf(message, "TEXT_FORWARD %s: %s",username,message_body);
            message_size = username_length + content_length + 15;
        }else{
            message = (char*) malloc(sizeof(char)*(content_length+24));
            sprintf(message, "TEXT_FORWARD ANONYMOUS: %s",message_body);
            message_size = content_length + 24;
        }
        pthread_t threads[nConns];
        SEND_MESSAGE_ARG threads_args[nConns];
        int thread_count = 0;
        for (int i = 2; i <= nConns + 1; i++){
            if (connStat[i].maintain_connection && connStat[i].username != NULL &&strcmp(connStat[i].username, connStat[connection].username) != 0 && connStat[i].type == 0){
                //send message to each of the receiver here, may be using multi threads here
                threads_args[thread_count].message = message;
                threads_args[thread_count].size = message_size;
                threads_args[thread_count].connection = i;
                threads_args[thread_count].type = 0;
                pthread_create(&threads[thread_count], NULL, sendMessageParallel, &threads_args[thread_count]);
                thread_count++;
            }
        }
        void *res;
        for (int i = 0; i < thread_count; i++){
            int s = pthread_join(threads[i], &res);
            if (s != 0){
                printf("failed to wait for thread\n");
            }else{
                free(res);
            }

        }
        logTimeStamp();
        printf("Check and save messages for offline user\n");
        checkOffline(message, message_size, NULL);
        free(message);
    }else if(command.compare("SEND2") == 0 || command.compare("SENDA2") == 0){
        //Send message specifically to only one target user
        int username_length = strlen(connStat[connection].username);
        char *username = connStat[connection].username;
        int next_sp = body.find(" ");
        std::string target_receiver_str = body.substr(0,next_sp);
        int target_receiver_length = target_receiver_str.length();
        char *target_receiver = (char*)target_receiver_str.c_str();
        int content_length = body.length();
        std::string message_body_str = body.substr(next_sp+1, content_length-next_sp-1);
        int bd_length = message_body_str.length();
        char *message_body = (char*)message_body_str.c_str();
        printf("Receive message from %s with content '%s', send to only user %s\n",username, message_body, target_receiver);
        char *message;
        int message_size = 0;
        if (command.compare("SEND2") == 0){
            message = (char*) malloc(sizeof(char)*(username_length+bd_length+15));
            sprintf(message, "TEXT_FORWARD %s: %s",username,message_body);
            message_size = username_length + bd_length + 15;
        }else{
            message = (char*) malloc(sizeof(char)*(bd_length+24));
            sprintf(message, "TEXT_FORWARD ANONYMOUS: %s",message_body);
            message_size = bd_length + 24;
        }
        int valid = 0;
        for (int i = 2; i <= nConns + 1; i++){
            if (connStat[i].username != NULL && strcmp(connStat[i].username, connStat[connection].username) != 0 && connStat[i].type == 0 && strcmp(connStat[i].username, target_receiver) == 0){
                valid = 1;
                //send message to each of the receiver here, may be using multi threads here
                sendMessage(i,message, message_size,0,NULL);
                break;
            }
        }
        //error invalid user
        if (valid == 0){
            int ret = checkOffline(message,message_size,target_receiver);
            if (ret == 0){
                free(message);
                const char *error_message = "TEXT_REPLY user is invalid";
                int length = strlen(error_message);
                message = (char*)malloc(sizeof(char)*(length + target_receiver_length + 2));
                sprintf(message, "TEXT_REPLY user %s is invalid", target_receiver);
                sendMessage(connection,message,length+target_receiver_length+2,0,NULL);
                free(message);
                logTimeStamp();
                printf("Target user %s is not valid\n",target_receiver);

            }
        }else{
            free(message);
        }

    }
    else if(command.compare("SENDF") == 0){
        //Send file to all user
        int username_length = 0;
        if (connStat[connection].username != NULL){
            username_length = strlen(connStat[connection].username);
        }
        char *username = connStat[connection].username;
        int split = body.find(" ");
        std::string fname = body.substr(0,split);
        char *fname_cstr = (char*)fname.c_str();
        char *fname_str = (char*)malloc(sizeof(char)*(split+1));
        memcpy(fname_str,fname_cstr,split);
        fname_str[split] = '\0';
        char *message_body = (char*)body.c_str();
        int bd_length = received_message_size - index - 1;
        char *message = (char*) malloc(sizeof(char)*(username_length+bd_length+16));
        printf("Receive file from %s with size %d send to everyone\n",username, bd_length);
        //prepare content of message to forward to other users
        const char *type = "FILE_FORWARD ";
        strcpy(message,type);
        char *message_sender = (char *) (message + 13);
        if (username != NULL){
            strcpy(message_sender, username);
        }
        message_sender[username_length] = ':';
        message_sender[username_length+1] = ' ';
        char *message_content = (char *) (message + username_length + 15);
        memset(message_content,'\0', bd_length);
        memcpy(message_content, message_body, bd_length);
        int message_size = username_length + bd_length + 15;
        int message_size_net = htonl(message_size);
        pthread_t threads[nConns];
        SEND_MESSAGE_ARG threads_args[nConns];
        int thread_count = 0;
        //Use multithread to allow concurrent file sending
        for (int i = 2; i <= nConns + 1; i++){
            if (connStat[i].maintain_connection && connStat[i].username != NULL && strcmp(connStat[i].username, connStat[connection].username) != 0 && connStat[i].type == 1){
                //send message to each of the receiver here, may be using multi threads here
                threads_args[thread_count].message = message;
                threads_args[thread_count].size = message_size;
                threads_args[thread_count].connection = i;
                threads_args[thread_count].type = 1;
                threads_args[thread_count].filename = fname_str;
                pthread_create(&threads[thread_count], NULL, sendMessageParallel, &threads_args[thread_count]);
                thread_count++;
            }
        }
        //Wait for all threads to be done
        void *res;
        for (int i = 0; i < thread_count; i++){
            int s = pthread_join(threads[i], &res);
            if (s != 0){
                logTimeStamp();
                printf("failed to wait for thread\n");
            }else{
                free(res);
            }

        }
        checkOffline(message, message_size, NULL);
        free(message);
    }else if(command.compare("SENDF2") == 0){
        //Send file only to a target user
        int next_seperator = body.find(" ");
        std::string receiver_name_str = body.substr(0,next_seperator);
        int target_receiver_length = receiver_name_str.length();
        char *target_receiver = (char*)receiver_name_str.c_str();
        int body_message_length = received_message_size - index - 1;
        body = body.substr(next_seperator+1, body_message_length - next_seperator - 1);
        int split = body.find(" ");
        std::string fname = body.substr(0,split);
        char *fname_cstr = (char*)fname.c_str();
        char *fname_str = (char*)malloc(sizeof(char)*(split+1));
        fname_str[split] = '\0';
        memcpy(fname_str,fname_cstr,split);
        int bd_length = body_message_length - next_seperator - 1;
        char *receiver_name = (char*) receiver_name_str.c_str();
        int username_length = strlen(connStat[connection].username);
        char *username = connStat[connection].username;
        printf("Receive file %s from %s with size %d, send to %s\n",fname_str,username, bd_length, target_receiver);
        //prepare content to send
        char *message_body = (char*)body.c_str();
        int content_length = body_message_length - next_seperator - 1;
        char *message = (char*) malloc(sizeof(char)*(username_length+bd_length+16));
        const char *type = "FILE_FORWARD ";
        strcpy(message,type);
        char *message_sender = (char *) (message + 13);
        strcpy(message_sender, username);
        message_sender[username_length] = ':';
        message_sender[username_length+1] = ' ';
        char *message_content = (char *) (message + username_length + 15);
        memset(message_content,'\0', bd_length);
        memcpy(message_content, message_body, bd_length);
        int message_size = username_length + bd_length + 15;
        int valid = 0;
        //loop through list to find the exact target receiver
        for (int i = 2; i <= nConns + 1; i++){
            if (connStat[i].username != NULL && strcmp(connStat[i].username, connStat[connection].username) != 0 &&connStat[i].type==1 && strcmp(receiver_name,connStat[i].username) == 0 && connStat[i].maintain_connection == 1){
                //found the target receiver
                valid = 1;
                sendMessage(i,message, message_size,1,fname_str);
                break;
            }
        }
        //If can't find target receiver than send back an error to client
        if (valid == 0){
            int ret = checkOffline(message,message_size,receiver_name);
            if (ret == 0){
                free(message);
                const char *error_message = "TEXT_REPLY user is invalid";
                int length = strlen(error_message);
                message = (char*)malloc(sizeof(char)*(length + target_receiver_length + 2));
                sprintf(message, "TEXT_REPLY user %s is invalid", target_receiver);
                sendMessage(connection,message,length + target_receiver_length + 2,0,NULL);
                free(message);
                logTimeStamp();
                printf("Target user %s is invalid\n", target_receiver);
            }else{
                logTimeStamp();
                printf("Target user %s is valid\n", target_receiver);
            }
            
        }else{
            free(message);
        }
        
    }
    else {
        if(command.compare("LIST") == 0){
            printf("User %s requires list of online user\n", connStat[connection].username);
            char *message = (char*)malloc(0);
            message[0] = '\0';
            int num_online_user = 0;
            for (int i = 0; i < num_total_users; i++){
                int current_size = strlen(message);
                //if the user is online;
                if (userlist[i].online == 1){
                    num_online_user++;
                    int user_name_len = strlen(userlist[i].name);
                    int new_size = current_size + 2 + user_name_len;
                    char *saved  = (char*)message;
                    message = (char*)malloc(sizeof(char)*new_size);
                    message[0] = '\0';
                    sprintf(message, "%s %s", saved, (char*)userlist[i].name);
                    free(saved);
                }
            }
            int data_size = strlen(message);
            char *final_message;
            if (num_online_user == 0){
                printf("No online user\n");
                final_message = (char*)malloc(27);
                sprintf(final_message, "TEXT_REPLY No online users\n");
                free(message);
                sendMessage(connection,final_message,27,0,NULL);
            }else{
                final_message = (char*)malloc(data_size + 33);
                final_message[0] = '\0';
                sprintf(final_message, "TEXT_REPLY List of online user:\n%s",message);
                printf("List of online users: %s\n", message);
                free(message);
                sendMessage(connection,final_message,data_size+33,0,NULL);
            }
            
            if (final_message != NULL){
                free(final_message);
            }
            
        }
        
    }
    

}
//Add prior users existing in database to list
void SetUpUserList(){
    logTimeStamp();
    int num_users = 0;
    userlist = (USER*)malloc(sizeof(USER)*2*num_users);
    num_total_users = 0;
    size_name_storage = 0;
    FILE* valid_user_file = fopen("users.txt", "r");
    char *name = (char*) malloc(sizeof(char)*MAX_USERNAME_LENGTH);
    int ret = fscanf(valid_user_file, "%s", name);
    while (ret != EOF){
        addUser(name);
        name = (char*) malloc(sizeof(char)*MAX_USERNAME_LENGTH);
        ret = fscanf(valid_user_file, "%s", name);
    }
    free(name);
    printf("Finish reading in list of users with %d users\n", num_total_users);

}
//Set up listen socket file descriptor for message and file channel
void setUpSocket(int svrPort, int svrPortFile){
    int listenFD = socket(AF_INET, SOCK_STREAM, 0);
	if (listenFD < 0) {
		Error("Cannot create listening socket.");
	}
	SetNonBlockIO(listenFD);
	
	struct sockaddr_in serverAddr;
	memset(&serverAddr, 0, sizeof(struct sockaddr_in));	
	serverAddr.sin_family = AF_INET;
	serverAddr.sin_port = htons((unsigned short) svrPort);
	serverAddr.sin_addr.s_addr = htonl(INADDR_ANY);
	signal(SIGPIPE, SIG_IGN);
	if (bind(listenFD, (struct sockaddr *)&serverAddr, sizeof(serverAddr)) != 0) {
		Error("Cannot bind to port %d.", svrPort);
	}
	
	if (listen(listenFD, 16) != 0) {
		Error("Cannot listen to port %d.", svrPort);
	}
    int listenFD2 = socket(AF_INET, SOCK_STREAM, 0);
	if (listenFD2 < 0) {
		Error("Cannot create listening socket.");
	}
	SetNonBlockIO(listenFD2);
	struct sockaddr_in serverAddrFile;
	memset(&serverAddrFile, 0, sizeof(struct sockaddr_in));	
	serverAddrFile.sin_family = AF_INET;
	serverAddrFile.sin_port = htons((unsigned short) svrPortFile);
	serverAddrFile.sin_addr.s_addr = htonl(INADDR_ANY);
	if (bind(listenFD2, (struct sockaddr *)&serverAddrFile, sizeof(serverAddrFile)) != 0) {
		Error("Cannot bind to port %d.", svrPortFile);
	}
	
	if (listen(listenFD2, 16) != 0) {
		Error("Cannot listen to port %d.", svrPortFile);
	}
    memset(peers, 0, sizeof(peers));	
    peers[0].fd = listenFD;
    peers[0].events = POLLRDNORM;
    peers[0].revents = 0;
	peers[1].fd = listenFD2;
    peers[1].events = POLLRDNORM;
    peers[1].revents = 0;

}
//Function that will be in charge of listening for upcoming connections of clients
void DoServer(int svrPort, int svrPortFile) {	
	int connID = 0;
    setUpSocket(svrPort, svrPortFile);
    //Add previous users to list
    SetUpUserList();
	while (1) {	//the main loop		
		//monitor the listening sock and data socks, nConn+1 in total
		int r = poll(peers, nConns + 2, -1);
		if (r < 0) {
			Error("Invalid poll() return value.");
		}			
				
		//new incoming connection, 0 for message channel, 1 for file channel
        for (int i = 0; i < 2; i++){
            struct sockaddr_in clientAddr;
		    socklen_t clientAddrLen = sizeof(clientAddr);
            if ((peers[i].revents & POLLRDNORM ) && (nConns < MAX_CONCURRENCY_LIMIT)) {	
                logTimeStamp();
                printf(" Have new incoming client connection\n");				
                int fd = accept(peers[i].fd, (struct sockaddr *)&clientAddr, &clientAddrLen);
                int address = clientAddr.sin_addr.s_addr;                
                if (fd != -1) {
                    SetNonBlockIO(fd);
                    nConns++;
                    peers[nConns+1].fd = fd;
                    peers[nConns+1].events = POLLRDNORM;
                    peers[nConns+1].revents = 0;
                    connStat[nConns+1].type = i;
                    connStat[nConns+1].address = address;
                    int s = connStat[nConns+1].send_messages.size();
                    reset_receiving_socket(nConns+1,4,0);
                }
                if (fd == -1){
                    printf("Error num %d\n", errno);
                    Error("Client socket close\n");
                }
            }
        }
		
		for (int i=2; i<=nConns+1; i++) {
            if ((peers[i].revents & POLLERR) || (peers[i].revents & POLLHUP)){
                logTimeStamp();
                printf("Connection closed\n");
                RemoveConnection(i);
                continue;
            }
			if (peers[i].revents & POLLRDNORM ){
				int fd = peers[i].fd;
				//recv request
				if (connStat[i].nRecv < connStat[i].received_message_size) {
                    //printf("started to received for connection %d\n", i);
                    if (Recv_NonBlocking(fd, connStat[i].received_message, connStat[i].received_message_size, &connStat[i], &peers[i]) < 0) {
						RemoveConnection(i);
						continue;
					}
					if (connStat[i].nRecv == connStat[i].received_message_size) {
                        if (connStat[i].receiving_stage == 0){//need user command
                            int *incoming_message_size = (int *)connStat[i].received_message;
                            int message_size = ntohl(*incoming_message_size);
                            reset_receiving_socket(i, message_size, 1);
                        }else if(connStat[i].receiving_stage == 1){
                            //finish receiving all messages here
                            connStat[i].receiving_stage = 2;
                        }
					}
				}
			
				//send response
                if (connStat[i].receiving_stage == 2){
                   //reset_receiving_socket(nConns,4,0);
                    process_request(i);
                }
			}
			
            //check if can continue to write
            check_continue_write(i);
           
		}
	}	
}

void cleanup(){
    printf("Clean up storage file\n");
        FILE* valid_user_file = fopen("users.txt", "r");
        char *name = (char*) malloc(sizeof(char)*MAX_USERNAME_LENGTH);
        int ret = fscanf(valid_user_file, "%s", name);
        while (ret != EOF){
            fclose(fopen(name,"w"));
            ret = fscanf(valid_user_file, "%s", name);
        }
        fclose(valid_user_file);
        fclose(fopen("storage.txt", "w"));
        fclose(fopen("users.txt","w"));

}
int main(int argc, char * * argv) {	
    if (argc == 2){
        char * option = argv[1];
        if (strcmp(option, "reset") == 0){
            cleanup();
        }
        return 0;
    }else if (argc != 3) {
		Log("Usage: %s [server Port message] [server Port file]", argv[0]);
		return -1;
	}
	
	int port = atoi(argv[1]);
    int port1 = atoi(argv[2]);
	DoServer(port, port1);
	return 0;
}