/* timelog-plus.c
 * Enhanced time logger
 */

#include <sys/types.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <termios.h>
#include <signal.h>
#include <ctype.h>


/* Constants */
#define TASK_STATE_NEW  0    /* New task */
#define TASK_STATE_CUR  1    /* Active task */
#define TASK_STATE_DONE 2    /* Completed task */

#define BUF_CHUNK 256        /* Input buffer allocation size */


/* Macros */
#define make_hms(t,h,m,s) ((s) = (t) % 60, (m) = ((t)/60) % 60, (h)=(t)/3600)
#define scan_hms(t,h,m,s) (t) = ((h) * 3600) + ((m) * 60) + (s)
#define look_mon(x,m) for (m = 11; m >= 0 && strcasecmp(x,months[m]); m--)


/* The task information structure */
typedef struct task_info {
  struct task_info *next;        /* Pointer to next item on stack */
  char *desc;                    /* Description of this task */
  off_t where;                   /* Location in logfile */
  time_t start_time;             /* Start time of task */
  unsigned long time_logged;     /* Time logged so far */
} task_info;


/* Globals */
static struct termios orig_tty_settings;
task_info *stack;                /* Current task stack pointer */
FILE *timelog_file;              /* Logfile pointer */
char *timelog_path;              /* Path to logfile */
time_t entry_time;               /* Time we entered the current task */
time_t min_entry_time;           /* Minimum time user may enter */
int is_break;                    /* Currnet task is a break (not on stack) */
int task_depth;                  /* Depth of task stack */
int hangup;                      /* Set if we're shutting down */

int f_continuous_time = 1;       /* Display current time iff set */

