/**
 * @file chat_server.c
 * @author Amol(amolkulk@andrew.cmu.edu)
 * @brief chat server contains chat rooms whihc users can join.
 * 
 *-> multithreaded approach, spawns new thread for each connection.
 *-> each new thread uses local stack variables other than trie and room, access
     to which is synced by a mutex lock
 * -> @see utils.c for more about trie and rooms
 * 
 * Flow:
 * -> each thread waits for a successful read on a socket.
 * -> Once it gets a join request, it validates it.
 * -> THe room name is traversed in the trie to check if room already exists
 * -> If room does not exist, new room is created.
 * -> Connection fd is stored in the room struct.
 * -> Everytime a new message is received, all users in that room get the message.
 * 
 * 
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <stdbool.h>
#include <inttypes.h>
#include <unistd.h>
#include <assert.h>

#include <pthread.h>
#include <signal.h>
#include <errno.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>

#include "utils.h"

#define DEFAULT_PORT (1234)
#define HOSTLEN (256)
#define SERVLEN (8)

typedef struct user{
    int connfd;
    char* user_name;
    char* room_name;
}user_t;

/**lock for access to trie APIs*/
static pthread_mutex_t trie_lock;

/** strings for standard entry and exit process */
static const char error_buff[] = "ERROR\n";
static const char join_buff[] = "has joined\n";
static const char left_buff[] = "has left\n";

void usage()
{
    printf(" Usage: ./chat_server <optional-port-number>");
}

void terminate()
{
    destroy_trie();
    exit(-1);
}

static int validate_join(const char* buff, user_t *user_info)
{
    if(!user_info){
        printf("inval arg\n");
        return -EINVAL;
    }

    char user_name[MAX_USERNAME_LEN];
    memset(user_name, 0 , MAX_USERNAME_LEN);

    char room_name[MAX_ROOMNAME_LEN];
    memset(room_name, 0 , MAX_ROOMNAME_LEN);

    char join_str[JOIN_STR_LEN];
    memset(join_str, 0 , JOIN_STR_LEN);

    int ret;

    if ((ret = sscanf(buff, "%s %s %s[^\n]", join_str, room_name, user_name)) != 3) {
        printf(" sscanf returned %d\n", ret);
        return -1;
    }

    int join_str_len  = strnlen(join_str, JOIN_STR_LEN);
    int user_name_len = strnlen(user_name, MAX_USERNAME_LEN);
    int room_name_len = strnlen(room_name, MAX_ROOMNAME_LEN);

    if(user_name_len == MAX_USERNAME_LEN || room_name_len == MAX_USERNAME_LEN
        || join_str_len == JOIN_STR_LEN){
        printf("malformed request\n");
        return -1;
    }

    if(strncasecmp(join_str, "join", JOIN_STR_LEN) != 0){
        printf("malformed request\n");
        return -1;
    }

    printf("user is %s and room is %s\n", user_name, room_name);

    user_info->user_name = (char*)malloc(user_name_len+1*sizeof(char));

    if(!(user_info->user_name)){
        perror(" Malloc failed ");
        return -errno;
    }

    strncpy(user_info->user_name, user_name, user_name_len);
    user_info->user_name[user_name_len] = '\0';


    user_info->room_name = (char*)malloc(room_name_len+1*sizeof(char));

    if(!(user_info->room_name)){
        perror(" Malloc failed");
        return -errno;
    }

    strncpy(user_info->room_name, room_name, room_name_len);
    user_info->room_name[room_name_len] = '\0';

    return 0;
}

/**
 * @brief robustly reads n bytes from a network fd to buffer
 * 
 * -> avoids the short count situation in case of network sockets
 * 
 * @param fd connection file descriptor
 * @param buf buffer to be read into
 * @param init hunts for the join request, validates join command if true
 * @param user_info if it is a join request then user name and room name is
 *                  added to this struct. If init is false user_info is NULL
 *
 * @return int start index of actual messages or negative on error
 * 
 * -> When packet is JOIN cooking amol<NL>hello<NL>Whats up<NL> 
 * -> Return value will be postion of hello.
 * -> When packet is JOIN cooking amol<NL>, retrn value will point after <NL>
 */
