#ifndef __UTILS_H
#define __UTILS_H

#include <pthread.h>

#define TRIE_MAX_CHILD (128)
// since strnlen is used if ret val is max_len + 1 then send error
#define MAX_BUFF_LEN (20001)
#define MAX_USERNAME_LEN (21)
#define MAX_ROOMNAME_LEN (21)
#define JOIN_STR_LEN (5)

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


chat_room_t* create_room(const char* room_name);
int delete_room(chat_room_t* room);
chat_room_t* search_room(const char* room_name);
int remove_from_trie(char* room_name, trie_node_t* itr, int len, int i);
int init_trie();
void destroy_trie();
int insert_into_rs_array(rs_array_t** rs, int user_fd);

#endif