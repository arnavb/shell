#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <signal.h>

#define COMMAND_LENGTH 1
#define JOBS_CAPACITY 10000

/* jobs */
/* job states */
typedef enum job_state {
    RUNNING,     /* job is running in background/foreground */
    STOPPED,     /* job is stopped */
    TERMINATED,  /* job is terminated by signal */
    COMPLETED    /* job completed normally */
} job_state;

/* structure representing a single job */
typedef struct job {
    int id;                 /* job id */
    char *command;          /* absolute path to executable */
    char *originalCommand;  /* tokens converted back to a string (for jobs output) */
    pid_t pgid;             /* process group id */
    job_state state;        /* job state */
    int bgProcess;          /* whether job is running in background/foreground */
    int termSig;            /* if job was terminated by signal, the terminating signal */
    struct job *next;       /* next job in linked list */
} job;

/* head/tail of job linked list */
job *jobListHead = NULL;
job *jobListTail = NULL;

/* to track job ids */
int nextJobId = 1;
int activeJobs = 0;

/* initializes the job list */
void initJobs();

/* creates the job pointer struct with default values, is
 * essentially a constructor */
job *createJob(char *command, char *originalCommand, pid_t pgid, job_state state, int bgProcess);

/* adds job to linked list of jobs */
void addJob(job *j);

/* marks a change in job state by pgid, called in SIGCHLD
 * handler (sigchldHandler). Usually used to  */
int markJob(pid_t pgid, job_state state, int termSig);

/* deletes jobs marked as completed/terminated (and outputs for signals) */
void cleanUpJobs();

/* prints all jobs */
void printJobs();

/* free individual job/any malloc'd members */
void freeJob(job *j);

/* frees linked list of jobs */
void freeAllJobs();

/* performs shell cleanup, e.g sending SIGHUP/SIGCONT, calling
 * freeAllJobs() and exiting */
void cleanUpShell();

/* signals */
void safeSignal(int signum, sig_t handler);
void sigchldHandler(int signum);
void sigintHandler(int signum);
void sigtstpHandler(int signum);

/* converts command line to array of tokens, removing extra whitespace */
char **tokenize(char *input, int *numTokens, int *capacity);

/* removes ampersands and indicates whether process is to run in background */
int handleAmpersand(char **tokens, int *numTokens);

/* commands */
/* builtins */
void bg(char **argv, int numTokens);
void cd(char **argv, int numTokens);
void exitFunc(int numTokens);
void fg(char **argv, int numTokens);
void jobs(int numTokens);
void killFunc(char **argv, int numTokens);

/* forks process to run command in foreground/background */
int runCommand(char *command, int argc, char **argv, int bgProcess);

/* converts string to a job id, used in fg/bg/kill, with error handling */
int stringToJobId(char *string);

/* joins array of strings into new string separated by spaces */
char *joinString(int argc, char **argv);

/* checks if file exists and is executable */
int fileExists(char *filename);