static char* read_wrapper(int fd, void* buff, bool init, user_t* user_info)
{
    size_t remainder = MAX_BUFF_LEN;
    ssize_t n;
    char* start_ptr = buff;

    while (remainder > 0) {

        if ((n = read(fd, buff, remainder)) < 0) {
            perror("Error in read_wrapper:");
            return NULL;
        }

        if(n == 0){
            printf("socket closed?\n");
            return NULL;
        }

        char* pos;
        if((pos = strchr(start_ptr, MSG_DELIMETER)) != NULL) {

            if(init){
                if(validate_join(start_ptr, user_info) < 0) {
                    printf("Malformed join req\n");
                    return NULL;
                }

                init = false;// should not validate again
                user_info->connfd = fd;

                return (char*)(pos+1); // since pos points to newline
            }

            return start_ptr;
        }
        remainder -= n;
        buff += n;
    }

    return NULL;
}

static int client_error(int fd)
{
    if(write(fd, error_buff, strlen(error_buff)) < 0){
        perror("error in write");
        return -errno;
    }

    if(close(fd) < 0){
        perror("error in close");
        return -errno;
    }

    return 0;
}

static int broadcast_msg(chat_room_t* room, const char* msg)
{
    int err;

    if((err = pthread_mutex_lock(&room->lock)) != 0){
        printf("Error locking room mutex : %s", strerror(err));

        return -err;
    }

    for(int i = 0; i < room->num_people; i++){

        if((err = write(room->user_fds->data[i], msg, 
                    strnlen(msg, MAX_BUFF_LEN)))
                    == -1){

            perror("Error in write");
            if((err = pthread_mutex_unlock(&room->lock)) != 0){
                printf("Error unlocking room mutex : %s", strerror(err));
                return -err;
            }
            return -errno;
        }
    }

    if((err = pthread_mutex_unlock(&room->lock)) != 0){
        printf("Error unlocking room mutex : %s", strerror(err));
        return -err;
    }

    return 0;
}

/**
 * @brief removes the given user from the room
 * 
 * -> if the user is the last user in the room, the room is deleted
 * 
 * @param user_info user name and room name
 * @param room room struct
 * @return int 0 on success and negative on error
 * 
 */
static int remove_user(user_t* user_info, chat_room_t* room)
{
    int err;
    if((err = pthread_mutex_lock(&room->lock)) != 0){
        printf("Error locking room mutex : %s", strerror(err));

        return -err;
    }
    int i;
    for(i = 0; i < room->num_people; i++){
        if(room->user_fds->data[i] == user_info->connfd){
            break;
        }
    }

    if(i < room->num_people){
        for(int j = i; j < room->num_people-1; j++){
            room->user_fds->data[j] = room->user_fds->data[j+1];
        }
    } else {
        if((err = pthread_mutex_unlock(&room->lock)) != 0){
            printf("Error unlocking room mutex : %s", strerror(err));
            return -err;
        }
        return -ENOENT;
    }

    room->user_fds->size--;
    room->num_people--;

    if(room->num_people == 0){
        if(delete_room(room) < 0){
            printf("Error in deleting room\n");
            return -1;
        }
    }

    if((err = pthread_mutex_unlock(&room->lock)) != 0){
        printf("Error unlocking room mutex : %s", strerror(err));
        return -err;
    }

    free(user_info->room_name);
    free(user_info->user_name);
    free(user_info);

    return 0;
}


/**
 * @brief Each connection spawns a new thread and then executes this function
 *      this function
 * 
 * @param arg contains the argument, connectionfd in this case
 * @return void* returns NULL only on error.
 * 
 */
