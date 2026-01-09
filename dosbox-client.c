/* MBXSRV.C - robust DOS mailbox command server (for DOSBox-X shared folder)
 *
 * Protocol (recommended):
 *   Host writes CMD.NEW then renames to CMD.TXT when complete.
 *   Guest claims by renaming CMD.TXT -> CMD.RUN.
 *   Guest executes script via MBXJOB.BAT, redirects stdout to OUT.NEW,
 *   then renames OUT.NEW -> OUT.TXT (and RC.NEW -> RC.TXT).
 *
 * Stop command:
 *   Put EXIT or QUIT on the first non-empty line (or as first command line).
 *
 * Build (DOS target):
 *   wcl -bt=dos -os -s -zq mbxsrv.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <unistd.h>
#include <dos.h>
#include <io.h>
#include <errno.h>
#include <conio.h>

#define CMD_TXT   "CMD.TXT"
#define CMD_RUN   "CMD.RUN"
#define OUT_TXT   "OUT.TXT"
#define OUT_NEW   "OUT.NEW"
#define RC_TXT    "RC.TXT"
#define RC_NEW    "RC.NEW"
#define STA_TXT   "STA.TXT"
#define LOG_TXT   "LOG.TXT"
#define JOB_BAT   "MBXJOB.BAT"

/* Limits */
#define MAX_LINE      512
#define MAX_PAYLOAD   (32 * 1024) /* max bytes copied from CMD into JOB_BAT */

static void ms_sleep(unsigned ms) { delay(ms); }

static void timestamp(char *buf, size_t cap)
{
    struct dosdate_t d;
    struct dostime_t t;
    _dos_getdate(&d);
    _dos_gettime(&t);
    /* YYYY-MM-DD HH:MM:SS */
    if (cap < 20) { if (cap) buf[0] = 0; return; }
    sprintf(buf, "%04u-%02u-%02u %02u:%02u:%02u",
            d.year, d.month, d.day, t.hour, t.minute, t.second);
}

static void log_line(const char *msg)
{
    FILE *f = fopen(LOG_TXT, "at");
    char ts[32];
    if (!f) return;
    timestamp(ts, sizeof(ts));
    fprintf(f, "[%s] %s\r\n", ts, msg);
    fclose(f);
}

/* printf-like into log (small, safe enough for our use) */
static void logf2(const char *a, const char *b)
{
    char buf[256];
    strncpy(buf, a, sizeof(buf)-1);
    buf[sizeof(buf)-1] = 0;
    strncat(buf, b, sizeof(buf)-1 - strlen(buf));
    log_line(buf);
}

static int file_exists(const char *path)
{
    return (access(path, 0) == 0);
}

static void trim(char *s)
{
    char *p = s;
    size_t n;

    while (*p && isspace((unsigned char)*p)) p++;
    if (p != s) memmove(s, p, strlen(p) + 1);

    n = strlen(s);
    while (n > 0 && isspace((unsigned char)s[n - 1])) {
        s[n - 1] = '\0';
        n--;
    }
}

/* Atomic-ish write: write TEMP then rename to FINAL */
static int write_text_atomic(const char *temp, const char *final, const char *text)
{
    FILE *f;

    remove(temp);
    f = fopen(temp, "wt");
    if (!f) return 0;
    fputs(text, f);
    fputs("\r\n", f);
    fclose(f);

    remove(final);
    if (rename(temp, final) != 0) return 0;
    return 1;
}

static void set_status(const char *state)
{
    /* Keep STA.TXT tiny and always replace */
    if (!write_text_atomic("STA.NEW", STA_TXT, state)) {
        /* If status write fails, at least log it */
        log_line("WARN: failed to write STA.TXT");
    }
}

/* Claim CMD.TXT by renaming to CMD.RUN with retries.
   Returns 1 if claimed, 0 otherwise. */
static int claim_cmd(void)
{
    int tries;
    remove(CMD_RUN); /* stale from crash */

    for (tries = 0; tries < 20; tries++) {
        if (!file_exists(CMD_TXT)) return 0;
        if (rename(CMD_TXT, CMD_RUN) == 0) return 1;
        ms_sleep(50);
    }
    return 0;
}

