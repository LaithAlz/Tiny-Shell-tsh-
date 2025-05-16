/* 
 * tsh - A tiny shell program with job control
 * 
 */
 #include <stdio.h>
 #include <stdlib.h>
 #include <unistd.h>
 #include <string.h>
 #include <ctype.h>
 #include <signal.h>
 #include <sys/types.h>
 #include <sys/wait.h>
 #include <errno.h>
 #include <fcntl.h>
 
 /* Misc manifest constants */
 #define MAXLINE    1024   /* max line size */
 #define MAXARGS     128   /* max args on a command line */
 #define MAXJOBS      16   /* max jobs at any point in time */
 #define MAXCOMMANDS 10000 /* max length of each command */
 
 /* Job states */
 #define UNDEF 0 /* undefined */
 #define FG 1    /* running in foreground */
 #define BG 2    /* running in background */
 #define ST 3    /* stopped */
 
 /* 
  * Jobs states: FG (foreground), BG (background), ST (stopped)
  * Job state transitions and enabling actions:
  *     FG -> ST  : ctrl-z
  *     ST -> FG  : fg command
  *     ST -> BG  : bg command
  *     BG -> FG  : fg command
  * At most 1 job can be in the FG state.
  */
 
 /* Global variables */
 extern char **environ;      /* defined in libc */
 char prompt[] = "tsh> ";    /* command line prompt (DO NOT CHANGE) */
 int verbose = 0;            /* if true, print additional output */
 char sbuf[MAXLINE];         /* for composing sprintf messages */
 
 struct job_t {              /* Per-job data */
     pid_t pid;              /* job PID */
     int jid;                /* job ID [1, 2, ...] */
     int state;              /* UNDEF, FG, BG, or ST */
     char cmdline[MAXLINE];  /* command line */
 };
 struct job_t jobs[MAXJOBS]; /* The job list */
 
 volatile sig_atomic_t ready; /* Is the newest child in its own process group? */
 
 /* End global variables */
 
 
 /* Function prototypes */
 
 /* Here are the functions that you will implement */
 void eval(char *cmdline);
 int builtin_cmd(char **argv);
 void do_bgfg(char **argv);
 void waitfg(pid_t pid);
 void sigchld_handler(int sig);
 void sigint_handler(int sig);
 void sigtstp_handler(int sig);
 
 /* Here are helper routines that we've provided for you */
 int parseline(const char *cmdline, char **argv); 
 void sigquit_handler(int sig);
 void sigusr1_handler(int sig);
 
 void clearjob(struct job_t *job);
 void initjobs(struct job_t *jobs);
 int freejid(struct job_t *jobs); 
 int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline);
 int deletejob(struct job_t *jobs, pid_t pid); 
 pid_t fgpid(struct job_t *jobs);
 struct job_t *getjobpid(struct job_t *jobs, pid_t pid);
 struct job_t *getjobjid(struct job_t *jobs, int jid); 
 int pid2jid(pid_t pid); 
 void listjobs(struct job_t *jobs);
 
 void usage(void);
 void unix_error(char *msg);
 void app_error(char *msg);
 typedef void handler_t(int);
 handler_t *Signal(int signum, handler_t *handler);
 
 /*
  * main - The shell's main routine 
  */
 int main(int argc, char **argv) {
     char c;
     char cmdline[MAXLINE];
     int emit_prompt = 1; /* emit prompt (default) */
 
     /* Redirect stderr to stdout (so that driver will get all output
      * on the pipe connected to stdout) */
     dup2(STDOUT_FILENO, STDERR_FILENO);
 
     /* Parse the command line */
     while ((c = getopt(argc, argv, "hvp")) != -1) {
         switch (c) {
             case 'h':             /* print help message */
                 usage();
                 break;
             case 'v':             /* emit additional diagnostic info */
                 verbose = 1;
                 break;
             case 'p':             /* don't print a prompt */
                 emit_prompt = 0;  /* handy for automatic testing */
                 break;
             default:
                 usage();
         }
     }
 
     /* Install the signal handlers */
 
     Signal(SIGUSR1, sigusr1_handler); /* Child is ready */
 
     /* These are the ones you will need to implement */
     Signal(SIGINT,  sigint_handler);   /* ctrl-c */
     Signal(SIGTSTP, sigtstp_handler);  /* ctrl-z */
     Signal(SIGCHLD, sigchld_handler);  /* Terminated or stopped child */
 
     /* This one provides a clean way to kill the shell */
     Signal(SIGQUIT, sigquit_handler); 
 
     /* Initialize the job list */
     initjobs(jobs);
 
     /* Execute the shell's read/eval loop */
     while (1) {
 
         /* Read command line */
         if (emit_prompt) {
             printf("%s", prompt);
             fflush(stdout);
         }
         if ((fgets(cmdline, MAXLINE, stdin) == NULL) && ferror(stdin))
             app_error("fgets error");
         if (feof(stdin)) { /* End of file (ctrl-d) */
             fflush(stdout);
             exit(0);
         }
 
         /* Evaluate the command line */
         eval(cmdline);
         fflush(stdout);
     } 
 
     exit(0); /* control never reaches here */
 }
 
 
 
 
 void execute_command(char *argv[], int input_fd, int output_fd) {
     if (fork() == 0) { // Creating a child process
         if (input_fd != STDIN_FILENO) {
             dup2(input_fd, STDIN_FILENO); // Redirect input
             close(input_fd); // Close the original file descriptor
         }
         if (output_fd != STDOUT_FILENO) {
             dup2(output_fd, STDOUT_FILENO); // Redirect output
             close(output_fd); // Close the original file descriptor
         }
         execvp(argv[0], argv); // Execute the command
         perror("execvp"); // Only reached if execution fails
         exit(EXIT_FAILURE);
     }
 }
 
 void execute_pipeline(char ***commands, int num_commands) {
     int i, fd[2], in_fd = STDIN_FILENO;
 
     for (i = 0; i < num_commands - 1; ++i) { // Loop for each command except the last
         pipe(fd); // Create a new pipe
         execute_command(commands[i], in_fd, fd[1]); // Execute current command
         // Setup for the next command
         if (in_fd != STDIN_FILENO) close(in_fd); // Close the previous input if it's not stdin
         close(fd[1]); // Close the write end of the current pipe
         in_fd = fd[0]; // Use the read end of the current pipe as input for the next command
     }
 
     // Execute the last command
     execute_command(commands[num_commands - 1], in_fd, STDOUT_FILENO);
     if (in_fd != STDIN_FILENO) close(in_fd); // Close the last input if it's not stdin
 
     // Wait for all commands to finish
     while (num_commands-- > 0) wait(NULL);
 }
 
   
 /* 
  * eval - Evaluate the command line that the user has just typed in
  * 
  * If the user has requested a built-in command (quit, jobs, bg or fg)
  * then execute it immediately. Otherwise, fork a child process and
  * run the job in the context of the child. If the job is running in
  * the foreground, wait for it to terminate and then return.  Note:
  * each child process must have a unique process group ID so that our
  * background children don't receive SIGINT (SIGTSTP) from the kernel
  * when we type ctrl-c (ctrl-z) at the keyboard.  
 */
 void eval(char *cmdline) {
     char *argv[MAXARGS];
     int argc = parseline(cmdline, argv);
     if (argc == 0){
         return;
     }
 
     for(int i =0; i<argc; i++){
         printf("Argument %d:  %s \n", i, argv[i]);
     }
     int built_in = builtin_cmd(argv);
     if(built_in == 1){
         return;
     }
 
 
     // POPULATE STRING ARRAY
 
 
     char *curr_argv[MAXARGS];
     char **array_of_arrays_of_strings[MAXARGS];
     int pipe_counter = 0;
     char *pipe_str = "|";
     int curr_argv_counter = 0;
 
     for (int i = 0; i < argc; i++)
     {
         if (strcmp(pipe_str, argv[i]) == 0)
         {
             array_of_arrays_of_strings[pipe_counter] = malloc((curr_argv_counter + 1) * sizeof(char *));
             for (int j = 0; j < curr_argv_counter; j++)
             {
                 array_of_arrays_of_strings[pipe_counter][j] = curr_argv[j];
             }
             array_of_arrays_of_strings[pipe_counter][curr_argv_counter] = NULL;
 
             pipe_counter++;
             curr_argv_counter = 0;
         }
         else
         {
             curr_argv[curr_argv_counter++] = argv[i];
         }
     }
 
     array_of_arrays_of_strings[pipe_counter] = malloc((curr_argv_counter + 1) * sizeof(char *));
     for (int j = 0; j < curr_argv_counter; j++)
     {
         array_of_arrays_of_strings[pipe_counter][j] = curr_argv[j];
     }
     array_of_arrays_of_strings[pipe_counter][curr_argv_counter] = NULL;
 
     for (int i = 0; i < pipe_counter + 1; i++)
     {
         int j = 0;
         while (array_of_arrays_of_strings[i][j] != NULL)
         {
             printf("%s ", array_of_arrays_of_strings[i][j]);
             j++;
         }
         printf("\n");
         free(array_of_arrays_of_strings[i]);
     }
 
     char *input = NULL;
     char *output = NULL; 
 
     int outputFirst = 2; // 0 if input file first, 1 if output file first
 
 
     for(int i = 0; i < argc; i++) {
         if(strcmp(argv[i], "<") == 0) {
             input = argv[++i];
             if(outputFirst == 2){
                 outputFirst = 1;
             }
         } else if(strcmp(argv[i], ">") == 0) {
             output = argv[++i];
             if(outputFirst == 2){
                 outputFirst = 0;
             }
         }
     }
 
     if(pipe_counter > 0){
         execute_pipeline(array_of_arrays_of_strings, pipe_counter);
     }else{
 
     int inputFile;
     int outputFile;
 
     if(input != NULL && output != NULL){
 
         printf("Input file: %s\n", input);
         inputFile = open(input, O_RDONLY);
        
         if (inputFile < 0) {
             perror("Failed to open input file");
             exit(EXIT_FAILURE);
         }
 
         printf("Output file: %s\n", output);
         outputFile = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
 
         if (outputFile < 0) {
             perror("Failed to open input file");
             exit(EXIT_FAILURE);
         }
 
         if(outputFirst == 1){
             printf("output first");
             if(dup2(outputFile, STDOUT_FILENO) < 0){
                 perror("failed to redirect stdin");
                 close(inputFile);
                 exit(EXIT_FAILURE);
             }
             close(outputFile);
 
 
             if(dup2(inputFile, STDIN_FILENO) < 0){
                 perror("failed to redirect stdin");
                 close(inputFile);
                 exit(EXIT_FAILURE);
             }
             close(inputFile);
 
         }else{
             printf("input first");
 
             if(dup2(inputFile, STDIN_FILENO) < 0){
                 perror("failed to redirect stdin");
                 close(inputFile);
                 exit(EXIT_FAILURE);
             }
             close(inputFile);
 
             if(dup2(outputFile, STDOUT_FILENO) < 0){
                 perror("failed to redirect stdin");
                 close(inputFile);
                 exit(EXIT_FAILURE);
             }
             close(outputFile);
 
         }
 
     }
 
 
     if(input != NULL) {
         printf("Input file: %s\n", input);
         inputFile = open(input, O_RDONLY);
        
         if (inputFile < 0) {
             perror("Failed to open input file");
             exit(EXIT_FAILURE);
         }
 
 
         if(dup2(inputFile, STDIN_FILENO) < 0){
             perror("failed to redirect stdin");
             close(inputFile);
             exit(EXIT_FAILURE);
         }
         close(inputFile);
 
 
     } else {
         printf("No input redirection found.\n");
     }
 
 
     if(output != NULL) {
         printf("Output file: %s\n", output);
         outputFile = open(output, O_WRONLY | O_CREAT | O_TRUNC, 0644);
 
         if (outputFile < 0) {
             perror("Failed to open input file");
             exit(EXIT_FAILURE);
         }
 
         if(dup2(outputFile, STDOUT_FILENO) < 0){
             perror("failed to redirect stdin");
             close(inputFile);
             exit(EXIT_FAILURE);
         }
         close(outputFile);
 
 
     } else {
         printf("No output redirection found.\n");
     }
 
 
 
 
 
 
 
     int state = 0;
 
     char *last = argv[argc-1];
     if(strcmp(last, "&") == 0){
         argv[argc-1] = NULL;
         argc -= 1;
         state = BG;
         printf("BACKGROUND JOB");
     }else{
         state = FG;
     }
     
     sigset_t mask, old_set;
 
     sigemptyset(&mask);
     sigaddset(&mask, SIGCHLD);
     sigaddset(&mask, SIGINT);
     sigaddset(&mask, SIGTSTP);
 
     if (sigprocmask(SIG_BLOCK, &mask, &old_set) < 0) {
         perror("sigprocmask");
     } else {
         // printf("SIGCHLD, SIGINT, SIGTSTP blocked.\n");
     }
 
 
 
 
     if(built_in == 0){ // Not Built In
 
 
         int fd[2];
         if(pipe(fd) == -1){
             perror("pipe");
             exit(1);
         }
         
 
 
         pid_t pid = fork(); 
 
         if (pid == 0) { // CHILD PROCESS
 
         
 
 
 
             if (sigprocmask(SIG_UNBLOCK, &mask, NULL) < 0) {
                 perror("sigprocmask");
             } else {
                 // printf("Signals unblocked in child.\n"); // signal unblocked successfully
             }        
 
 
             setpgid(0, 0);
             kill(getppid(), SIGUSR1);
 
         
 
     
             Signal(SIGINT, SIG_DFL);
             Signal(SIGTSTP, SIG_DFL);
             Signal(SIGCHLD, SIG_DFL);
             Signal(SIGQUIT, SIG_DFL);
             Signal(SIGUSR1, SIG_DFL);
 
 
             execv(argv[0], argv);
             printf("%s", argv);
             perror("execv");
             exit(1);
 
 
 
         }else if(pid < 0){
             perror("ERROR FORKING");
         }else{ // PARENT PROCESS
 
 
 
             
 
             sigsuspend(&mask);
             int new_job = addjob(jobs, pid, state, cmdline);
 
             
 
             if(state == FG){ // if foreground job
                 int status;
 
 
             
                 if (sigprocmask(SIG_UNBLOCK, &mask, NULL)  == -1) {
                     perror("sigprocmask");
                 } else {
                     // printf("Signals unblocked in parent.\n");
 
                 }
                 waitfg(fgpid(jobs));
 
 
 
             }
 
             
             if(state == BG) { // background job
                 printf("[%d] %d %s", getjobpid(jobs, (pid_t) pid)->jid, pid, cmdline);
             } 
             
         }
 
         
         
     }
     }
     return;
 }
 
 /* 
  * parseline - Parse the command line and build the argv array.
  * 
  * Characters enclosed in single quotes are treated as a single
  * argument.  Return number of arguments parsed.
  */
 int parseline(const char *cmdline, char **argv) {
     static char array[MAXLINE]; /* holds local copy of command line */
     char *buf = array;          /* ptr that traverses command line */
     char *delim;                /* points to space or quote delimiters */
     int argc;                   /* number of args */
 
     strcpy(buf, cmdline);
     buf[strlen(buf)-1] = ' ';  /* replace trailing '\n' with space */
     while (*buf && (*buf == ' ')) /* ignore leading spaces */
         buf++;
 
     /* Build the argv list */
     argc = 0;
     if (*buf == '\'') {
         buf++;
         delim = strchr(buf, '\'');
     }
     else {
         delim = strchr(buf, ' ');
     }
 
     while (delim) {
         argv[argc++] = buf;
         *delim = '\0';
         buf = delim + 1;
         while (*buf && (*buf == ' ')) /* ignore spaces */
             buf++;
 
         if (*buf == '\'') {
             buf++;
             delim = strchr(buf, '\'');
         }
         else {
             delim = strchr(buf, ' ');
         }
     }
     argv[argc] = NULL;
     
     return argc;
 }
 
 
 
 /* 
  * builtin_cmd - If the user has typed a built-in command then execute
  *    it immediately.  
  */
 int builtin_cmd(char **argv) {
     if(strcmp(argv[0], "quit") == 0){         // IF COMMAND IS "quit"
 
         // GET FOREGROUND JOB ID AND SEND SIGNAL TO TERMINATE
         // pid_t fpid =  fgpid(jobs);
         // kill(-fpid, SIGQUIT);
         exit(0);
 
         return 1;
 
 
     }else if(strcmp(argv[0], "jobs") == 0){        // IF COMMAND IS "jobs"
 
         // int i;
         // int counter = 0;
         // for (i = 0; i < MAXJOBS; i++) {
         //     if (jobs[i].pid != 0) {
         //         if(jobs[i].state == BG){
         //             printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
         //             counter += 1;
         //         }
         //         printf("%s", jobs[i].cmdline);
         //     }
         // }
 
         // if(counter == 0){
         //     printf("No background jobs running.\n");
         // }
         listjobs(jobs);
         return 1;
 
 
     }else if(strcmp(argv[0], "bg") == 0){ // FIX, bg <job> not bg
 
         do_bgfg(argv);
         
         return 1;
 
 
     }else if(strcmp(argv[0], "fg") == 0){
         do_bgfg(argv);            
 
         return 1;
     }else{
         return 0;     /* not a builtin command */
 
     }
 
 }
 
 /* 
  * do_bgfg - Execute the builtin bg and fg commands
  */
 void do_bgfg(char **argv)
 {
     char *first_arg = argv[0];
     char *job_arg = argv[1];
 
 
 
     if (strcmp(first_arg, "bg") == 0)
     {
         errno = 0;
         int job_pid_or_jid = strtol(job_arg, NULL, 10);
 
         if (errno != 0)
         {
             fprintf(stderr, "Error: [-d delay] must be an integer value\n");
             exit(1);
         }
 
         struct job_t *curr_job_if_pid = getjobpid(jobs, job_pid_or_jid);
         struct job_t *curr_job_if_jid = getjobjid(jobs, job_pid_or_jid);
 
         if (curr_job_if_pid != NULL)
         {
             // IT IS A PID, SEND THE SIGNAL
             kill(job_pid_or_jid, SIGCONT);
 
             // FLAG AS BACKGROUND JOB IN STRUCT
             curr_job_if_pid->state = BG;
         }
         else if (curr_job_if_jid != NULL)
         {
             // IT IS A JID, SEND THE SIGNAL
             pid_t pid = curr_job_if_jid->pid;
             kill(pid, SIGCONT);
 
             // FLAG AS BACKGROUND JOB IN STRUCT
             curr_job_if_jid->state = BG;
         }
     }
 else if (strcmp(first_arg, "fg") == 0)
     {
         errno = 0;
         int job_pid_or_jid = strtol(job_arg, NULL, 10);
 
         if (errno != 0)
         {
             fprintf(stderr, "Error: [-d delay] must be an integer value\n");
             exit(1);
         }
 
         struct job_t *curr_job_if_pid = getjobpid(jobs, job_pid_or_jid);
         struct job_t *curr_job_if_jid = getjobjid(jobs, job_pid_or_jid);
 
         if (curr_job_if_pid != NULL)
         {
             // WAIT FIRST FOR CURRENT FOREGROUND TO FINISH, IF EXISTING
             if (fgpid(jobs) != 0)
             {
                 waitfg(fgpid(jobs));
             }
 
             // IT IS A PID, SEND THE SIGNAL TO RESTART
             kill(job_pid_or_jid, SIGCONT);
 
             // FLAG AS FOREGROUND JOB IN STRUCT
             curr_job_if_pid->state = FG;
 
             // WAIT FOR IT TO FINISH BEFORE GIVING COMMANDLINE ACCESS
             if (fgpid(jobs) != 0)
             {
                 waitfg(fgpid(jobs));
             }
         }
         else if (curr_job_if_jid != NULL)
         {
             // WAIT FIRST FOR CURRENT FOREGROUND TO FINISH, IF EXISTING
             if (fgpid(jobs) != 0)
             {
                 waitfg(fgpid(jobs));
             }
 
             // IT IS A JID, SEND THE SIGNAL TO RESTART
             pid_t pid = curr_job_if_jid->pid;
             kill(pid, SIGCONT);
 
             // FLAG AS FOREGROUND JOB IN STRUCT
             curr_job_if_jid->state = FG;
 
             // WAIT FOR IT TO FINISH BEFORE GIVING COMMANDLINE ACCESS
             if (fgpid(jobs) != 0)
             {
                 waitfg(fgpid(jobs));
             }
         }
         else
         {
             perror("non-existent PID/JID");
         }
     }
 
     return;
 }
 
 
 
 
 /* 
  * waitfg - Block until process pid is no longer the foreground process
  */
 void waitfg(pid_t pid) {
 
 
 
     int foreground_job_id = fgpid(jobs);
     struct job_t *job = getjobpid(jobs, pid);
 
     sigset_t mask;
     sigemptyset(&mask);
     // sigaddset(&mask, SIGCHLD);
     
     while(sigsuspend(&mask)){
         if(errno == EFAULT){
             perror("sigsuspend waitfg");
             exit(1);
         }
 
         if(pid != fgpid(jobs)){
             return;
         }
     }
 
 
     // while (job->state != FG){
     //     sigsuspend(&mask);
     //     printf("Were waiting");
     // }
 
     printf("done waiting");
 
 
 
     return;
 
 
 }
 
 
 /*****************
  * Signal handlers
  *****************/
 
 /* 
  * sigchld_handler - The kernel sends a SIGCHLD to the shell whenever
  *     a child job terminates (becomes a zombie), or stops because it
  *     received a SIGSTOP or SIGTSTP signal. The handler reaps all
  *     available zombie children, but doesn't wait for any other
  *     currently running children to terminate.  
  */
 void sigchld_handler(int sig) {
     pid_t pid;
     int status;
 
     while ((pid = waitpid(-1, &status, WNOHANG | WUNTRACED)) > 0) {
         struct job_t *job = getjobpid(jobs, pid);
         
         if (WIFSTOPPED(status)) { // SIGSTP
             printf("Job [%d] (%d) stopped by signal %d (SIGSTP) \n", pid2jid(pid), pid, WSTOPSIG(status));
             job->state = ST; 
 
         } else if (WIFSIGNALED(status)) { // SIGINT
             printf("Job [%d] (%d) terminated by signal %d (SIGINT) \n", pid2jid(pid), pid, WTERMSIG(status));
             deletejob(jobs, pid); 
 
         } else if (WIFEXITED(status)) { // FINISHED
             printf("Job [%d] (%d) exited with status %d (FINISHED) \n", pid2jid(pid), pid, WEXITSTATUS(status));
             deletejob(jobs, pid); 
         }
     }
 
     if (pid < 0 && errno != ECHILD) {
         perror("waitpid error");
         exit(1);
     }
 
     return;
 }
 
 
 /* 
  * sigint_handler - The kernel sends a SIGINT to the shell whenever the
  *    user types ctrl-c at the keyboard.  Catch it and send it along
  *    to the foreground job.  
  */
 void sigint_handler(int sig) {
     // printf("done\n");
 
     pid_t fpid =  fgpid(jobs); // FOREGROUND JOB ID
 
     if(fpid != 0){
         kill(-fpid, SIGINT);
 
     }else{
     }
 
     return;
 }
 
 /*
  * sigtstp_handler - The kernel sends a SIGTSTP to the shell whenever
  *     the user types ctrl-z at the keyboard. Catch it and suspend the
  *     foreground job by sending it a SIGTSTP.  
  */
 void sigtstp_handler(int sig) {
     
     pid_t fpid =  fgpid(jobs); // FOREGROUND JOB ID 
     struct job_t *job = getjobpid(jobs, fpid); // FOREGROUND JOB ADDRESS
 
     if(fpid != 0){
         printf("called\n");
         kill(-fpid, SIGTSTP); 
 
     }else{
         printf("fpid = 0");
     }
 
     return;
 }
 
 /*
  * sigusr1_handler - child is ready
  */
 void sigusr1_handler(int sig) {
     ready = 1;
 }
 
 
 /*********************
  * End signal handlers
  *********************/
 
 /***********************************************
  * Helper routines that manipulate the job list
  **********************************************/
 
 /* clearjob - Clear the entries in a job struct */
 void clearjob(struct job_t *job) {
     job->pid = 0;
     job->jid = 0;
     job->state = UNDEF;
     job->cmdline[0] = '\0';
 }
 
 /* initjobs - Initialize the job list */
 void initjobs(struct job_t *jobs) {
     int i;
 
     for (i = 0; i < MAXJOBS; i++)
         clearjob(&jobs[i]);
 }
 
 /* freejid - Returns smallest free job ID */
 int freejid(struct job_t *jobs) {
     int i;
     int taken[MAXJOBS + 1] = {0};
     for (i = 0; i < MAXJOBS; i++)
         if (jobs[i].jid != 0) 
         taken[jobs[i].jid] = 1;
     for (i = 1; i <= MAXJOBS; i++)
         if (!taken[i])
             return i;
     return 0;
 }
 
 /* addjob - Add a job to the job list */
 int addjob(struct job_t *jobs, pid_t pid, int state, char *cmdline) {
     int i;
     
     if (pid < 1)
         return 0;
     int free = freejid(jobs);
     if (!free) {
         printf("Tried to create too many jobs\n");
         return 0;
     }
     for (i = 0; i < MAXJOBS; i++) {
         if (jobs[i].pid == 0) {
             jobs[i].pid = pid;
             jobs[i].state = state;
             jobs[i].jid = free;
             strcpy(jobs[i].cmdline, cmdline);
             if(verbose){
                 printf("Added job [%d] %d %s\n", jobs[i].jid, jobs[i].pid, jobs[i].cmdline);
             }
             return 1;
         }
     }
     return 0; /*suppress compiler warning*/
 }
 
 /* deletejob - Delete a job whose PID=pid from the job list */
 int deletejob(struct job_t *jobs, pid_t pid) {
     int i;
 
     if (pid < 1)
         return 0;
 
     for (i = 0; i < MAXJOBS; i++) {
         if (jobs[i].pid == pid) {
             clearjob(&jobs[i]);
             return 1;
         }
     }
     return 0;
 }
 
 /* fgpid - Return PID of current foreground job, 0 if no such job */
 pid_t fgpid(struct job_t *jobs) {
     int i;
 
     for (i = 0; i < MAXJOBS; i++)
         if (jobs[i].state == FG)
             return jobs[i].pid;
     return 0;
 }
 
 /* getjobpid  - Find a job (by PID) on the job list */
 struct job_t *getjobpid(struct job_t *jobs, pid_t pid) {
     int i;
 
     if (pid < 1)
         return NULL;
     for (i = 0; i < MAXJOBS; i++)
         if (jobs[i].pid == pid)
             return &jobs[i];
     return NULL;
 }
 
 /* getjobjid  - Find a job (by JID) on the job list */
 struct job_t *getjobjid(struct job_t *jobs, int jid) 
 {
     int i;
 
     if (jid < 1)
         return NULL;
     for (i = 0; i < MAXJOBS; i++)
         if (jobs[i].jid == jid)
             return &jobs[i];
     return NULL;
 }
 
 /* pid2jid - Map process ID to job ID */
 int pid2jid(pid_t pid) {
     int i;
 
     if (pid < 1)
         return 0;
     for (i = 0; i < MAXJOBS; i++)
         if (jobs[i].pid == pid) {
             return jobs[i].jid;
     }
     return 0;
 }
 
 /* listjobs - Print the job list */
 void listjobs(struct job_t *jobs) {
     int i;
     
     for (i = 0; i < MAXJOBS; i++) {
         if (jobs[i].pid != 0) {
             printf("[%d] (%d) ", jobs[i].jid, jobs[i].pid);
             switch (jobs[i].state) {
                 case BG: 
                     printf("Running ");
                     break;
                 case FG: 
                     printf("Foreground ");
                     break;
                 case ST: 
                     printf("Stopped ");
                     break;
                 default:
                     printf("listjobs: Internal error: job[%d].state=%d ", 
                        i, jobs[i].state);
             }
             printf("%s", jobs[i].cmdline);
         }
     }
 }
 /******************************
  * end job list helper routines
  ******************************/
 
 
 /***********************
  * Other helper routines
  ***********************/
 
 /*
  * usage - print a help message and terminate
  */
 void usage(void) {
     printf("Usage: shell [-hvp]\n");
     printf("   -h   print this message\n");
     printf("   -v   print additional diagnostic information\n");
     printf("   -p   do not emit a command prompt\n");
     exit(1);
 }
 
 /*
  * unix_error - unix-style error routine
  */
 void unix_error(char *msg) {
     fprintf(stdout, "%s: %s\n", msg, strerror(errno));
     exit(1);
 }
 
 /*
  * app_error - application-style error routine
  */
 void app_error(char *msg) {
     fprintf(stdout, "%s\n", msg);
     exit(1);
 }
 
 /*
  * Signal - wrapper for the sigaction function
  */
 handler_t *Signal(int signum, handler_t *handler) {
     struct sigaction action, old_action;
 
     action.sa_handler = handler;  
     sigemptyset(&action.sa_mask); /* block sigs of type being handled */
     action.sa_flags = SA_RESTART; /* restart syscalls if possible */
 
     if (sigaction(signum, &action, &old_action) < 0)
         unix_error("Signal error");
     return (old_action.sa_handler);
 }
 
 /*
  * sigquit_handler - The driver program can gracefully terminate the
  *    child shell by sending it a SIGQUIT signal.
  */
 void sigquit_handler(int sig) {
     printf("Terminating after receipt of SIGQUIT signal\n");
     exit(1);
 }
 
 
 