void *client_serve(void* arg)
{
    int err;
    if(pthread_detach(pthread_self()) != 0){
        printf("error detaching");
        exit(-1);
    }

    char* packet_start = NULL;

    char in_buff[MAX_BUFF_LEN];

    char out_buff[MAX_BUFF_LEN];

    int *clientfd = (int*)arg;

    bool new_request = true;

    user_t *user_info = (user_t*)malloc(sizeof(user_t));
    chat_room_t *room;

    if(!user_info){
        printf(" Out of Memory\n");
        return NULL;
    }
    while(1) {

        memset(in_buff, 0, MAX_BUFF_LEN);
        memset(out_buff, 0, MAX_BUFF_LEN);

        if((packet_start = read_wrapper(*clientfd, in_buff, new_request,
                                             user_info)) == NULL){

            if(!new_request) {

                char user_name_temp[MAX_USERNAME_LEN];
                memset(user_name_temp, 0, MAX_USERNAME_LEN);

                strncpy(user_name_temp, user_info->user_name, MAX_USERNAME_LEN);

                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }

                if(remove_user(user_info, room) < 0){
                    printf("Terminal Irony: error removing user\n");
                }

                snprintf(out_buff, MAX_BUFF_LEN, "%s %s\n", user_name_temp,
                            left_buff);

                if(broadcast_msg(room, out_buff) < 0){
                    printf("Terminal Irony: Unable to tell other users that %s"\
                            " left\n", user_name_temp);
                }

            } else {
                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
            }
            return NULL;
        }

        if(new_request){
            //search if room already exists else create it

            if((err = pthread_mutex_lock(&trie_lock)) != 0){
                printf("Error locking trie mutex : %s", strerror(err));
                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                free(user_info);
                return NULL;
            }

            room = search_room(user_info->room_name);

            if((err = pthread_mutex_unlock(&trie_lock)) != 0){
                printf("Error unlocking trie mutex : %s", strerror(err));
                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                free(user_info);
                return NULL;
            }

            if(!room){

                if((err = pthread_mutex_lock(&trie_lock)) != 0){
                    printf("Error locking trie mutex : %s", strerror(err));
                    if(client_error(*clientfd) < 0){
                        printf("Irony: Error sending error msg to client\n");
                    }
                    free(user_info);
                    return NULL;
                }

                room = create_room(user_info->room_name);

                if(!room){
                    if(remove_user(user_info, room) < 0){
                        printf("Terminal Irony: error removing user\n");
                    }
                    if(client_error(*clientfd) < 0){
                        printf("Irony: Error sending error msg to client\n");
                    }

                    if((err = pthread_mutex_unlock(&trie_lock)) != 0){
                        printf("Error unlocking trie mutex : %s", strerror(err));
                    }

                    return NULL;
                }

                if((err = pthread_mutex_unlock(&trie_lock)) != 0){
                    printf("Error unlocking trie mutex : %s", strerror(err));
                    if(client_error(*clientfd) < 0){
                        printf("Irony: Error sending error msg to client\n");
                    }
                    if(remove_user(user_info, room) < 0){
                        printf("Terminal Irony: error removing user\n");
                    }
                    return NULL;
                }
            }

            //lock room
            if((err = pthread_mutex_lock(&room->lock)) != 0){
                printf("Error locking room mutex : %s", strerror(err));
                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                if(remove_user(user_info, room) < 0){
                    printf("Terminal Irony: error removing user\n");
                }
                return NULL;
            }

            if(insert_into_rs_array(&room->user_fds, *clientfd) < 0){
                printf("Error adding user fd\n");
                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                if(remove_user(user_info, room) < 0){
                    printf("Terminal Irony: error removing user\n");
                }
                if((err = pthread_mutex_unlock(&room->lock)) != 0){
                    printf("Error unlocking trie mutex : %s", strerror(err));
                }
                return NULL;
            }

            room->num_people++;

            if((err = pthread_mutex_unlock(&room->lock)) != 0){
                printf("Error unlocking trie mutex : %s", strerror(err));

                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                if(remove_user(user_info, room) < 0){
                    printf("Terminal Irony: error removing user\n");
                }
                return NULL;
            }

            snprintf(out_buff, MAX_BUFF_LEN, "%s %s\n", user_info->user_name,
                        join_buff);

            if(broadcast_msg(room, out_buff) < 0){

                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                if(remove_user(user_info, room) < 0){
                    printf("Terminal Irony: error removing user\n");
                }
                return NULL;
            }

            new_request = false;// user has been added so not a new req anymore
                                // userful in case of merged packets.
        }

        packet_start = strtok(packet_start, "\n");
        while(packet_start != NULL){

            // printf("sending %s to users in room %s len is %ld\n", packet_start,
            //         user_info->room_name, strnlen(packet_start, MAX_BUFF_LEN));

            snprintf(out_buff, MAX_BUFF_LEN, "%s: %s\n", user_info->user_name,
                        packet_start);


            if(broadcast_msg(room, out_buff) < 0){

                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
                if(remove_user(user_info, room) < 0){
                    printf("Terminal Irony: error removing user\n");
                }
                return NULL;
            }
            packet_start = strtok(NULL, "\n");
        }
    }
}