char *months[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun",
                   "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };


/* Safe memory allocation */
void *xmalloc(size_t size, void *oldptr)
{
  void *result;

  result = oldptr ? realloc(oldptr, size) : malloc(size);
  if (!result) {
    fprintf(stderr, "\nMemory allocation failed!\n");
    exit(1);
  }
  return result;
}


/* Write the current state of a task to the log file */
void log_task(task_info *task, unsigned long add_time, int state)
{
  time_t now = time(0);
  struct tm tm_now = *(localtime(&now));
  struct tm tm_start = *(localtime(&task->start_time));
  unsigned long total_time = task->time_logged + add_time;
  int hh, mm, ss;


  if (state == TASK_STATE_NEW) {
    fseek(timelog_file, 0, SEEK_END);
    task->where = ftell(timelog_file);
  } else {
    fseek(timelog_file, task->where, SEEK_SET);
  }

  make_hms(total_time, hh, mm, ss);
  fprintf(timelog_file, "%02d-%3s-%04d %c %02d:%02d:%02d-%02d:%02d:%02d"
                        "  %3d:%02d:%02d  %-1.40s\n",
    tm_start.tm_mday, months[tm_start.tm_mon], tm_start.tm_year+1900,
    state == TASK_STATE_DONE ? '*' : '_',
    tm_start.tm_hour, tm_start.tm_min, tm_start.tm_sec,
    tm_now.tm_hour,   tm_now.tm_min,   tm_now.tm_sec,
    hh, mm, ss, task->desc);
  fflush(timelog_file);
}


/* Scan the log file to reconstruct the task stack */
void log_scan(void)
{
  int sDD, sYY, shh, smm, sss, hh, mm, ss, n;
  char x[4], desc[41], state;
  struct tm when;
  task_info *task;
  off_t where;

  stack = 0;
  task_depth = 0;
  fseek(timelog_file, 0, SEEK_SET);
  for (;;) {
    where = ftell(timelog_file);
    n = fscanf(timelog_file, "%d-%3s-%d %c %d:%d:%d-%*d:%*d:%*d %d:%d:%d %[^\n]\n",
               &sDD, x, &sYY, &state, &shh, &smm, &sss, &hh, &mm, &ss, desc);
#ifdef DEBUG
    printf("Read %d fields: %s", n, n < 11 ? "Done!\n" : "");
#endif
    if (n < 11) break;
#ifdef DEBUG
    printf("%02d-%s-%04d %c %02d:%02d:%02d %3d:%02d:%02d %s\n",
           sDD, x, sYY, state, shh, smm, sss, hh, mm, ss, desc);
#endif
    if (state == '*') continue;

    task = (task_info *)xmalloc(sizeof(task_info), 0);
    task->next = stack;
    task->desc = xmalloc(strlen(desc) + 1, 0);
    strcpy(task->desc, desc);
    task->where = where;
    scan_hms(task->time_logged, hh, mm, ss);

    when.tm_sec  = sss;
    when.tm_min  = smm;
    when.tm_hour = shh;
    when.tm_mday = sDD;
    look_mon(x, when.tm_mon);
    when.tm_year = sYY - 1900;
    task->start_time = mktime(&when);
    stack = task;
    task_depth++;
  }
  entry_time = time(0);
  if (stack) {
    min_entry_time = stack->start_time + stack->time_logged;
    is_break = 0;
  } else {
    min_entry_time = 0;
    is_break = 1;
    task_depth = 1;
  }
#ifdef DEBUG
  printf("After preload: task_depth=%d, is_break=%d\n", task_depth, is_break);
#endif
}


/* Write a prompt for the current task */
void write_prompt(task_info *task)
{
  static time_t last_update = 0;
  time_t now = time(0);
  unsigned long add_time = now - entry_time;
  unsigned long total_time = (task ? task->time_logged : 0) + add_time;
  struct tm now_tm = *(localtime(&now));
  char *arrow = xmalloc(task_depth + 1, 0);
  int hh, mm, ss;

  memset(arrow, '-', task_depth); arrow[task_depth] = 0;
  make_hms(total_time,hh,mm,ss);
  printf("\r%02d:%02d [%d:%02d:%02d] %s %s> ",
         now_tm.tm_hour, now_tm.tm_min, hh, mm, ss,
         task ? task->desc : "break", arrow);
  if (task && now - last_update >= 60) {
    log_task(task, add_time, TASK_STATE_CUR);
    last_update = now;
  }
  fflush(stdout);
  free(arrow);
}


/* Write out the list of tasks */
void write_stack(void)
{
  struct task_info *T;
  struct tm start;
  int hh, mm, ss;

  for (T = stack; T; T = T->next) {
    start = *(localtime(&T->start_time));
    make_hms(T->time_logged, hh, mm, ss);
    printf(">> %02d:%02d [%d:%02d:%02d] %s\n",
           start.tm_hour, start.tm_min, hh, mm, ss, T->desc);
  }
  fflush(stdout);
}


/* Update the task stack and logfile based on some input
 */
void update_tasks(char *text)
{
  time_t now = time(0);
  time_t when = now;
  struct tm tm_when = *(localtime(&when));
  struct task_info *task;
  unsigned long add_time;
  int hh, mm, finish, start_break;
  char *x;

  if (text) {
    /* Strip leading and trailing whitespace */
    while (*text && strchr(" \t\r\n", *text)) text++;
    x = text + strlen(text) - 1;
    while (x >= text && strchr(" \t\r\n", *x)) *x-- = 0;

    /* Parse a time specifier, if present */
    if (*text == '@' && sscanf(text, "@%d:%d ", &hh, &mm) == 2) {
      if (hh >  tm_when.tm_hour
      || (hh == tm_when.tm_hour && mm > tm_when.tm_min))
        tm_when.tm_mday--;
      tm_when.tm_hour = hh;
      tm_when.tm_min  = mm;
      tm_when.tm_sec  = 0;
      when = mktime(&tm_when);
      text++;                        /* @ */
      while (isdigit(*text)) text++; /* hour */
      text++;                        /* : */
      while (isdigit(*text)) text++; /* min */
      while (*text == ' ' || *text == '\t') text++;
#ifdef DEBUG
      printf("min = %ld, when = %ld, max = %ld\n", min_entry_time, when, now);
#endif
      if (when < min_entry_time || when > now) {
        /* Can't record an event earler than the entry time */
        fputc(7, stdout);
        fflush(stdout);
        return;
      }
    }
#ifdef DEBUG
    printf("Time is %02d:%02d:%02d; text is /%s/\n",
           tm_when.tm_hour, tm_when.tm_min, tm_when.tm_sec, text);
#endif

    /* Now, what is the event? */
    finish = start_break = 0;
    if (!*text) finish = 1;
    else if (!strcmp(text, ".") || !strcasecmp(text, "break")) start_break = 1;
  } else {
#ifdef DEBUG
    printf("Time is %02d:%02d:%02d; no text\n",
           tm_when.tm_hour, tm_when.tm_min, tm_when.tm_sec);
#endif
    finish = 0;
    start_break = 1;
  }

  /* If the current task is not a break, log time on it */
  if (!is_break && when >= entry_time) {
    stack->time_logged += when - entry_time;
    log_task(stack, 0, finish ? TASK_STATE_DONE : TASK_STATE_CUR);
  }

  min_entry_time = entry_time = when;
  add_time = now - entry_time;

#ifdef DEBUG
  printf("ACTION: finish=%d, start_break=%d\n", finish, start_break);
  printf("BEFORE: task_depth=%d, is_break=%d\n", task_depth, is_break);
#endif

  /* Take some action... */
  if (finish) {                       /* Finish a task (or break) */
    task_depth--;
    if (is_break) is_break = 0;
    else {
      task = stack;
      stack = task->next;
      free(task->desc);
      free(task);
      if (!stack) is_break = task_depth = 1;
    }
  } else {                            /* Start a task (or break) */
    if (!is_break) task_depth++;
    if (start_break) is_break = 1;
    else {
      task = (task_info *)xmalloc(sizeof(task_info), 0);
      task->next = stack;
      task->desc = xmalloc(strlen(text + 1), 0);
      strcpy(task->desc, text);
      task->start_time = when;
      task->time_logged = 0;
      log_task(task, 0, TASK_STATE_NEW);
      stack = task;
      is_break = 0;
    }
  }
#ifdef DEBUG
  printf("AFTER:  task_depth=%d, is_break=%d\n", task_depth, is_break);
#endif
}


/* Read an arbitrary-length line */
char *readln(task_info *task)
{
  static char *buf = 0;
  static int buf_size = 0;
  int input_len = 0, x_offset = 0, n, ext_prefix = 0;
  char *x = buf;
  unsigned char c;
  struct timeval tv;
  task_info *T;
  fd_set set;

  /* Wait for input to be ready, periodically updating the prompt */
  while (!hangup) {
    FD_ZERO(&set);
    FD_SET(0, &set);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    n = select(1, &set, 0, 0, &tv);
    if (n > 0) break;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      fprintf(stderr, "\nselect() failed: %s\n", strerror(errno));
      exit(1);
    }
    write_prompt(task);
  }

  /* Read the user's input */
  while (!hangup) {
    if (input_len >= buf_size - 1) {
      buf_size += BUF_CHUNK;
      buf = xmalloc(buf_size, buf);
    }
    buf[input_len] = 0;

    FD_ZERO(&set);
    FD_SET(0, &set);
    tv.tv_sec = 1;
    tv.tv_usec = 0;
    n = select(1, &set, 0, 0, f_continuous_time ? &tv : 0);
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      fprintf(stderr, "\nselect() failed: %s\n", strerror(errno));
      exit(1);
    }
    write_prompt(task);
    fputs(buf, stdout);
    for (n = x_offset; n < input_len; n++) fputc('\b', stdout);
    fflush(stdout);
    if (!FD_ISSET(0, &set)) continue;

    n = read(0, &c, 1);
    if (!n) continue;
    if (n < 0 && errno == EINTR) continue;
    if (n < 0) {
      fprintf(stderr, "\nread() failed: %s\n", strerror(errno));
      exit(1);
    }

    if (ext_prefix) {
      ext_prefix = 0;
      switch (c) {
        case 't': f_continuous_time = !f_continuous_time; continue;
        case 's':
          fputc('\n', stdout);
          write_stack();
          write_prompt(task);
          fputs(buf, stdout);
          for (n = x_offset; n < input_len; n++) fputc('\b', stdout);
          fflush(stdout);
          continue;
          
        default:
          fputc('\a', stdout);
          fflush(stdout);
          continue;
      }
    }

    switch (c) {
      case  1:  /* ^A - to BOL */
        while (x_offset--) fputc('\b', stdout);
        fflush(stdout);
        x_offset = 0;
        continue;

      case  5:  /* ^E - to EOL */
        fputs(buf + x_offset, stdout);
        fflush(stdout);
        x_offset = input_len;
        continue;

      case  2:  /* ^B - backward */
        if (!x_offset) continue;
        fputc('\b', stdout); fflush(stdout);
        x_offset--;
        continue;

      case  6:  /* ^F - forward */
        if (x_offset == input_len) continue;
        fputc(buf[x_offset++], stdout);
        fflush(stdout);
        continue;

      case 0x7f: /* DEL - backspace */
      case  8:  /* ^H - backspace */
        if (!x_offset) continue;
        fputc('\b', stdout);
        x_offset--;
        /* fall through... */

      case  4:  /* ^D - delete char */
        if (!x_offset && !input_len) {
          /* ^D on an empty line begins a break */
          fputc('\n', stdout);
          fflush(stdout);
          return 0;
        }
        if (x_offset == input_len) continue;
        fputs(buf + x_offset + 1, stdout);
        fputc(' ', stdout);
        for (n = x_offset; n < input_len; n++) {
          fputc('\b', stdout);
          buf[n] = buf[n+1];
         }
        fflush(stdout);
        input_len--;
        continue;

      case 11:  /* ^K - kill to EOL */
        for (n = x_offset; n < input_len; n++) fputc(' ', stdout);
        for (n = x_offset; n < input_len; n++) fputc('\b', stdout);
        fflush(stdout);
        input_len = x_offset;
        buf[x_offset] = 0;
        continue;

      case 10:  /* ^J - newline */
      case 13:  /* ^M - newline */
        fputs(buf + x_offset, stdout);
        fputc('\n', stdout);
        fflush(stdout);
        return buf;

      case 24:  /* ^X - extended command */
        ext_prefix = 1;
        continue;

      default:  /* self-insert */
        if (c < ' ' || c > 0x7f) continue; /* Only printables */
        for (n = input_len; n >= x_offset; n--) buf[n+1] = buf[n];
        buf[x_offset] = c;
        fputs(buf + x_offset, stdout);
        x_offset++; input_len++;
        for (n = input_len; n > x_offset; n--) fputc('\b', stdout);
        fflush(stdout);
        continue;
    }
  }
  fputc('\n', stdout);
  fflush(stdout);
  return 0; // hangup
}


