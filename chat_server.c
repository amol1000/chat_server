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


#define DEFAULT_PORT (1234)
#define TRIE_MAX_CHILD (128)
#define HOSTLEN (256)
#define SERVLEN (8)

// since strnlen is used if ret val is max_len + 1 then send error
#define MAX_BUFF_LEN (20001)
#define MAX_USERNAME_LEN (21)
#define MAX_ROOMNAME_LEN (21)

#define MSG_DELIMETER ('\n')
#define INIT_ARR_CAP (1000)

/**
 * @brief resizeable array which doubles when full
 * 
 */
typedef struct resizeable_array{
    int *data;
    int size;
    int cap;
}rs_array_t;

typedef struct user{
    int connfd;
    char* user_name;
    char* room_name;
}user_t;

typedef struct ChatRoom{
    char* room_name;
    int num_people;
    rs_array_t* user_fds;
    pthread_mutex_t lock;
}chat_room_t;

/**
 * @brief trie node, each node contains one character and 26 child pointers.
 * Also contains a pointer to a room.
 * 
 */
//@{
typedef struct TrieNode{
    char ch;
    bool is_word;
    struct TrieNode* child[TRIE_MAX_CHILD];
    chat_room_t* room;
}trie_node_t;
//@}

static trie_node_t *trie_root;
static pthread_mutex_t trie_lock;

// static const char delim_buff[] = "\n";
static const char hello_buff[] = "Hello from server\n";
static const char error_buff[] = "ERROR\n";
static const char join_buff[] = "has joined\n";
static const char left_buff[] = "has left\n";

void usage()
{
    printf(" Usage: ./chat_server <optional-port-number>");
}


void terminate()
{
    //broadcast closing to all and close connections, free resources and exit
    char end_buff[] = "broadcast closing to all and close connections, free resources and exit";
    write(STDOUT_FILENO, end_buff, strlen(end_buff));
    exit(-1);
}


/**
 * @brief initialize a trie node
 * 
 * @return trie_node* pointer to the new trie node
 */
static trie_node_t* init_trie_node()
{
    trie_node_t* temp = (trie_node_t*)calloc(1, sizeof(trie_node_t));

    for(int i = 0; i < TRIE_MAX_CHILD; i++){
        temp->child[i] = NULL;
    }

    temp->is_word = false;
    temp->room = NULL;

    return temp;
}

/**
 * @brief initialize the resizeable array
 * 
 * @return rs_array* pointer to array
 */
static rs_array_t* init_rs_array()
{
    rs_array_t* temp_rs = (rs_array_t*)malloc(sizeof(rs_array_t));

    if(!temp_rs){
        return NULL;
    }

    temp_rs->data = (int*)malloc(INIT_ARR_CAP*sizeof(int));

    if(!(temp_rs->data)){
        return NULL;
    }

    temp_rs->cap = INIT_ARR_CAP;
    temp_rs->size = 0;

    return temp_rs;
}


/**
 * @brief inserts the given line number in the array, double the array if it is
 *         full
 * 
 * @param rs resizeable array struct
 * @param user_fd file descritpor for user connection
 * 
 * @return int 0 on success, negative on error
 */