int main(int argc, char *argv[])
{
    if(argc > 2){
        usage();
    }

    int port = DEFAULT_PORT;
    if(argc == 2){
        port = atoi(argv[1]);
    }

    int serverfd, client_addrlen;
    struct sockaddr_in *server, *client;

    server = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));
    if(!server){
        perror("Malloc failed:");
        exit(-errno);
    }

    server->sin_addr.s_addr = htonl(INADDR_ANY);
    server->sin_port = htons(port);
    server->sin_family = AF_INET;

    // socket create and verification 
    serverfd = socket(AF_INET, SOCK_STREAM, 0); 
    if (serverfd == -1) { 
        perror("socket creation failed: ");
        exit(-errno);
    } else {
        printf(" Server Socket successfully created\n");
    }

    if ((bind(serverfd, (struct sockaddr*)server, 
                sizeof(struct sockaddr))) != 0) { 
        perror("socket bind failed"); 
        exit(-errno);
    } else {
        printf("Socket successfully binded\n");
    }

    //1000 connections can be pending
    if(listen(serverfd, 1000) < 0){
        perror("socket bind failed"); 
        exit(-errno);
    }

    int err;
    if ((err = pthread_mutex_init(&trie_lock, NULL)) != 0) { 
        printf(" mutex init failed for trie: %s\n", strerror(err));
        exit(-err);
    }

    if(!init_trie()){
        printf("Out of memory for trie");
        exit(-ENOMEM);
    }

    /* Sigpipe should be ignored*/ 
    signal(SIGPIPE, SIG_IGN);
    signal(SIGABRT, terminate);
    signal(SIGTERM, terminate);
    signal(SIGINT, terminate);

    while(true){

        client = (struct sockaddr_in*)malloc(sizeof(struct sockaddr_in));

        int *clientfd = (int*)malloc(sizeof(int));

        if(!client || !clientfd){

            perror("Malloc failed:");

            /* in case of terminal irony */
            if((err = pthread_mutex_destroy(&trie_lock)) < 0){
                printf(" mutex destroy failed for trie: %s\n", strerror(err));
                exit(-err);
            }
            if(close(serverfd) < 0){
                perror("error closing server fd");
                exit(-errno);
            }
            exit(-errno);
        }

        client_addrlen = sizeof(struct sockaddr);

        *clientfd = accept(serverfd, (struct sockaddr *)client, 
                            &client_addrlen);
        /* if connection was successful then spawn a thread and 
        * let it handle the client else we wait for a new one again*/
        if(*clientfd > 0){
            pthread_t thread;
            pthread_create(&thread, NULL, client_serve, (void*)clientfd);
        }

    }

    return 0;
}