int main(int argc, char **argv) {
    int running = 1;
    initJobs();

    safeSignal(SIGCHLD, sigchldHandler);
    safeSignal(SIGINT, sigintHandler);
    safeSignal(SIGTSTP, sigtstpHandler);

    while (running) {
        char *input = NULL;
        char **tokens;
        int numTokens;
        int i;
        size_t bufferLength = 0;
        int charsRead;
        int capacity;
        int bgProcess = 0;
        char *command;
        
        printf("> ");

        /* read input command */
        charsRead = getline(&input, &bufferLength, stdin);

        if (charsRead == -1) {
            free(input);
            break;
        }

        cleanUpJobs();

        /* Remove trailing newline */
        input[--charsRead] = '\0';
        
        if (charsRead == 0) {
            /* user typed an empty line */
            free(input);
            continue;
        }

        /* Break line into individual tokens */
        tokens = tokenize(input, &numTokens, &capacity);
        bgProcess = handleAmpersand(tokens, &numTokens);

        if (numTokens == 0) {
            /* user typed line of only whitespace */
            free(tokens);
            free(input);
            continue;
        }

        if (numTokens == capacity) {
            capacity += COMMAND_LENGTH;
            tokens = realloc(tokens, capacity * sizeof(char*));
        }
        tokens[numTokens] = NULL;

        command = strdup(tokens[0]);

        /* relative or absolute filepath */
        if (command[0] == '.' || command[0] == '/') {
            if (fileExists(command)) {
                runCommand(command, numTokens, tokens, bgProcess);
            } else {
                printf("%s: No such file or directory\n", command);
            }
        } else {
            /* direct command name */
            sigset_t maskOne, prevOne;

            sigemptyset(&maskOne);
            sigaddset(&maskOne, SIGCHLD);

            sigprocmask(SIG_BLOCK, &maskOne, &prevOne);

            /* builtins */
            if (strcmp(command, "bg") == 0) {
                bg(tokens, numTokens);
                sigprocmask(SIG_SETMASK, &prevOne, NULL);
            } else if (strcmp(command, "fg") == 0) {
                fg(tokens, numTokens);
                sigprocmask(SIG_SETMASK, &prevOne, NULL);
            } else if (strcmp(command, "cd") == 0) {
                sigprocmask(SIG_SETMASK, &prevOne, NULL);
                cd(tokens, numTokens);
            } else if (strcmp(command, "jobs") == 0) {
                jobs(numTokens);
                sigprocmask(SIG_SETMASK, &prevOne, NULL);
            } else if (strcmp(command, "kill") == 0) {
                killFunc(tokens, numTokens);
                sigprocmask(SIG_SETMASK, &prevOne, NULL);
            } else if (strcmp(command, "exit") == 0) {
                sigprocmask(SIG_SETMASK, &prevOne, NULL);
                for (i = 0; i < numTokens; i++) {
                    free(tokens[i]);
                }

                free(command);
                free(tokens);
                free(input);
                break;
            } else {
                /* check /bin and /usr/bin in that order */
                int result;
                int commandLength = strlen(command);
                char *binPrefix = malloc((5 + commandLength + 1) * sizeof(char));
                char *usrBinPrefix = malloc((9 + commandLength + 1) * sizeof(char));

                sigprocmask(SIG_SETMASK, &prevOne, NULL);

                sprintf(binPrefix, "/bin/%s", command);
                sprintf(usrBinPrefix, "/usr/bin/%s", command);

                if (fileExists(binPrefix)) {
                    result = runCommand(binPrefix, numTokens, tokens, bgProcess);
                } else if (fileExists(usrBinPrefix)) {
                    result = runCommand(usrBinPrefix, numTokens, tokens, bgProcess);
                } else {
                    printf("%s: command not found\n", command);
                }

                free(usrBinPrefix);
                free(binPrefix);
            }
        }


        for (i = 0; i < numTokens; i++) {
            free(tokens[i]);
        }

        free(command);
        free(tokens);
        free(input);

        cleanUpJobs();
    }
    cleanUpShell();

    return EXIT_SUCCESS;
}

void initJobs() {
    jobListHead = NULL;
}

job *createJob(char *command, char *originalCommand, pid_t pgid, job_state state, int bgProcess) {
    job *j = malloc(sizeof(job));

    if (activeJobs == 0) {
        nextJobId = 1;
    } else {
        nextJobId++;
    }

    j->id = nextJobId;
    j->command = command;
    j->originalCommand = originalCommand;
    j->pgid = pgid;
    j->state = state;
    j->bgProcess = bgProcess;
    j->termSig = -1;
    j->next = NULL;

    return j;
}

void addJob(job *j) {
    activeJobs++;
    if (jobListHead == NULL) {
        jobListHead = jobListTail = j;
    } else {
        jobListTail->next = j;
        jobListTail = j;
    }
}

int markJob(pid_t pgid, job_state state, int termSig) {
    job *previous;
    job *current = jobListHead;

    while (current != NULL && current->pgid != pgid) {
        previous = current;
        current = current->next;
    }

    if (current == NULL) {
        return -1;
    }

    current->state = state;
    if (current->state == TERMINATED) {
        current->termSig = termSig;
    }
    return 0;
}

