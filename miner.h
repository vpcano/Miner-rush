#include <unistd.h>
#include <semaphore.h>

#define OK 0
#define MAX_WORKERS 10

#define SHM_NAME_NET "/netdata"
#define SHM_NAME_BLOCK "/block"

#define MAX_MINERS 200

#define SEM_TIMEOUT 3

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
    int total_miners;
    pid_t monitor_pid;
    pid_t current_winner;
    pid_t last_winner;
    sem_t sem_net_mutex;
    sem_t sem_block_mutex;
    sem_t sem_round;
    sem_t sem_winner;
    sem_t sem_updated;
    sem_t sem_entry;
    sem_t sem_voting;
    sem_t sem_result;
} NetData;

int check_arguments(int argc, char **argv, int *n_wks, int *n_rds);
int sig_setup();
long int simple_hash(long int number);
int net_register(NetData **netStruct, Block **blockStruct, int *miner_ind);
int miner_main_loop(NetData *netStruct, Block *blockStruct, Block **pplast_block, int miner_ind, int n_workers, int n_rounds, int *win);
int update_blockchain(NetData *netStruct, Block *shm_block, Block **pplast_block);
void print_blocks(Block *plast_block);
int prepare_next_round(NetData *netStruct, Block *blockStruct, int *v_res, int n_miners);
int clean(NetData *netStruct, Block *blockStruct, Block *plast_block, int miner_ind, int *win);
