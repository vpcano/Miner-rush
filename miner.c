#include <stdio.h>
#include <fcntl.h>
#include <errno.h>
#include <time.h>
#include <signal.h>
#include <string.h>
#include <mqueue.h>
#include <stdlib.h>
#include <unistd.h>
#include <pthread.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/types.h>
#include <semaphore.h>
#include "miner.h"

#define PRIME 99997669
#define BIG_X 435679812
#define BIG_Y 100001819
#define TRUE 1
#define FALSE 0


struct worker_args_struct {
    long target;
    long start;
    long end;
    long solution;
};




/**********************
 ** Global variables **
 **********************/
static volatile sig_atomic_t active = TRUE;
static volatile sig_atomic_t flag = TRUE;




/*********************
 ** Signal handlers **
 *********************/
void sigusr2_handler(int sig) {
    flag = FALSE;
}

void sigint_handler(int sig) {
    flag = FALSE;
    active = FALSE;
}




/*******************
 ** Main function **
 *******************/

int main(int argc, char **argv) {
    NetData *net_data;
    Block *plast_block = NULL; /* Last block of the blockchain pointer. Allows access to all the chain */
    Block *shm_block; /* Pointer to shared memeory segment */
    int miner_index; /* To free miners_pid position on shared memory later */
    int win = FALSE;
    int n_workers, n_rounds;

    srand(time(NULL));

    if (check_arguments(argc, argv, &n_workers, &n_rounds) != EXIT_SUCCESS) exit(EXIT_FAILURE);

    if (sig_setup() != EXIT_SUCCESS) exit(EXIT_FAILURE);

    if (net_register(&net_data, &shm_block, &miner_index) != EXIT_SUCCESS) exit(EXIT_FAILURE);

    if (miner_main_loop(net_data, shm_block, &plast_block, miner_index, n_workers, n_rounds, &win) != EXIT_SUCCESS) {
        clean(net_data, shm_block, plast_block, miner_index, &win);
        exit(EXIT_FAILURE);
    }

    print_blocks(plast_block);

    if (clean(net_data, shm_block, plast_block, miner_index, &win) != EXIT_SUCCESS) exit(EXIT_FAILURE);

    exit(EXIT_SUCCESS);
}




/* Private */
int down(sem_t *sem) {
    while (sem_wait(sem) != 0) {
        if (errno != EINTR) {
            return EXIT_FAILURE;
        }
    }

    return EXIT_SUCCESS;
}

