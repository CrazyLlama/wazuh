/* Copyright (C) 2009 Trend Micro Inc.
 * All right reserved.
 *
 * This program is a free software; you can redistribute it
 * and/or modify it under the terms of the GNU General Public
 * License (version 2) as published by the FSF - Free Software
 * Foundation
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/types.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>

#ifdef WIN32
#define sleep(x) Sleep(x * 1000)
#endif

#ifdef INOTIFY_ENABLED
#include <sys/inotify.h>
#define OS_SIZE_6144    6144
#define OS_MAXSTR       OS_SIZE_6144    /* Size for logs, sockets, etc */
#else
#include "shared.h"
#endif

#include "fs_op.h"
#include "hash_op.h"
#include "debug_op.h"
#include "syscheck.h"
#include "error_messages/error_messages.h"
#include "error_messages/debug_messages.h"

/* Prototypes */
int realtime_checksumfile(const char *file_name) __attribute__((nonnull));


/* Checksum of the realtime file being monitored */
int realtime_checksumfile(const char *file_name)
{
    char *buf;

    buf = (char *) OSHash_Get(syscheck.fp, file_name);
    if (buf != NULL) {
        char c_sum[256 + 2];

        c_sum[0] = '\0';
        c_sum[255] = '\0';

        /* If it returns < 0, we have already alerted */
        if (c_read_file(file_name, buf, c_sum) < 0) {
            // Update database
            snprintf(c_sum, sizeof(c_sum), "%.*s -1", SK_DB_NATTR, buf);
            free(buf);

            if (!OSHash_Update(syscheck.fp, file_name, strdup(c_sum))) {
                merror("Unable to update file to db: %s", file_name);
            }

            return (0);
        }

        if (strcmp(c_sum, buf + SK_DB_NATTR) != 0) {
            char alert_msg[OS_MAXSTR + 1];

            // Update database
            snprintf(alert_msg, sizeof(alert_msg), "%.*s%.*s", SK_DB_NATTR, buf, (int)strcspn(c_sum, " "), c_sum);

            if (!OSHash_Update(syscheck.fp, file_name, strdup(alert_msg))) {
                merror("Unable to update file to db: %s", file_name);
            }

            alert_msg[OS_MAXSTR] = '\0';
            char *fullalert = NULL;

            if (buf[6] == 's' || buf[6] == 'n') {
                fullalert = seechanges_addfile(file_name);
                if (fullalert) {
                    snprintf(alert_msg, OS_MAXSTR, "%s %s\n%s", c_sum, file_name, fullalert);
                    free(fullalert);
                    fullalert = NULL;
                } else {
                    snprintf(alert_msg, 912, "%s %s", c_sum, file_name);
                }
            } else {
                snprintf(alert_msg, 912, "%s %s", c_sum, file_name);
            }
            send_syscheck_msg(alert_msg);

            free(buf);

            return (1);
        } else {
            mdebug2("Discarding '%s': checksum already reported.", file_name);
        }

        return (0);
    } else {
        /* New file */
        char *c;
        int i;
        buf = strdup(file_name);

        /* Find container directory */

        while (c = strrchr(buf, '/'), c && c != buf) {
            *c = '\0';

            for (i = 0; syscheck.dir[i]; i++) {
                if (strcmp(syscheck.dir[i], buf) == 0) {
                    mdebug1("Scanning new file '%s' with options for directory '%s'.", file_name, buf);
                    read_dir(file_name, syscheck.opts[i], syscheck.filerestrict[i]);
                    break;
                }
            }

            if (syscheck.dir[i]) {
                break;
            }
        }

        free(buf);
    }

    return (0);
}

#ifdef INOTIFY_ENABLED
#include <sys/inotify.h>

#define REALTIME_MONITOR_FLAGS  IN_MODIFY|IN_ATTRIB|IN_MOVED_FROM|IN_MOVED_TO|IN_CREATE|IN_DELETE|IN_DELETE_SELF
#define REALTIME_EVENT_SIZE     (sizeof (struct inotify_event))
#define REALTIME_EVENT_BUFFER   (2048 * (REALTIME_EVENT_SIZE + 16))