void gone(int sig)
{
  if (sig == SIGUSR1) hangup = 1;
  else hangup = -1;
}

void tty_setup(int sig)
{
  struct termios tty_settings;

  tcgetattr(0, &tty_settings);
  orig_tty_settings = tty_settings;
  tty_settings.c_oflag |=  OPOST;
  tty_settings.c_oflag &=~ OCRNL;
  tty_settings.c_lflag |=  ISIG;
  tty_settings.c_lflag &=~ (ICANON | IEXTEN | ECHO);
  tty_settings.c_cc[VMIN] = 1;
  tty_settings.c_cc[VTIME] = 0;
  tcsetattr(0, TCSANOW, &tty_settings);
}


/* MAIN PROGRAM */
void main(int argc, char **argv)
{
  struct sigaction action;
  char *text;
  int log_fd;

  if (argc != 2) {
    fprintf(stderr, "Usage: timelog <logfile>\n");
    exit(1);
  }

  memset(&action, 0, sizeof(action));
  action.sa_handler = tty_setup;
  action.sa_flags = SA_RESTART;
  sigaction(SIGCONT, &action, 0);
  tty_setup(0);

  action.sa_handler = gone;
  sigaction(SIGHUP, &action, 0);
  sigaction(SIGINT, &action, 0);
  sigaction(SIGTERM, &action, 0);
  sigaction(SIGUSR1, &action, 0);

  timelog_path = argv[1];
  log_fd = open(timelog_path, O_RDWR | O_CREAT, 0644);
  if (log_fd < 0) {
    fprintf(stderr, "open(%s) failed: %s\n", timelog_path, strerror(errno));
    exit(1);
  }

  timelog_file = fdopen(log_fd, "r+");
  if (!timelog_file) {
    fprintf(stderr, "fdopen() failed!\n");
    exit(1);
  }

  log_scan();
  while ((stack || is_break) && !hangup) {
    write_prompt(is_break ? 0 : stack);
    text = readln(is_break ? 0 : stack);
#ifdef DEBUG
    if (text) printf("Got /%s/\n", text);
    else printf("Got ^D\n");
#endif
    update_tasks(text);
    if (hangup > 0) hangup = 0;
  }
  if (stack && !is_break)
    update_tasks(0);
  fclose(timelog_file);
  tcsetattr(0, TCSANOW, &orig_tty_settings);
  exit(0);
}