/* Read first non-empty line from CMD.RUN into buf. Returns 1 on success. */
static int read_first_nonempty_line(const char *path, char *buf, size_t bufsz)
{
    FILE *f = fopen(path, "rt");
    if (!f) return 0;

    while (fgets(buf, (int)bufsz, f)) {
        size_t n = strlen(buf);
        while (n > 0 && (buf[n-1] == '\r' || buf[n-1] == '\n')) { buf[n-1]=0; n--; }
        trim(buf);
        if (buf[0] != 0) { fclose(f); return 1; }
    }

    fclose(f);
    return 0;
}

/* Copy CMD.RUN into JOB_BAT as a script, with wrapper + RC capture.
   Returns 1 on success, 0 on failure. */
static int build_job_bat_from_cmd(const char *cmd_path, int *out_payload_bytes)
{
    FILE *in, *out;
    char line[MAX_LINE];
    int total = 0;

    remove(JOB_BAT);
    remove(RC_NEW);

    in = fopen(cmd_path, "rt");
    if (!in) return 0;

    out = fopen(JOB_BAT, "wt");
    if (!out) { fclose(in); return 0; }

    /* Wrapper */
    fputs("@echo off\r\n", out);
    fputs("rem MBXSRV job wrapper\r\n", out);

    /* Copy payload, line by line, enforcing MAX_PAYLOAD */
    while (fgets(line, sizeof(line), in)) {
        int len = (int)strlen(line);
        if (total + len > MAX_PAYLOAD) {
            fputs("rem ERROR: payload too large\r\n", out);
            /* Force errorlevel-ish */
            fputs("echo 1 > " RC_NEW "\r\n", out);
            fclose(in);
            fclose(out);
            if (out_payload_bytes) *out_payload_bytes = total;
            return 0;
        }
        fputs(line, out);
        total += len;
    }

    /* Always write return code file for host */
    fputs("\r\nrem Capture ERRORLEVEL of last command\r\n", out);
    fputs("echo %errorlevel% > " RC_NEW "\r\n", out);

    fclose(in);
    fclose(out);

    if (out_payload_bytes) *out_payload_bytes = total;
    return 1;
}

/* Execute JOB_BAT, redirecting stdout to OUT_NEW.
   Optional stderr capture if env MBX_STDERR=1 (works on FreeDOS/4DOS; not classic MS-DOS).
   Returns system() result. */
static int exec_job_to_out(void)
{
    const char *comspec = getenv("COMSPEC");
    const char *stderr_opt = getenv("MBX_STDERR"); /* set to "1" if your shell supports 2>&1 */
    char cmd[256];

    if (!comspec || !comspec[0]) comspec = "COMMAND.COM";

    remove(OUT_NEW);

    /* Build: <COMSPEC> /C MBXJOB.BAT > OUT.NEW [2>&1] */
    /* Keep it short for DOS command length limits. */
    strcpy(cmd, comspec);
    strcat(cmd, " /C ");
    strcat(cmd, JOB_BAT);
    strcat(cmd, " > ");
    strcat(cmd, OUT_NEW);

    if (stderr_opt && stderr_opt[0] == '1') {
        strcat(cmd, " 2>&1");
    }

    return system(cmd);
}

static void publish_results(int sys_rc)
{
    char msg[160];

    /* Ensure OUT_NEW exists; if not, create an error output */
    if (!file_exists(OUT_NEW)) {
        FILE *f = fopen(OUT_NEW, "wt");
        if (f) {
            fprintf(f, "ERROR: OUT.NEW missing (system rc=%d)\r\n", sys_rc);
            fclose(f);
        }
    }

    /* Publish OUT */
    remove(OUT_TXT);
    if (rename(OUT_NEW, OUT_TXT) != 0) {
        /* If rename fails, try to at least log it */
        sprintf(msg, "ERROR: failed to rename OUT.NEW -> OUT.TXT (errno=%d)", errno);
        log_line(msg);
    }

    /* Publish RC if present */
    if (file_exists(RC_NEW)) {
        remove(RC_TXT);
        if (rename(RC_NEW, RC_TXT) != 0) {
            sprintf(msg, "ERROR: failed to rename RC.NEW -> RC.TXT (errno=%d)", errno);
            log_line(msg);
        }
    } else {
        /* Create RC.TXT to signal something happened */
        FILE *f = fopen(RC_TXT, "wt");
        if (f) { fputs("1\r\n", f); fclose(f); }
    }
}