/* Start real time monitoring using inotify */
int realtime_start()
{
    minfo("Initializing real time file monitoring engine.");

    syscheck.realtime = (rtfim *) calloc(1, sizeof(rtfim));
    if (syscheck.realtime == NULL) {
        merror_exit(MEM_ERROR, errno, strerror(errno));
    }
    syscheck.realtime->dirtb = OSHash_Create();
    syscheck.realtime->fd = -1;

#ifdef INOTIFY_ENABLED
    syscheck.realtime->fd = inotify_init();
    if (syscheck.realtime->fd < 0) {
        merror("Unable to initialize inotify.");
        return (-1);
    }
#endif

    return (1);
}

/* Add a directory to real time checking */
int realtime_adddir(const char *dir)
{
    if (!syscheck.realtime) {
        realtime_start();
    }

    /* Check if it is ready to use */
    if (syscheck.realtime->fd < 0) {
        return (-1);
    } else {
        int wd = 0;

        if(syscheck.skip_nfs) {
            short is_nfs = IsNFS(dir);
            if( is_nfs == 1 ) {
                merror("%s NFS Directories do not support iNotify.", dir);
            	return(-1);
            }
            else {
                mdebug2("syscheck.skip_nfs=%d, %s::is_nfs=%d", syscheck.skip_nfs, dir, is_nfs);
            }
        }

        wd = inotify_add_watch(syscheck.realtime->fd,
                               dir,
                               REALTIME_MONITOR_FLAGS);
        if (wd < 0) {
            merror("Unable to add directory to real time monitoring: '%s'. %d %d", dir, wd, errno);
        } else {
            char wdchar[32 + 1];
            wdchar[32] = '\0';
            snprintf(wdchar, 32, "%d", wd);

            /* Entry not present */
            if (!OSHash_Get(syscheck.realtime->dirtb, wdchar)) {
                char *ndir;

                ndir = strdup(dir);
                if (ndir == NULL) {
                    merror_exit("Out of memory. Exiting.");
                }

                OSHash_Add(syscheck.realtime->dirtb, wdchar, ndir);
                mdebug1("Directory added for real time monitoring: '%s'.", ndir);
            }
        }
    }

    return (1);
}

/* Process events in the real time queue */
int realtime_process()
{
    ssize_t len;
    size_t i = 0;
    char buf[REALTIME_EVENT_BUFFER + 1];
    struct inotify_event *event;

    buf[REALTIME_EVENT_BUFFER] = '\0';

    len = read(syscheck.realtime->fd, buf, REALTIME_EVENT_BUFFER);
    if (len < 0) {
        merror("Unable to read from real time buffer.");
    } else if (len > 0) {
        while (i < (size_t) len) {
            event = (struct inotify_event *) (void *) &buf[i];

            if (event->len) {
                char wdchar[32 + 1];
                char final_name[MAX_LINE + 1];

                wdchar[32] = '\0';
                final_name[MAX_LINE] = '\0';

                snprintf(wdchar, 32, "%d", event->wd);

                snprintf(final_name, MAX_LINE, "%s/%s",
                         (char *)OSHash_Get(syscheck.realtime->dirtb, wdchar),
                         event->name);

                /* Need a sleep here to avoid triggering on vim
                * (and finding the file removed)
                */

                struct timeval timeout = {0, syscheck.rt_delay * 1000};
                select(0, NULL, NULL, NULL, &timeout);

                realtime_checksumfile(final_name);
            }

            i += REALTIME_EVENT_SIZE + event->len;
        }
    }

    return (0);
}

#elif defined(WIN32)
typedef struct _win32rtfim {
    HANDLE h;
    OVERLAPPED overlap;

    char *dir;
    TCHAR buffer[12288];
} win32rtfim;

int realtime_win32read(win32rtfim *rtlocald);

