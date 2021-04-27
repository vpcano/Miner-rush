#include <unistd.h>
#include <semaphore.h>

#define OK 0
#define MAX_WORKERS 10

#define SHM_NAME_NET "/netdata"
#define SHM_NAME_BLOCK "/block"

#define MAX_MINERS 200

typedef struct _Block {
    int wallets[MAX_MINERS];
    long int target;
    long int solution;
    int id;
    int is_valid;
    struct _Block *next;
    struct _Block *prev;
} Block;

typedef struct _NetData {
    pid_t miners_pid[MAX_MINERS];
    char voting_pool[MAX_MINERS];
    /* int last_miner; */
    int total_miners;
    pid_t monitor_pid;
    pid_t last_winner;
    sem_t sem_net_mutex;
} NetData;

long int simple_hash(long int number);
int net_register(NetData **netStruct, int *miner_ind);
int clean(NetData *netStruct, int miner_ind);