void cleanUpJobs() {
    sigset_t maskOne, prevOne;

    sigemptyset(&maskOne);
    sigaddset(&maskOne, SIGCHLD);
    sigprocmask(SIG_BLOCK, &maskOne, &prevOne);

    job *previous = NULL;
    job *current = jobListHead;

    while (current != NULL) {
        if (current->state == COMPLETED || current->state == TERMINATED) {
            if (current->state == TERMINATED) {
                printf("[%d] %d terminated by signal %d\n", current->id, current->pgid, current->termSig);
            }
            /* deleting first node */
            if (previous == NULL) {
                job *temp = current->next;
                freeJob(current);
                jobListHead = current = temp;
            } else {
                previous->next = current->next;
                freeJob(current);
                current = previous->next;
            }
            activeJobs--;
        } else {
            previous = current;
            current = current->next;
        }
    }

    jobListTail = previous;

    sigprocmask(SIG_SETMASK, &prevOne, NULL);
}

void cleanUpShell() {
    job *current = jobListHead;
    sigset_t maskOne, prevOne;

    sigemptyset(&maskOne);
    sigaddset(&maskOne, SIGCHLD);
    sigprocmask(SIG_BLOCK, &maskOne, &prevOne);

    while (current != NULL) {
        if (current->state == STOPPED) {
            if (kill(current->pgid, SIGHUP) < 0) {
                puts("SIGHUP failed");
                current = current->next;
                continue;
            }

            if (kill(current->pgid, SIGCONT) < 0) {
                puts("SIGCONT failed");
                current = current->next;
                continue;
            }
        } else if (current->state == RUNNING) {
            if (kill(current->pgid, SIGHUP) < 0) {
                puts("SIGHUP failed");
                current = current->next;
                continue;
            }
        }
        current = current->next;
    }

    freeAllJobs();
    exit(0);

    sigprocmask(SIG_SETMASK, &prevOne, NULL);
}

void printJobs() {
    job *current = jobListHead;

    while (current != NULL) {
        printf("[%d] %d ", current->id, current->pgid);

        if (current->state == RUNNING) {
            printf("Running ");
        } else if (current->state == STOPPED) {
            printf("Stopped ");
        }

        printf("%s ", current->originalCommand);

        if (current->bgProcess) {
            printf("&");
        }

        puts("");

        current = current->next;
    }
}

void freeJob(job *j) {
    free(j->command);
    free(j->originalCommand);
    free(j);
}

void freeAllJobs() {
    job *current = jobListHead;
    
    while (current != NULL) {
        job *next = current->next;
        freeJob(current);
        current = next;
    }

    jobListHead = NULL;
}

void safeSignal(int signum, sig_t handler) {
    struct sigaction sa;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sa.sa_handler = handler;

    sigaction(signum, &sa, NULL);
}

void sigchldHandler(int signum) {
    int oldErrno = errno;
    sigset_t maskAll, prevAll;
    int status;
    pid_t pid;

    sigfillset(&maskAll);

    while ((pid = waitpid(-1, &status, WUNTRACED | WNOHANG)) > 0) {
        sigprocmask(SIG_BLOCK, &maskAll, &prevAll);
        if (WIFEXITED(status)) {
            markJob(pid, COMPLETED, -1);
        }

        if (WIFSTOPPED(status)) {
            markJob(pid, STOPPED, -1);
        }

        if (WIFSIGNALED(status)) {
            markJob(pid, TERMINATED, WTERMSIG(status));
        }
        sigprocmask(SIG_SETMASK, &prevAll, NULL);
    }
    errno = oldErrno;
}

/* empty so that shell doesn't exit */
void sigintHandler(int signum) {
}
void sigtstpHandler(int signum) {
}