/* Private */
int down_timed(sem_t *sem) {
    struct timespec ts;

    if (clock_gettime(CLOCK_REALTIME, &ts) == -1) {
        perror("Error: could not get actual time\nclock_gettime");
        return EXIT_FAILURE;
    }

    ts.tv_sec += SEM_TIMEOUT;

    if (sem_timedwait(sem, &ts) == -1) {
        if (errno == ETIMEDOUT) {
            fprintf(stdout, "Max timeout reached. Aborting...\n");
            return EXIT_SUCCESS;
        }
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}




/*********************
 ** Check arguments **
 *********************/
int check_arguments(int argc, char **argv, int *n_wks, int *n_rds) {

    if (argc < 3) {
        fprintf(stderr, "Error: invalid arguments\n");
        fprintf(stdout, "Usage: %s <number_of_workers> <number_of_rounds>\n", argv[0]);
        return EXIT_FAILURE;
    }

    *n_wks = atoi(argv[1]);
    *n_rds = atoi(argv[2]);

    if (*n_wks > 10 || *n_wks < 1) {
        fprintf(stderr, "Error: number of workers must be a positive number greater than %d\n", MAX_WORKERS);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}




/***************************
 ** Signal handlers setup **
 ***************************/
int sig_setup() {
    struct sigaction act1, act2;

    act1.sa_handler = sigusr2_handler;
    sigemptyset(&(act1.sa_mask));
    act1.sa_flags = 0;

    act2.sa_handler = sigint_handler;
    sigemptyset(&(act2.sa_mask));
    act2.sa_flags = 0;

    if (sigaction(SIGUSR2, &act1, NULL) < 0
        || sigaction(SIGINT, &act2, NULL) < 0) {
        perror("Error: could not set up a signal handler.\nsigaction");
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}




/************************
 ** Net Data Functions **
 ************************/
/*
**                     |-----> init_net -----> init_shm_block
**  net_register ------| or
**                     |-----> join_net -----> open_shm_block
 */

/* Private */
int init_shm_block(Block **blockStruct) {
    int shm_block_fd;

    if ((shm_block_fd = shm_open(SHM_NAME_BLOCK, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        if (errno == EEXIST) {
            fprintf(stderr, "Error: block shared memory segment already exists\n");
        }
        else {
            perror("Error: could not create block structure on shared memory\nshm_open");
        }

        return EXIT_FAILURE;
    }

    /* Truncate the memory segment to the size of a block */
    if (ftruncate(shm_block_fd, sizeof(Block))==-1) {
        perror("Error: could not truncate shared memory size to block structure\nftruncate");
        close(shm_block_fd);
        unlink(SHM_NAME_BLOCK);
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *blockStruct = mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_block_fd, 0);
    close(shm_block_fd);
    if (*blockStruct == MAP_FAILED) {
        perror("Error: could not map block structure\nmmap");
        unlink(SHM_NAME_BLOCK);
        return EXIT_FAILURE;
    }

    (*blockStruct)->id = 1;
    (*blockStruct)->is_valid = FALSE;
    (*blockStruct)->target = (long) ((double)rand()*(double)PRIME/(double)RAND_MAX);
    (*blockStruct)->solution = -1;
    (*blockStruct)->wallets[0] = 0;
    for (int i=1; i<MAX_MINERS; i++) (*blockStruct)->wallets[i] = -1;
    (*blockStruct)->next = NULL;
    (*blockStruct)->prev = NULL;

    return EXIT_SUCCESS;
}

/* Private */
int init_net(NetData *netStruct, Block **blockStruct, int *miner_ind) {

    /* Initialize mutex */
    if (sem_init(&(netStruct->sem_net_mutex), 1, 0) == -1
        || sem_init(&(netStruct->sem_block_mutex), 1, 0) == -1
        || sem_init(&(netStruct->sem_round), 1, 1) == -1
        || sem_init(&(netStruct->sem_winner), 1, 1) == -1
        || sem_init(&(netStruct->sem_updated), 1, 0) == -1
        || sem_init(&(netStruct->sem_entry), 1, 1) == -1
        || sem_init(&(netStruct->sem_voting), 1, 0) == -1
        || sem_init(&(netStruct->sem_result), 1, 0) == -1)
    {
        perror("Error: could not initialize a semaphore\nsem_init");
        shm_unlink(SHM_NAME_NET);
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }

    netStruct->miners_pid[0] = getpid();
    netStruct->last_winner = -1;
    netStruct->current_winner = -1;
    /* Initialize all array elements to -1, to indicate there is no active miner */
    for (int i=1; i<MAX_MINERS; i++) netStruct->miners_pid[i] = -1;
    netStruct->total_miners = 1;
    /* Initialize all array elements to -1, to indicate it is not a valid vote */
    for (int i=0; i<MAX_MINERS; i++) netStruct->voting_pool[i] = -1;
    *miner_ind = 0;

    /* Now other processes can access net data structure on shared memory when joining
     * the net, but block structure on shared memory is still restricted */
    sem_post(&(netStruct->sem_net_mutex));

    if (init_shm_block(blockStruct) == EXIT_FAILURE) {
        shm_unlink(SHM_NAME_NET);
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }

    /* Now processes joining the net can also open and access block structure on shared memory */
    sem_post(&(netStruct->sem_block_mutex));

    return EXIT_SUCCESS;
}

/* Private */
int open_shm_block(Block **blockStruct, int miner_ind) {
    int shm_block_fd;

    if ((shm_block_fd = shm_open(SHM_NAME_BLOCK, O_RDWR, 0)) == -1) {
        perror("Error: could not open existing block structure on shared memory\nshm_open");
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *blockStruct = mmap(NULL, sizeof(Block), PROT_READ | PROT_WRITE, MAP_SHARED, shm_block_fd, 0);
    close(shm_block_fd);
    if (*blockStruct == MAP_FAILED) {
        perror("Error: could not map block structure\nmmap");
        return EXIT_FAILURE;
    }

    (*blockStruct)->wallets[miner_ind] = 0;

    return EXIT_SUCCESS;
}

/* Private */
int join_net(NetData *netStruct, Block **blockStruct, int *miner_ind) {
    int i;

    down(&(netStruct->sem_entry));

    down(&(netStruct->sem_net_mutex));

    for (i=0; netStruct->miners_pid[i]!=-1 && i<netStruct->total_miners+1; i++);
    if (i >= MAX_MINERS) {
        fprintf(stderr, "Error: max miners amount reached\n");
        sem_post(&(netStruct->sem_net_mutex));
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }
    netStruct->miners_pid[i] = getpid();
    netStruct->total_miners++;
    *miner_ind = i;

    sem_post(&(netStruct->sem_net_mutex));

    down(&(netStruct->sem_block_mutex));

    if (open_shm_block(blockStruct, *miner_ind) == EXIT_FAILURE) {
        /* If error, revert changes and exit */
        sem_post(&(netStruct->sem_block_mutex));
        netStruct->miners_pid[i] = -1;
        netStruct->total_miners--;
        munmap(netStruct, sizeof(NetData));
        return EXIT_FAILURE;
    }

    sem_post(&(netStruct->sem_block_mutex));

    /* To enter into miner's main loop */
    sem_post(&(netStruct->sem_round));

    sem_post(&(netStruct->sem_entry));

    fprintf(stdout, "Successfuly joined net.\n"); /* REMOVE */

    return EXIT_SUCCESS;
}

int net_register(NetData **netStruct, Block **blockStruct, int *miner_ind) {
    int shm_net_fd;
    int exist = FALSE;

    /* Try creating net info structure on share memory */
    if ((shm_net_fd = shm_open(SHM_NAME_NET, O_CREAT | O_EXCL | O_RDWR, S_IRUSR | S_IWUSR)) == -1) {
        if (errno == EEXIST) {
            exist = TRUE;
            /* If structure already exists, open it */
            if ((shm_net_fd = shm_open(SHM_NAME_NET, O_RDWR, 0)) == -1) {
                perror("Error: could not open existing miner net info structure on shared memory\nshm_open");
                return EXIT_FAILURE;
            }
        }
        else {
            perror("Error: could not create miner net info structure on shared memory\nshm_open");
            return EXIT_FAILURE;
        }
    }

    /* If shared memory was created, truncate it */
    if (!exist && ftruncate(shm_net_fd, sizeof(NetData))==-1) {
        perror("Error: could not truncate shared memory size to miner net data structure\nftruncate");
        close(shm_net_fd);
        unlink(SHM_NAME_NET);
        return EXIT_FAILURE;
    }

    /* Map the memory segment. */
    *netStruct = mmap(NULL, sizeof(NetData), PROT_READ | PROT_WRITE, MAP_SHARED, shm_net_fd, 0);
    close(shm_net_fd);
    if (*netStruct == MAP_FAILED) {
        perror("Error: could not map miner net info structure\nmmap");
        if (!exist) unlink(SHM_NAME_NET); /* If I am the first miner, delete the shared memory */
        return EXIT_FAILURE;
    }

    if (!exist) return init_net(*netStruct, blockStruct, miner_ind);
    else return join_net(*netStruct, blockStruct, miner_ind);
}




/**********************
 ** Mining functions **
 **********************/

long int simple_hash(long int number) {
    long int result = (number * BIG_X + BIG_Y) % PRIME;
    return result;
}

void *worker_main_loop(struct worker_args_struct *args) {
    long i;

    for (i=args->start; flag && i<args->end; i++) {
        if (args->target == simple_hash(i)) {
            flag = 0;
            args->solution = i;
            pthread_exit(NULL);
        }
    }

    args->solution = -1;
    pthread_exit(NULL);
}

/* Private */
int setup_workers(pthread_t **workers, struct worker_args_struct **w_args, int n_workers) {

    *workers = (pthread_t*) malloc(n_workers*sizeof(pthread_t));
    if (*workers == NULL) {
        perror("Error: could not alloc memory\nmalloc");
        return EXIT_FAILURE;
    }

    *w_args = (struct worker_args_struct *) malloc(n_workers*sizeof(struct worker_args_struct));
    if (*w_args == NULL) {
        perror("Error: could not alloc memory\nmalloc");
        free(*workers);
        return EXIT_FAILURE;
    }

    return EXIT_SUCCESS;
}

/* Private */
int load_workers(pthread_t *workers, struct worker_args_struct *w_args, int n_workers, long target, long *solution, int *win) {
    int i, f, error;
    double div;

    div = (double)PRIME/(double)n_workers;
    f = 0;

    /* Initialize workers (threads) */
    for (i=0; flag && i<n_workers; i++) {
        w_args[i].start = i*div;
        w_args[i].end = (i+1)*div;
        w_args[i].target = target;
        w_args[i].solution = -1;
        error = pthread_create(workers+i, NULL, (void*) worker_main_loop, w_args+i);
        if (error != 0) {
            fprintf(stderr, "Error: could not start worker\npthread_create: %s\n", strerror(error));
            flag = FALSE; /* Tell other workers to end */
            f = TRUE; /* To return error later */
            break; /* Stop creating new threads, and join the ones created */
        }
    }

    /* Wait for workers to end */
    for (i--; i>=0; i--) {
        error = pthread_join(workers[i], NULL);
        /* If thread did not end correctly, tell other threads to end, wait for them and return error */
        if (error != 0) {
            fprintf(stderr, "Error: worker did not end correctly\npthread_join: %s\n", strerror(error));
            flag = FALSE; /* Tell other active threads to end */
            f = TRUE; /* To return error later */
        }
        else if (!f && w_args[i].solution > 0) {
            /* If worker found the solution, and no error occured, update win status */
            *solution = w_args[i].solution;
            *win = TRUE;
        }
    }

    if (f) return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

/* Private */
int handle_win(NetData *netStruct, Block *blockStruct, long solution, int *n_miners) {
    int j, k;

    down(&(netStruct->sem_winner));
    down(&(netStruct->sem_net_mutex));
    if (netStruct->current_winner > 0) {
        /* Another miner has already found the solution, retry */
        fprintf(stdout, "Another miner found already the same solution\n"); /* REMOVE */
        sem_post(&(netStruct->sem_net_mutex));
        sem_post(&(netStruct->sem_winner));
        return FALSE;
    }
    else {
        fprintf(stdout, "Found solution: %ld\n", solution); /* REMOVE */

        netStruct->current_winner = getpid();
        /* Stop other miners */
        for (k=0, k=0; j < netStruct->total_miners && k < MAX_MINERS; k++) {
            if (netStruct->miners_pid[k] != -1 && netStruct->miners_pid[k] != getpid()) {
                kill(netStruct->miners_pid[k], SIGUSR2);
                j++;
            }
        }
        sem_post(&(netStruct->sem_net_mutex));

        /* Now other miners must wait to enter the net */
        down(&(netStruct->sem_entry));


        down(&(netStruct->sem_net_mutex));
        /* Number of miners that will participate in the voting */
        *n_miners = netStruct->total_miners;
        sem_post(&(netStruct->sem_net_mutex));


        down(&(netStruct->sem_block_mutex));
        /* Write solution to shared memory block */
        blockStruct->solution = solution;
        sem_post(&(netStruct->sem_block_mutex));

        sem_post(&(netStruct->sem_winner));
        return TRUE;
    }
}

int handle_voting(NetData *netStruct, Block *blockStruct, int miner_ind,  int n_miners) {
    int i, v_yes, v_no, ret;

    /* Wait until every miner has voted */
    for (i=0; i<n_miners-1; i++) down(&(netStruct->sem_voting));

    for (i=0, v_yes=0, v_no=0; i<MAX_MINERS; i++) {
        if (netStruct->voting_pool[i] == TRUE) v_yes++;
        else if (netStruct->voting_pool[i] == FALSE) v_no++;
    }

    if (v_yes > v_no || (v_yes==0 && v_no==0)) {
        down(&(netStruct->sem_block_mutex));
        blockStruct->is_valid = TRUE;
        blockStruct->wallets[miner_ind]++;
        sem_post(&(netStruct->sem_block_mutex));
        ret = TRUE;
    }
    else {
        fprintf(stdout, "Not enough votes, invalid solution\n");
        ret = FALSE;
    }

    for (i=0; i<n_miners-1; i++) sem_post(&(netStruct->sem_result));

    return ret;
}

int vote(NetData *netStruct, Block *blockStruct, int miner_ind) {
    long solution, target;
    int ret;

    /* Wait for winner to update solution */
    down(&(netStruct->sem_winner));

    down(&(netStruct->sem_block_mutex));
    solution = blockStruct->solution;
    target = blockStruct->target;
    sem_post(&(netStruct->sem_block_mutex));

    if (simple_hash(solution) == target) {
        down(&(netStruct->sem_net_mutex));
        netStruct->voting_pool[miner_ind] = TRUE;
        sem_post(&(netStruct->sem_net_mutex));
        fprintf(stdout, "Voted in favor. solution: %ld, target: %ld\n", solution, target); /* REMOVE */
    }
    else {
        down(&(netStruct->sem_net_mutex));
        netStruct->voting_pool[miner_ind] = FALSE;
        sem_post(&(netStruct->sem_net_mutex));
        fprintf(stdout, "Voted against. solution: %ld, target: %ld\n", solution, target); /* REMOVE */
    }

    sem_post(&(netStruct->sem_voting));

    sem_post(&(netStruct->sem_winner));

    /* Wait until voting's result is known */
    down_timed(&(netStruct->sem_result));

    down(&(netStruct->sem_block_mutex));
    ret = blockStruct->is_valid;
    sem_post(&(netStruct->sem_block_mutex));

    return ret;
}

int miner_main_loop(NetData *netStruct, Block *blockStruct, Block **pplast_block, int miner_ind, int n_workers, int n_rounds, int *win) {
    int i, j, v_res, n_miners;
    long target, solution;
    pthread_t *workers;
    struct worker_args_struct *w_args;

    if (setup_workers(&workers, &w_args, n_workers) != EXIT_SUCCESS) return EXIT_FAILURE;

    /* Main loop */
    i = 0; /* Round counter */
    while (active && (n_rounds<=0 || i<n_rounds)) {
        *win = FALSE;
        down(&(netStruct->sem_round));

        down(&(netStruct->sem_block_mutex));
        target = blockStruct->target;
        sem_post(&(netStruct->sem_block_mutex));

        fprintf(stdout, "Searching solution for block with target: %ld\n", target); /* REMOVE */

        if (load_workers(workers, w_args, n_workers, target, &solution, win) != EXIT_SUCCESS) {
            free(workers);
            free(w_args);
            return EXIT_FAILURE;
        }

        /* If there has been more than one winner, reduce them to just one.
         * Stop other miners, and write solution to shared memory block. */
        if (*win == TRUE) *win = handle_win(netStruct, blockStruct, solution, &n_miners);

        if (*win == TRUE) v_res = handle_voting(netStruct, blockStruct, miner_ind, n_miners); /* Will wait for all miners to vote */
        else v_res = vote(netStruct, blockStruct, miner_ind); /* Will end when voting result is known */


        if (v_res == TRUE) {
            fprintf(stdout, "Updating blockchain with winner's solution.\n"); /* REMOVE */
            if (update_blockchain(netStruct, blockStruct, pplast_block) != EXIT_SUCCESS) {
                /* CdE */
                free(workers);
                free(w_args);
                return EXIT_FAILURE;
            }

            if (*win == TRUE) {
                /* Wait for all miners (except winner) to update their blockchains */
                for (j=0; j<n_miners-1; j++) down(&(netStruct->sem_updated));
            }
            else if (active && (n_rounds<=0 || (i+1)<n_rounds)) {
                /* If this is miner's last round, update just after cleaning and decreasing
                 * total miners, so it will not be active when the next round begins. */
                sem_post(&(netStruct->sem_updated));
            }
        }

        /* For the next round */
        if (*win == TRUE) prepare_next_round(netStruct, blockStruct, &v_res, n_miners);
        i++;
        flag = TRUE;
    }

    down_timed(&(netStruct->sem_round));

    free(workers);
    free(w_args);

    return EXIT_SUCCESS;
}

int prepare_next_round(NetData *netStruct, Block *blockStruct, int *v_res, int n_miners) {
    int i;

    down(&(netStruct->sem_block_mutex));
    down(&(netStruct->sem_net_mutex));

    if (*v_res == TRUE) {
        blockStruct->id++ ;
        blockStruct->target = blockStruct->solution;
        blockStruct->solution = -1;
        blockStruct->is_valid = FALSE;
        netStruct->last_winner = getpid();
    }
    else {
        blockStruct->solution = -1;
    }
    netStruct->current_winner = -1;
    for (i=0; i<MAX_MINERS; i++) netStruct->voting_pool[i] = -1;

    sem_post(&(netStruct->sem_net_mutex));
    sem_post(&(netStruct->sem_block_mutex));


    for (i=0; i<n_miners; i++) sem_post(&(netStruct->sem_round));

    /* Now other miners can join the net */
    sem_post(&(netStruct->sem_entry));

    *v_res = -1;

    return EXIT_SUCCESS;
}

int update_blockchain(NetData *netStruct, Block *shm_block, Block **pplast_block) {
    Block *temp;

    down(&(netStruct->sem_block_mutex));

    temp = (Block*) malloc(sizeof(Block));
    if (temp == NULL) return EXIT_FAILURE;
    if (memcpy(temp, shm_block, sizeof(Block)) == NULL) {
        free(temp);
        return EXIT_FAILURE;
    }

    temp->prev = *pplast_block;
    if (*pplast_block != NULL) (*pplast_block)->next = temp;

    *pplast_block = temp;

    sem_post(&(netStruct->sem_block_mutex));

    return EXIT_SUCCESS;
}

void print_blocks(Block *plast_block) {
    Block *block = NULL;
    int i, j;

    for(i = 0, block = plast_block; block != NULL; block = block->prev, i++) {
        printf("Block number: %d; Target: %ld;    Solution: %ld\n", block->id, block->target, block->solution);
        for(j = 0; j < MAX_MINERS; j++) {
            if (block->wallets[j] != -1) printf("%d: %d;         ", j, block->wallets[j]);
        }
        printf("\n\n\n");
    }
    printf("A total of %d blocks were printed\n", i);
}

int clean(NetData *netStruct, Block *blockStruct, Block *plast_block, int miner_ind, int *win) {

    down(&(netStruct->sem_block_mutex));

    blockStruct->wallets[miner_ind] = -1;

    sem_post(&(netStruct->sem_block_mutex));

    down(&(netStruct->sem_net_mutex));

    /* Tell other miners this miner is finished */
    netStruct->miners_pid[miner_ind] = -1;
    netStruct->total_miners--;

    if (netStruct->total_miners <= 0) {

        fprintf(stdout, "Last miner, destroying net.\n"); /* REMOVE */

        /* Free shared memory if this is the last miner */
        sem_destroy(&netStruct->sem_net_mutex);
        sem_destroy(&netStruct->sem_block_mutex);
        sem_destroy(&netStruct->sem_round);
        sem_destroy(&netStruct->sem_winner);
        sem_destroy(&netStruct->sem_updated);
        sem_destroy(&netStruct->sem_entry);
        sem_destroy(&netStruct->sem_voting);
        sem_destroy(&netStruct->sem_result);
        shm_unlink(SHM_NAME_NET);
        shm_unlink(SHM_NAME_BLOCK);
    } else {
        sem_post(&(netStruct->sem_net_mutex));
        if (!*win) sem_post(&(netStruct->sem_updated));

        fprintf(stdout, "Miner ended successfuly\n"); /* REMOVE */
    }

    munmap(netStruct, sizeof(NetData));
    munmap(blockStruct, sizeof(Block));

    /* Free miner's blockchain */
    for (Block *block = plast_block, *temp = NULL; block != NULL && temp != NULL; block = temp) {
        temp = block->prev;
        free(block);
    }

    return EXIT_SUCCESS;
}