/* Write a clear error message into OUT.TXT (atomic-ish) */
static void write_error_output(const char *what)
{
    FILE *f;

    remove(OUT_NEW);
    f = fopen(OUT_NEW, "wt");
    if (!f) return;

    fprintf(f, "ERROR: %s\r\n", what);
    fprintf(f, "errno=%d\r\n", errno);
    fclose(f);

    remove(OUT_TXT);
    rename(OUT_NEW, OUT_TXT);

    /* Also ensure RC is non-zero */
    remove(RC_NEW);
    f = fopen(RC_NEW, "wt");
    if (f) { fputs("1\r\n", f); fclose(f); }
    remove(RC_TXT);
    rename(RC_NEW, RC_TXT);
}

static int is_exit_cmd(const char *s)
{
    return (stricmp(s, "EXIT") == 0 || stricmp(s, "QUIT") == 0);
}

int main(int argc, char **argv)
{
    int idle_ms = 100;
    char first[MAX_LINE];
    char logbuf[200];

    /* Optional: allow polling interval as argv[1] */
    if (argc >= 2) {
        int v = atoi(argv[1]);
        if (v >= 10 && v <= 2000) idle_ms = v;
    }

    log_line("MBXSRV starting");
    set_status("READY");

    /* Crash recovery: if CMD.RUN exists, process it */
    if (file_exists(CMD_RUN)) {
        log_line("Found stale CMD.RUN; will process it");
    }

    for (;;) {
        /* Let ESC stop the server locally */
        if (kbhit()) {
            int ch = getch();
            if (ch == 27) { /* ESC */
                log_line("ESC pressed; exiting");
                set_status("BYE");
                break;
            }
        }

        /* If no CMD.RUN, try to claim CMD.TXT */
        if (!file_exists(CMD_RUN) && file_exists(CMD_TXT)) {
            if (claim_cmd()) {
                log_line("Claimed CMD.TXT -> CMD.RUN");
            }
        }

        /* If we have CMD.RUN, process it */
        if (file_exists(CMD_RUN)) {
            int payload_bytes = 0;
            int sys_rc;

            set_status("RUNNING");

            if (!read_first_nonempty_line(CMD_RUN, first, sizeof(first))) {
                log_line("ERROR: CMD.RUN empty");
                write_error_output("CMD file is empty");
                remove(CMD_RUN);
                set_status("READY");
                ms_sleep((unsigned)idle_ms);
                continue;
            }

            if (is_exit_cmd(first)) {
                log_line("Received EXIT/QUIT");
                write_text_atomic(OUT_NEW, OUT_TXT, "MBXSRV BYE");
                write_text_atomic(RC_NEW, RC_TXT, "0");
                remove(CMD_RUN);
                set_status("BYE");
                break;
            }

            /* Build job bat */
            if (!build_job_bat_from_cmd(CMD_RUN, &payload_bytes)) {
                sprintf(logbuf, "ERROR: build_job_bat failed (payload=%d, errno=%d)", payload_bytes, errno);
                log_line(logbuf);
                write_error_output("Failed to build MBXJOB.BAT (payload too large or file error)");
                remove(CMD_RUN);
                set_status("READY");
                ms_sleep((unsigned)idle_ms);
                continue;
            }

            sprintf(logbuf, "Executing job (payload=%d bytes)", payload_bytes);
            log_line(logbuf);

            /* Clean old published files to reduce confusion */
            remove(OUT_TXT);
            remove(RC_TXT);

            /* Execute */
            sys_rc = exec_job_to_out();
            sprintf(logbuf, "system() rc=%d", sys_rc);
            log_line(logbuf);

            publish_results(sys_rc);

            remove(CMD_RUN);
            set_status("READY");
        }

        ms_sleep((unsigned)idle_ms);
    }

    log_line("MBXSRV stopped");
    return 0;
}