char **tokenize(char *input, int *numTokens, int *capacity) {
    int maxSize = COMMAND_LENGTH;
    char **result = malloc(maxSize * sizeof(char*));
    int currentSize = 0;
    char *token = strtok(input, " \t\r\n");
    
    while (token) {
        if (currentSize == maxSize) {
            maxSize += COMMAND_LENGTH;
            result = realloc(result, maxSize * sizeof(char*));
        }

        result[currentSize] = strdup(token);

        token = strtok(NULL, " ");
        currentSize++;
    }

    *numTokens = currentSize;
    *capacity = maxSize;
    return result;
}

int handleAmpersand(char **tokens, int *numTokens) {
    int lastTokenLength;

    /* last token is an ampersand */
    if (strcmp(tokens[*numTokens - 1], "&") == 0) {
        free(tokens[*numTokens - 1]);
        (*numTokens)--;
        return 1;
    }

    lastTokenLength = strlen(tokens[*numTokens - 1]);

    /* last character of last token is an ampersand */
    if (tokens[*numTokens - 1][lastTokenLength - 1] == '&') {
        tokens[*numTokens - 1][lastTokenLength - 1] = '\0';
        return 1;
    }

    return 0;
}

void bg(char **argv, int numTokens) {
    int jid;
    job *current;

    if (numTokens != 2) {
        puts("bg: wrong number of arguments");
        return;
    }

    if ((jid = stringToJobId(argv[1])) == -1) {
        puts("bg: invalid job id");
        return;
    }

    current = jobListHead;
    while (current != NULL && current->id != jid) {
        current = current->next;
    }

    if (current == NULL) {
        puts("bg: job not found");
        return;
    }

    if (current->state == RUNNING) {
        puts("bg: job is already running");
        return;
    }

    current->bgProcess = 1;
    current->state = RUNNING;

    if (kill(current->pgid, SIGCONT) < 0) {
        puts("bg: could not continue process");
        return;
    }
}

void cd(char **argv, int numTokens) {
    char *directory;

    if (numTokens > 2) {
        puts("cd: too many arguments");
        return;
    }

    directory = numTokens == 1 ? getenv("HOME") : argv[1];

    if (directory != NULL) {
        if (chdir(directory) != 0) {
            printf("cd: no such file or directory: %s\n", directory);
            return;
        }
        setenv("PWD", directory, 1);
    }
}

void exitFunc(int numTokens) {
    if (numTokens > 1) {
        puts("exit: too many arguments");
        return;
    }

    cleanUpShell();
}

void fg(char **argv, int numTokens) {
    int jid;
    job *current;
    int status, wpid;

    if (numTokens != 2) {
        puts("fg: wrong number of arguments");
        return;
    }

    if ((jid = stringToJobId(argv[1])) == -1) {
        puts("fg: invalid job id");
        return;
    }

    current = jobListHead;
    while (current != NULL && current->id != jid) {
        current = current->next;
    }

    if (current == NULL) {
        puts("fg: job not found");
        return;
    }

    current->bgProcess = 0;

    if (current->state == STOPPED) {
        if (kill(current->pgid, SIGCONT) < 0) {
            puts("fg: could not continue process");
            return;
        }
        current->state = RUNNING;
    }

    signal(SIGTTOU, SIG_IGN);
    tcsetpgrp(STDIN_FILENO, getpgid(current->pgid));
    wpid = waitpid(current->pgid, &status, WUNTRACED);
    tcsetpgrp(STDIN_FILENO, getpgid(getpid()));

    if (wpid < 0) {
        puts(strerror(errno));
        return;
    }

    if (WIFEXITED(status)) {
        /* exited normally */
        current->state = COMPLETED;
    }

    if (WIFSTOPPED(status)) {
        current->state = STOPPED;
    }

    if (WIFSIGNALED(status)) {
        current->state = TERMINATED;
        current->termSig = WTERMSIG(status);
    }
}


void jobs(int numTokens) {
    if (numTokens > 1) {
        puts("jobs: too many arguments");
        return;
    }

    printJobs();
}