static int insert_into_rs_array(rs_array_t** rs, int user_fd)
{
    if(!rs || !*rs){
        return -EINVAL;
    }

    int size = (*rs)->size;
    // if line number is already inserted then do not insert again
    if((*rs)->data[size-1] == user_fd){
        return -EINVAL;
    }

    if((*rs)->size == ((*rs)->cap)){
        //array mem full. add more. (double it!)
        (*rs)->cap = 2*(*rs)->cap;
        (*rs)->data = realloc((*rs)->data, sizeof(int)*(*rs)->cap);
        if(!((*rs)->data)){
            return -ENOMEM;
        }
    }

    (*rs)->data[size++] = user_fd;
    (*rs)->size = size;


    return 0;
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

    int ret;
    if ((ret = sscanf(buff, "JOIN %s %s", room_name, user_name)) != 2) {
        printf(" sscanf returned %d\n", ret);
        return -1;
    }

    printf("sscanf returned %d\n", ret);
    int user_name_len = strnlen(user_name, MAX_USERNAME_LEN);
    int room_name_len = strnlen(room_name, MAX_ROOMNAME_LEN);

    if(user_name_len == MAX_USERNAME_LEN || room_name_len == MAX_USERNAME_LEN){
        return -1;
    }

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


static chat_room_t* search_room(const char* room_name)
{
    trie_node_t* itr = trie_root;

    for(int i = 0; i < strnlen(room_name, MAX_ROOMNAME_LEN); i++){

        int j = toascii(room_name[i]);

        if(!(itr->child[j])){
            return NULL;
        }

        itr = itr->child[j];
    }

    if(itr->room){
        return itr->room;
    }

    return NULL;
}

static chat_room_t* create_room(const char* room_name){

    trie_node_t* itr = trie_root;

    if(!room_name || strchr(room_name, MSG_DELIMETER) || strchr(room_name, ' ')){
        return NULL;
    }

    for(int i = 0; i < strnlen(room_name, MAX_ROOMNAME_LEN); i++){

        // char lower_ch = tolower(room_name[i]);

        int j = toascii(room_name[i]);

        if(!(itr->child[j])){

            // printf("char is %c, creating trie node", lower_ch);
            //new char
            trie_node_t* new_node = init_trie_node();

            if(!new_node){
                printf("no memory for trie node\n");
                return NULL;
            }

            // new_node->ch = lower_ch;

            itr->child[j] = new_node;

        }
        //if node exists go down, if it doesn't then one new node has been
        //just created
        if(!(itr->child[j])){
            printf("child is empty");
            exit(-1);
        }
        itr = itr->child[j];
    }


    if(!itr){
        printf("error");
        exit(-1);
    }

    itr->room = (chat_room_t*)malloc(sizeof(chat_room_t));

    if(!itr->room){
        printf("Malloc failed for create room");
        return NULL;
    }

    itr->room->user_fds = init_rs_array();

    if(!(itr->room->user_fds)){
        printf("No memory for fds\n");
        return NULL;
    }
    itr->room->num_people = 0;

    int err;
    if ((err = pthread_mutex_init(&itr->room->lock, NULL)) != 0) { 
        printf(" mutex init failed for trie: %s\n", strerror(err));
        exit(-err);
    }

    printf("room %s created\n", room_name);

    return itr->room;
}

/**
 * @brief robustly reads n bytes from a network fd to buffer
 * 
 * -> avoids the short count situation in case of network sockets
 * 
 * @param fd 
 * @param buf 
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
static char* read_wrapper(int fd, void* buff, bool init, user_t* user_info, 
                            size_t* packet_size)
{
    size_t remainder = MAX_BUFF_LEN;
    ssize_t n;
    char* start_ptr = buff;

    while (remainder > 0) {

        if ((n = read(fd, buff, remainder)) < 0) {
            perror("Error in read_wrapper:");
            return NULL;
        }

        printf("From client: %s of %ld\n", (char*)buff, n);
        if(n == 0){
            printf("socket closed?\n");
            return NULL;
        }

        char* pos;
        if((pos = strchr(start_ptr, MSG_DELIMETER)) != NULL) {

            if(init){
                if(validate_join(start_ptr, user_info) < 0) {
                    return NULL;
                }

                // printf("user %s wants to join %s\n", user_info->user_name, 
                //         user_info->room_name);

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
            return -errno;
        }
    }

    if((err = pthread_mutex_unlock(&room->lock)) != 0){
        printf("Error unlocking room mutex : %s", strerror(err));
        return -err;
    }
}

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
        for(int j = i; j < room->num_people; j++){
            room->user_fds->data[j] = room->user_fds->data[j+1];
        }
    } else {
        return -ENOENT;
    }

    room->user_fds->size--;
    room->num_people--;

    if(room->num_people == 0){
        printf("delete room\n");
    }

    if((err = pthread_mutex_unlock(&room->lock)) != 0){
        printf("Error unlocking room mutex : %s", strerror(err));
        return -err;
    }

    if(close(user_info->connfd) < 0){
        perror("Error closing user fd");
        return -errno;
    }
    free(user_info->room_name);
    free(user_info->user_name);
    free(user_info);

    return 0;
}

void *client_serve(void* arg)
{
    int err;
    if(pthread_detach(pthread_self()) != 0){
        printf("error detaching");
        exit(-1);
    }

    size_t packet_size = 0;
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
                                             user_info, &packet_size)) == NULL){

            if(!new_request) {
                printf("removing user from room\n");

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
                printf("error in new client req, closing connection\n");
                if(client_error(*clientfd) < 0){
                    printf("Irony: Error sending error msg to client\n");
                }
            }
            return NULL;
        }

        if(new_request){
            //search if room already exists else create it
            // printf("serving new req\n");

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
                printf("room not found, creating\n");

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

            // printf("added fd to array\n");

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
        printf("broadcasting msg %s\n",in_buff);
        while(packet_start != NULL){

            printf("sending %s to users in room %s len is %ld\n", packet_start,
                    user_info->room_name, strnlen(packet_start, MAX_BUFF_LEN));

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

    // Binding newly created socket to given IP and verification 
    if ((bind(serverfd, (struct sockaddr*)server, 
                sizeof(struct sockaddr))) != 0) { 
        perror("socket bind failed"); 
        exit(-errno);
    } else {
        printf("Socket successfully binded\n");
    }

    if(listen(serverfd, 10) < 0){
        perror("socket bind failed"); 
        exit(-errno);
    }

    int err;
    if ((err = pthread_mutex_init(&trie_lock, NULL)) != 0) { 
        printf(" mutex init failed for trie: %s\n", strerror(err));
        exit(-err);
    }

    trie_root = init_trie_node();
    if(!trie_root){
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
        } else {
            perror(" error in accept ");
            exit(-errno);
        }

        printf("accepting connection on %d", port);

    }

    return 0;
}