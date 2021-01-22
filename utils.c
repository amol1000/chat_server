/**
 * @file utils.c
 * @author Amol(amolkulk@andrew.cmu.edu)
 * @brief this file contains the trie and chat_room APIs
 * 
 * -> The trie supports all 128 ascii characters, (except newline and space)
 * -> each trie node contains a pointer to a room struct if it is the end of the
 *     word(eg. for room name "cooking" the 'g' node will contain ptr to room)
 * -> Each room contains a resizeable array of user fds.
 * 
 * 
 * 
 * Design choice: Following options were considered for storing room names
 * 1) Linked List: Everytime for a neq request will have to travers o(n). In 
 *                  case of lots of rooms, unique node for each room. Easy to 
 *                  implement
 * 2) Trie: Traversal only for string length.O(1). More scalable when there are 
 *          a lot of rooms. since cook and cooks will have overlapping memory.
 *          Not so difficult to implement.
 * 3) unordered Map: pure O(1) but will have to take care of collisions. Complex
 *                  to implement
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
#include <errno.h>

#include "utils.h"


static trie_node_t *trie_root;

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


static void clear_trie(trie_node_t* root)
{
    if(!root){
        return;
    }

    for(int i = 0; i < TRIE_MAX_CHILD; i++){
        clear_trie(root->child[i]);
    }

    if(root->is_word){
        if(root->room){
            free(root->room->room_name);
            pthread_mutex_destroy(&root->room->lock);
            free(root->room);
        }
    }

    free(root);
}

/**
 * @brief free the memory used by the trie struct
 * 
 */
void destroy_trie()
{
    clear_trie(trie_root);
}

int init_trie()
{
    if((trie_root = init_trie_node()) == NULL){
        return 0;
    }

    return 1;
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
 * @brief inserts the given fd in the array, double the array if it is
 *         full
 * 
 * @param rs resizeable array struct
 * @param user_fd file descritpor for user connection
 * 
 * @return int 0 on success, negative on error
 */
int insert_into_rs_array(rs_array_t** rs, int user_fd)
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

/**
 * @brief checks if trie node is the leaf or not
 * 
 * @param root node to be checked for leaf condition
 * 
 * @return true if leaf
 * @return false 
 */
static bool is_leaf(trie_node_t* root)
{
    for(int i = 0; i < TRIE_MAX_CHILD; i++){
        if(root->child[i]){
            return false;
        }
    }

    return true;
}


/**
 * @brief removes the given room from trie
 * 
 * -> deletes the node only if it is the leaf node.
 * 
 * @param room_name
 * @param itr trie node used for recursive iteration
 * @param len len of room name
 * @param i current postion in string
 * 
 * @return int 1 if matched and deleted 0 otherwise
 */
int remove_from_trie(char* room_name, trie_node_t* itr, int len, int i)
{
    if(!itr){
        return 0;
    }

    if(!(itr->child[toascii(room_name[i])])){
        return 0;

    } else {

        trie_node_t* child = itr->child[toascii(room_name[i])];
        if(child->is_word && i == (len -1)){

            if(is_leaf(child)){
                free(child);
                itr->child[toascii(room_name[i])] = NULL;
                child->room = NULL;
                return 1;
            } else {
                child->is_word = false;
                child->room = NULL;
                return 0;
            }
        }
    }

    itr = itr->child[toascii(room_name[i])];

    int should_delete = remove_from_trie(room_name, itr, len, i+1);

    if(should_delete){

        if(!(itr->is_word) && trie_root != itr && is_leaf(itr)){
            free(itr);
            return 1;
        }
    }

    return 0;
}

/**
 * @brief deletes the given room, free the memeory and remove it from trie if
 *         possible
 * 
 * @param room 
 * @return int 0 on success negative on error
 */
int delete_room(chat_room_t* room)
{
    if(!room || room->num_people != 0){
        printf("Error deleting room\n");
        return -EINVAL;
    }

    remove_from_trie(room->room_name, trie_root, 
                            strlen(room->room_name), 0);

    if(pthread_mutex_destroy(&room->lock) < 0){
        printf("Error destroying room mutex\n");
        return -1;
    }

    free(room->room_name);
    free(room);
    
    return 0;
}


/**
 * @brief Create a room struct, traverses the trie until node to be created is 
 *          found.
 * -> Room pointer is addded to the last character node(derived from name)
 * 
 * @param room_name 
 * @return chat_room_t* 
 */
chat_room_t* create_room(const char* room_name)
{
    trie_node_t* itr = trie_root;

    if(!room_name || strchr(room_name, MSG_DELIMETER) || strchr(room_name, ' ')){
        return NULL;
    }

    int i = 0;
    int j = 0;
    while(i < strnlen(room_name, MAX_ROOMNAME_LEN)){

        j = toascii(room_name[i]);

        if(!(itr->child[j])){

            trie_node_t* new_node = init_trie_node();

            if(!new_node){
                printf("no memory for trie node\n");
                return NULL;
            }

            itr->child[j] = new_node;

        }

        if(i < strnlen(room_name, MAX_ROOMNAME_LEN)){
            itr = itr->child[j];
            i++;
        }
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

    itr->room->room_name = (char*)malloc(sizeof(char)*(strlen(room_name)+1));

    if(!(itr->room->room_name)){
        printf("Malloc failed for create room");
        return NULL;
    }

    strncpy(itr->room->room_name, room_name, strlen(room_name));

    itr->room->room_name[strlen(room_name)] = '\0';

    itr->is_word = true;

    return itr->room;
}


/**
 * @brief search the given room name to see if room already exists in trie
 * 
 * @param room_name 
 * @return chat_room_t* NUll if not found
 */
chat_room_t* search_room(const char* room_name)
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