void killFunc(char **argv, int numTokens) {
    int jid;
    job *current;

    if (numTokens != 2) {
        puts("kill: wrong number of arguments");
        return;
    }

    if ((jid = stringToJobId(argv[1])) == -1) {
        puts("kill: invalid job id");
        return;
    }

    current = jobListHead;
    while (current != NULL && current->id != jid) {
        current = current->next;
    }

    if (current == NULL) {
        puts("kill: job not found");
        return;
    }

    if (kill(current->pgid, SIGTERM) < 0) {
        puts("kill: could not terminate job");
        return;
    }
}

int runCommand(char *command, int argc, char **argv, int bgProcess) {
    pid_t pid;

    sigset_t maskAll, maskOne, prevOne;

    sigfillset(&maskAll);
    sigemptyset(&maskOne);
    sigaddset(&maskOne, SIGCHLD);

    sigprocmask(SIG_BLOCK, &maskOne, &prevOne);

    pid = fork();

    /* child process */
    if (pid == 0) {
        sigprocmask(SIG_SETMASK, &prevOne, NULL);
        setpgid(0, 0);

        if (execv(command, argv) != 0) {
            if (argv[0][0] == '.' || argv[0][0] == '/') {
                printf("%s: No such file or directory\n", command);
            }
            exit(errno);
        }
    } else if (pid > 0) {
        int status;
        char *originalCommand;
        sigprocmask(SIG_BLOCK, &maskAll, NULL);

        setpgid(pid, pid);

        originalCommand = joinString(argc, argv);

        if (bgProcess) {
            /* create and add job to job list */
            job *j = createJob(strdup(command), originalCommand, pid, RUNNING, 1);
            addJob(j);

            printf("[%d] %d\n", j->id, pid);

            sigprocmask(SIG_SETMASK, &prevOne, NULL);
        } else {
            job *j;
            pid_t wpid;

            j = createJob(strdup(command), originalCommand, pid, RUNNING, 0);
            addJob(j);

            signal(SIGTTOU, SIG_IGN);
            tcsetpgrp(STDIN_FILENO, getpgid(pid));
            wpid = waitpid(pid, &status, WUNTRACED);
            tcsetpgrp(STDIN_FILENO, getpgid(getpid()));


            if (wpid < 0) {
                puts(strerror(errno));
                return -1;
            }

            if (WIFEXITED(status)) {
                /* exited normally */
                j->state = COMPLETED;
            }

            if (WIFSTOPPED(status)) {
                /* user pressed Ctrl-Z */
                j->state = STOPPED;
            }

            if (WIFSIGNALED(status)) {
                /* exited with some unhandled signal */
                j->state = TERMINATED;
                j->termSig = WTERMSIG(status);
            }
            sigprocmask(SIG_SETMASK, &prevOne, NULL);

        }
        return 0;
    }

    /* failed to fork */
    return -1;
}

int stringToJobId(char *string) {
    int i;
    int result = 0;

    /* make sure job id starts with % */
    if (string[0] != '%') {
        return -1;
    }

    for (i = 1; string[i]; i++) {
        if (!isdigit(string[i])) {
            return -1;
        }

        result *= 10;
        result += string[i] - '0';
    }

    return result;
}

char *joinString(int argc, char **argv) {
    char *result = NULL;
    int resultSize = 1; /* null terminator */
    int i;

    for (i = 0; i < argc; i++) {
        resultSize += strlen(argv[i]); /* length of each argument */
    }

    resultSize += (argc - 1); /* separators */

    result = malloc(resultSize * sizeof(char));
    result[0] = '\0';

    for (i = 0; i < argc; i++) {
        strcat(result, argv[i]);
        if (i < (argc - 1)) {
            strcat(result, " ");
        }
    }

    return result;
}

int fileExists(char *filename) {
    struct stat sb;

    if (stat(filename, &sb) != 0) {
        return 0;
    }

    if (S_ISDIR(sb.st_mode)) {
        return 0;
    }

    return (sb.st_mode & S_IXUSR) > 0;
}