void CALLBACK RTCallBack(DWORD dwerror, DWORD dwBytes, LPOVERLAPPED overlap)
{
    int lcount;
    size_t offset = 0;
    char *ptfile;
    char wdchar[260 + 1];
    char final_path[MAX_LINE + 1];
    win32rtfim *rtlocald;
    PFILE_NOTIFY_INFORMATION pinfo;
    TCHAR finalfile[MAX_PATH];

    if (dwBytes == 0) {
        merror("real time call back called, but 0 bytes.");
        return;
    }

    if (dwerror != ERROR_SUCCESS) {
        merror("real time call back called, but error is set.");
        return;
    }

    /* Get hash to parse the data */
    wdchar[260] = '\0';
    snprintf(wdchar, 260, "%s", (char*)overlap->Pointer);
    rtlocald = OSHash_Get(syscheck.realtime->dirtb, wdchar);
    if (rtlocald == NULL) {
        merror("real time call back called, but hash is empty.");
        return;
    }

    do {
        pinfo = (PFILE_NOTIFY_INFORMATION) &rtlocald->buffer[offset];
        offset += pinfo->NextEntryOffset;

        lcount = WideCharToMultiByte(CP_ACP, 0, pinfo->FileName,
                                     pinfo->FileNameLength / sizeof(WCHAR),
                                     finalfile, MAX_PATH - 1, NULL, NULL);
        finalfile[lcount] = TEXT('\0');

        /* Change forward slashes to backslashes on finalfile */
        ptfile = strchr(finalfile, '\\');
        while (ptfile) {
            *ptfile = '/';
            ptfile++;

            ptfile = strchr(ptfile, '\\');
        }

        final_path[MAX_LINE] = '\0';
        snprintf(final_path, MAX_LINE, "%s/%s", rtlocald->dir, finalfile);

        /* Check the change */
        realtime_checksumfile(final_path);
    } while (pinfo->NextEntryOffset != 0);

    realtime_win32read(rtlocald);

    return;
}

int realtime_start()
{
    minfo("Initializing real time file monitoring engine.");

    os_calloc(1, sizeof(rtfim), syscheck.realtime);
    syscheck.realtime->dirtb = (void *)OSHash_Create();
    syscheck.realtime->fd = -1;
    syscheck.realtime->evt = CreateEvent(NULL, TRUE, FALSE, NULL);

    return (0);
}

int realtime_win32read(win32rtfim *rtlocald)
{
    int rc;

    rc = ReadDirectoryChangesW(rtlocald->h,
                               rtlocald->buffer,
                               sizeof(rtlocald->buffer) / sizeof(TCHAR),
                               TRUE,
                               FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME | FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                               0,
                               &rtlocald->overlap,
                               RTCallBack);
    if (rc == 0) {
        merror("Unable to set directory for monitoring: %s", rtlocald->dir);
        sleep(2);
    }

    return (0);
}

int realtime_adddir(const char *dir)
{
    char wdchar[260 + 1];
    win32rtfim *rtlocald;

    if (!syscheck.realtime) {
        realtime_start();
    }

    /* Maximum limit for realtime on Windows */
    if (syscheck.realtime->fd > syscheck.max_fd_win_rt) {
        merror("Unable to add directory to real time monitoring: '%s' - Maximum size permitted.", dir);
        return (0);
    }

    /* Set key for hash */
    wdchar[260] = '\0';
    snprintf(wdchar, 260, "%s", dir);
    if(OSHash_Get(syscheck.realtime->dirtb, wdchar)) {
        mdebug2("Entry '%s' already exists in the RT hash.", wdchar);
    }
    else {
        os_calloc(1, sizeof(win32rtfim), rtlocald);

        rtlocald->h = CreateFile(dir,
                                FILE_LIST_DIRECTORY,
                                FILE_SHARE_DELETE | FILE_SHARE_READ | FILE_SHARE_WRITE,
                                NULL,
                                OPEN_EXISTING,
                                FILE_FLAG_BACKUP_SEMANTICS | FILE_FLAG_OVERLAPPED,
                                NULL);


        if (rtlocald->h == INVALID_HANDLE_VALUE || rtlocald->h == NULL) {
            free(rtlocald);
            rtlocald = NULL;
            merror("Unable to add directory to real time monitoring: '%s'.", dir);
            return (0);
        }
        syscheck.realtime->fd++;

        /* Add final elements to the hash */
        os_strdup(dir, rtlocald->dir);
        os_strdup(dir, rtlocald->overlap.Pointer);
        OSHash_Add(syscheck.realtime->dirtb, wdchar, rtlocald);

        /* Add directory to be monitored */
        realtime_win32read(rtlocald);
    }

    return (1);
}

#else /* !WIN32 */

int realtime_start()
{
    merror("Unable to initialize real time file monitoring.");

    return (0);
}

int realtime_adddir(__attribute__((unused)) const char *dir)
{
    return (0);
}

int realtime_process()
{
    return (0);
}

#endif /* WIN32 */
