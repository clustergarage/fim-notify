#define _GNU_SOURCE
#include <errno.h>
#include <ftw.h>
#include <limits.h>
#include <mqueue.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <unistd.h>

#include "fimnotify.h"
#include "fimcache.h"
#include "fimtree.h"
#include "fimutil.h"

// get access to shared variables from fimcache
extern struct fimwatch *wlcache;
// set local static variables
static mqd_t imq;

/**
 * when the cache is in an unrecoverable state, we discard the current
 * inotify file descriptor `oldfd` and create a new one (returned
 * as the function result), and remove and rebuild the cache
 *
 * if `oldfd` is -1, this is the initial build of the cache, or an
 * explicitly requested cache rebuild, so we are a little less verbose,
 * and we reset `reinitc`
 *
 * `mask` can be reinitialized this way
 */
static int reinitialize(struct fimwatch *watch) {
    static int reinitc; // @TODO: is this needed?
    int fd;
    int i, slot;
    bool rebuild = watch->fd != EOF;

    if (rebuild) {
        close(watch->fd);
        ++reinitc;
#if DEBUG
        printf("reinitializing cache and inotify fd (reinitc = %d)\n", reinitc);
        fflush(stdout);
#endif
    } else {
#if DEBUG
        printf("initializing cache\n");
        fflush(stdout);
#endif
        reinitc = 0;
    }

    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd == EOF) {
#if DEBUG
        perror("inotify_init1");
#endif
        return EOF;
    }
    watch->fd = fd;
#if DEBUG
    printf("  new fd = %d\n", fd);
    fflush(stdout);
#endif

    // free watch cache
    free_cache(watch);
    // begin traversing tree, or non-recursive directories
    watch_subtree(watch);

    slot = find_cached_slot(watch->pid, watch->sid);
    if (slot == -1) {
        // cache information about the watch
        add_watch_to_cache(watch);
    }

#if DEBUG
    int cnt;
    for (i = 0, cnt = 0; i < wlcachec; ++i) {
        if (wlcache[i].pid != watch->pid ||
            wlcache[i].sid != watch->sid) {
            continue;
        }
        if (wlcache[i].pathc != EOF) {
            cnt += wlcache[i].pathc;
        }
    }
    if (rebuild) {
        printf("rebuilt cache with %d entries\n", cnt);
        fflush(stdout);
    }
#endif

    // check cache consistency right away, in case there are multiple
    // containers in a single pod that don't have a path on the
    // filesystem that we specified to watch
    check_cache_consistency(watch);

    return fd;
}

/**
 * process the next inotify event in the buffer specified by `ptr` and `len`
 * in most cases, a single event is consumed, but if there is an IN_MOVED_FROM+
 * IN_MOVED_TO pair that share a cookie value, both events are consumed returns
 * the number of bytes in the event(s) consumed from `ptr`
 */
static size_t process_next_inotify_event(struct fimwatch *watch, char *ptr, int len, bool first) {
    const struct inotify_event *event = (const struct inotify_event *)ptr;
    char *path;
    char fullpath[PATH_MAX + NAME_MAX];
    size_t evtlen;
    int slot, wdslot;

    if (event->wd != EOF) {
        path = wd_to_path_name(watch, event->wd);

        if (!(event->mask & IN_IGNORED)) {
            // IN_Q_OVERFLOW has (event->wd == -1)
            // skip IN_IGNORED, since it will come after an event that has already
            // removed the corresponding cache entry
            //
            // cache consistency check
            // see the discussion of "intra-tree" `rename` events
            slot = find_watch_checked(watch, event->wd);
            if (slot == -1) {
                // reinitialize the inotify watch
                watch->fd = EOF;
                // cache reached an inconsistent state
                reinitialize(watch);
                // discard all remaining events in current `read` buffer
                return INOTIFY_READ_BUF_LEN;
            }
        }
    }

    evtlen = sizeof(struct inotify_event) + event->len;

    if ((event->mask & IN_ISDIR) &&
        (event->mask & (IN_CREATE | IN_MOVED_TO))) {
        // a new subdirectory was created, or a subdirectory was renamed into
        // the tree; create watches for it, and all of its subdirectories
        snprintf(fullpath, sizeof(fullpath), "%s/%s", path, event->name);

#if DEBUG
        printf("directory creation on wd %d: %s\n", event->wd, fullpath);
        fflush(stdout);
#endif

        // we only watch the new subtree if it has not already been cached
        // this deals with a race condition:
        // * on the one hand, the following steps might occur:
        //   1. the "child" directory is created
        //   2. the "grandchild" directory is created
        //   3. we receive an IN_CREATE event for the creation of the "child"
        //      and create a watch and a cache entry for it
        //   4. to handle the possibility that step 2 came before step 3, we
        //      recursively walk through the descendants of the "child"
        //      directory, adding any subdirectories to the cache
        // * on the other hand, the following steps might occur:
        //   1. the "child" directory is created
        //   3. we receive an IN_CREATE event for the creation of the "child"
        //      and create a watch and a cache entry for it
        //   3. the "grandchild" directory is created
        //   4. during the recursive walk through the descendants of the "child"
        //      directory, we cache the "grandchild" and add a watch for it
        //   5. we receive the IN_CREATE event for the creation of the
        //      "grandchild"
        //      at this point, we should NOT create a cache entry and watch for
        //      the "grandchild" because they already exist (creating the watch
        //      for the second time is harmless, but adding a second cache for
        //      the grandchild would leave the cache in a confused state)
        if (!path_name_to_cache_slot(watch, fullpath) > -1) {
            wdslot = wd_to_cache_slot(watch, event->wd);
            if (wdslot > -1 &&
                // only do this if watching recursively
                watch->recursive) {
                watch->pathc = 0;
                watch_subtree(watch);
                // @TODO: does this work
                wlcache[watch->slot] = *watch;
            }
        }
    } else if (event->mask & IN_DELETE_SELF) {
        // a directory was deleted
        // remove the corresponding item from the cache
#if DEBUG
        printf("clearing watchlist item %d (%s)\n", event->wd, path);
        fflush(stdout);
#endif
        if (find_root_path(watch, path) != NULL) {
            remove_root_path(watch, path);
        }
        mark_cache_slot_empty(slot);
        // no need to remove the watch; that happens automatically
    } else if ((event->mask & (IN_MOVED_FROM | IN_ISDIR)) == (IN_MOVED_FROM | IN_ISDIR)) {
        // we have a "moved from" event
        // to know how to deal with it, we need to determine whether there is a
        // following "moved to" event with a matching cookie value (i.e., an
        // "intra-tree" `rename` where the source and destination are inside
        // our monitored trees)
        // if there is not, then we are dealing with a `rename` out of our
        // monitored tree(s)
        //
        // we assume that if this is an "intra-tree" `rename` event, then the
        // "moved to" event is the next event in the buffer returned by the
        // current `read`
        // (if we are already at the last event in this buffer, then we ask our
        // caller to read a bit more, in the hope of getting the following
        // IN_MOVED_TO event in the next `read`)
        //
        // in most cases, the assumption holds
        // however, where multiple processes are manipulating the tree, we can
        // can get event sequences such as the following:
        //
        //   IN_MOVED_FROM   (rename(x) by process A)
        //     IN_MOVED_FROM (rename(y) by process B)
        //     IN_MOVED_TO   (rename(y) by process B)
        //   IN_MOVED_TO     (rename(x) by process A)
        //
        // in principle, there may be arbitrarily complex variations on the
        // above theme
        // our assumption that related IN_MOVED_FROM and IN_MOVED_TO events are
        // consecutive is broken by such scenarios
        //
        // we could try to resolve this issue by extending the window we use to
        // search for IN_MOVED_TO events beyond the next item in the queue
        // but this must be done heuristically (e.g., limiting the window to N
        // events or to events read within X milliseconds), because sometimes
        // we will have an unmatched IN_MOVED_FROM events that result from
        // out-of-tree renames
        // the heuristic approach is therefore unavoidably racy: there is
        // always a chance that we will fail to match up an IN_MOVED_FROM+
        // IN_MOVED_TO event pair
        //
        // so, this program takes the simple approach of assuming that an
        // IN_MOVED_FROM+IN_MOVED_TO pair occupy consecutive events in the
        // buffer returned by `read`
        //
        // when that assumption is wrong (and we therefore fail to recognize
        // an intra-tree `rename` event), then the rename will be treated as
        // separate "moved from" and "moved to" events, with the result that
        // some watch items and cache entries are removed and re-created
        // this causes the watch descriptors in our cache to become
        // inconsistent with the watch descriptors in as yet unread events,
        // because the watches are re-created with different watch descriptor
        // numbers
        //
        // once such an inconsistency occurs, then, at some later point, we
        // will do a lookup for a watch descriptor returned by inotify, and
        // find that it is not in our cache
        // when that happens, we reinitialize our cache with a fresh set of
        // watch descriptors and re-create the inotify file descriptor, in
        // order to bring our cache back into consistency with the filesystem
        // an alternative would be to cache the cookies of the (recent)
        // IN_MOVED_FROM events for which which we did not find a matching
        // IN_MOVED_TO event, and rebuild our watch cache when we find an
        // IN_MOVED_TO event whose cookie matches one of the cached cookies
        // yet another approach when we detect an out-of-tree rename would be
        // to reinitialize the cache and create a new inotify file descriptor
        //
        // (@TODO: consider the fact that for a rename event, there won't be
        // other events for the object between IN_MOVED_FROM and IN_MOVED_TO)
        //
        // rebuilding the watch cache is expensive if the monitored tree is
        // large
        // so, there is a trade-off between how much effort we want to go to to
        // avoid cache rebuilds versus how much effort we want to devote to
        // matching up IN_MOVED_FROM+IN_MOVED_TO event pairs
        // at the one extreme we would do no search ahead for IN_MOVED_TO, with
        // the result that every `rename` potentially could trigger a cache
        // rebuild
        // limiting the search window to just the following event is a
        // compromise that catches the vast majority of intra-tree renames and
        // triggers relatively few cache rebuilds

        const struct inotify_event *nextevent = (const struct inotify_event *)(ptr + evtlen);

        if (((char *)nextevent < ptr + len) &&
            (nextevent->mask & IN_MOVED_TO) &&
            (nextevent->cookie == event->cookie)) {
            // we have a `rename` event
            // we need to fix up the cached pathnames for the corresponding
            // directory and all of its subdirectories
            int nextslot = find_watch_checked(watch, nextevent->wd);
            if (nextslot == -1) {
                // reinitialize the inotify watch
                watch->fd = EOF;
                // cache reached an inconsistent state
                reinitialize(watch);
                // discard all remaining events in current `read` buffer
                return INOTIFY_READ_BUF_LEN;
            }

            rewrite_cached_paths(watch, path, event->name,
                wd_to_path_name(watch, nextevent->wd), nextevent->name);

            // also processed the next (IN_MOVED_TO) event, so skip over it
            evtlen += sizeof(struct inotify_event) + nextevent->len;
        } else if (((char *)nextevent < ptr + len) || !first) {
            // got a "moved from" event without an accompanying "moved to" event
            // the directory has been moved outside the tree we are monitoring
            // need to remove the watches and remove the cache entries for the
            // moved directory and all of its subdirectories
#if DEBUG
            printf("moved out: %p %p\n", path, event->name);
            printf("first = %d; remaining bytes = %ld\n", first, ptr + len - (char *)nextevent);
            fflush(stdout);
#endif
            snprintf(fullpath, sizeof(fullpath), "%s/%s", path, event->name);

            slot = find_watch_checked(watch, event->wd);
            if (slot > -1 &&
                remove_subtree(watch, fullpath) == -1) {
                // cache reached an inconsistent state
                reinitialize(watch);
                // discard all remaining events in current `read` buffer
                return INOTIFY_READ_BUF_LEN;
            }
        } else {
#if DEBUG
            printf("hanging IN_MOVED_FROM\n");
            fflush(stdout);
#endif
            // tell caller to do another `read`
            return -1;
        }
    } else if (event->mask & IN_Q_OVERFLOW) {
        static int overflowc = 0; // @TODO: is this needed?
        ++overflowc;
#if DEBUG
        printf("queue overflow (%d) (inotifyreadc = %d)\n", overflowc, inotifyreadc);
        fflush(stdout);
#endif

        // when the queue overflows, some events are lost, at which point we've
        // lost any chance of keeping our cache consistent with the state of
        // the filesystem
        // so, discard this inotify file descriptor and create a new one, and
        // remove and rebuild the cache
        slot = find_watch_checked(watch, event->wd);
        if (slot > -1) {
            reinitialize(watch);
        }
        // discard all remaining events in current `read` buffer
        evtlen = INOTIFY_READ_BUF_LEN;
    } else if (event->mask & IN_UNMOUNT) {
        // when a filesystem is unmounted, each of the watches on the is
        // dropped, and an unmount and an ignore event are generated
        // there's nothing left for us to monitor, so we just remove the
        // corresponding cache entry
#if DEBUG
        printf("filesystem unmounted: %s\n", path);
        fflush(stdout);
#endif
        mark_cache_slot_empty(slot);
        // no need to remove the watch; that happens automatically
    } else if (event->mask & IN_MOVE_SELF &&
        find_root_path(watch, path) != NULL) {
        // if the root path moves to a new location in the same filesystem,
        // then all cached pathnames become invalid, and we have no direct way
        // of knowing the new name of the root path
        // we could in theory find the new name by caching the i-node of the
        // root path on start-up and then trying to find a pathname that
        // corresponds to that i-node
        // instead, we'll keep things simple, and just cease monitoring it
#if DEBUG
        printf("root path moved: %s\n", path);
        fflush(stdout);
#endif
        remove_root_path(watch, path);

        if (remove_subtree(watch, path) == -1) {
            // cache reached an inconsistent state
            slot = find_watch_checked(watch, event->wd);
            if (slot > -1) {
                reinitialize(watch);
            }
            // discard all remaining events in current `read` buffer
            return INOTIFY_READ_BUF_LEN;
        }
    }

    slot = find_watch_checked(watch, event->wd);
    if (slot == -1 ||
        // only continue with the events we care about
        !(event->mask & watch->event_mask)) {
        // discard all remaining events in current `read` buffer
        return sizeof(struct inotify_event) + event->len;
    }

sendevent: ; // hack to get past label syntax error
    struct fimwatch_event fwevent = {
        .event_mask = event->mask,
        .path_name = path,                          // name of the watched directory
        .file_name = event->len ? event->name : "", // name of the file
        .is_dir = (event->mask & IN_ISDIR)
    };

#if DEBUG
    printf("send event: path = %s; file: %s; event mask = %d; dir: %d\n", fwevent.path_name,
        fwevent.file_name, fwevent.event_mask, fwevent.is_dir);
    fflush(stdout);
#endif

    // @TODO: document this
    if (mq_send(imq, (const char *)&fwevent, sizeof(fwevent), 0) == EOF) {
#if DEBUG
        perror("mq_send");
#endif
    }

    check_cache_consistency(watch);

    return evtlen;
}

/**
 * read all available inotify events from the file descriptor `fd`
 */
static void process_inotify_events(struct fimwatch *watch) {
    // some systems cannot read integer variables if they are not properly
    // aligned on other systems, incorrect alignment may decrease performance
    // hence, the buffer used for reading from the inotify file descriptor
    // should have the same alignment as struct inotify_event
    char buf[INOTIFY_READ_BUF_LEN] __attribute__((aligned(__alignof__(struct inotify_event))));
    ssize_t len, nr;
    int i, evtlen, slot;
    bool first = true;
    char *ptr;

    len = read(watch->fd, buf, INOTIFY_READ_BUF_LEN);
    if (len == EOF) {
#if DEBUG
        perror("read");
#endif
        return;
    } else if (len == 0) {
#if DEBUG
        fprintf(stderr, "`read` from inotify fd returned 0!");
#endif
        return;
    }

    ++inotifyreadc;

    // process each event in the buffer returned by `read`
    // loop over all events in the buffer
    for (ptr = buf; ptr < buf + len; /*ptr += sizeof(struct inotify_event) + event->len*/) {
        evtlen = process_next_inotify_event(watch, ptr, buf + len - ptr, first);

        if (evtlen > 0) {
            ptr += evtlen;
            first = true;
        } else {
            // we got here because an IN_MOVED_FROM event was found at the end
            // of a previously read buffer and that event may be part of an
            // "intra-tree" `rename`, meaning that we should check if there is
            // a subsequent IN_MOVED_TO event with the same cookie value
            // we left that event unprocessed and we will now try to read some
            // more events, delaying for a short time, to give the associated
            // IN_MOVED_IN event (if there is one) a chance to arrive
            // however, we only want to do this once: if the `read` below fails
            // to gather further events, then when we reprocess the
            // IN_MOVED_FROM we should treat it as though this is an
            // out-of-tree `rename`
            // set `first` to 0 for the next `process_next_inotify_event` call
            int savederr;
            first = false;
            len = buf + len - ptr;

            // shuffle remaining bytes to start of buffer
            for (i = 0; i < len; ++i) {
                buf[i] = ptr[i];
            }

            // set a timeout for `read
            // some rough testing suggests that a 2-millisecond timeout is
            // sufficient to ensure that, in around 99.8% of cases, we get the
            // IN_MOVED_TO event (if there is one) that matched an
            // IN_MOVED_FROM event, even in a highly dynamic directory tree
            // this number may, of course, warrant tuning on different hardware
            // and in environments with different filesystem activity levels
            ualarm(2000, 0);
            nr = read(watch->fd, buf + len, INOTIFY_READ_BUF_LEN - len);

            // in case `ualarm` should change errno
            savederr = errno;
            // cancel alarm
            ualarm(0, 0);
            errno = savederr;

            if (nr == -1 && errno != EINTR) {
#if DEBUG
                perror("read");
#endif
            } else if (nr == 0) {
#if DEBUG
                fprintf(stderr, "`read` from inotify fd returned 0!");
#endif
                return;
            }

            if (errno != -1) {
                len += nr;
                ++inotifyreadc;
            } else {
                // EINTR
            }
            // start again at beginning of buffer
            ptr = buf;
        }
    }
}

int start_inotify_watcher(const int pid, const int sid, int pathc, char *paths[], int ignorec, char *ignores[],
    uint32_t mask, bool only_dir, bool recursive, int max_depth, int processevtfd, mqd_t mq) {
    int fd, pollc;
    nfds_t nfds;
    struct pollfd fds[2];
    sigset_t sigmask;
    sigemptyset(&sigmask);
    sigaddset(&sigmask, SIGCHLD);

    // @TODO: document this
    struct fimwatch watch;
    int slot = find_cached_slot(pid, sid);
    if (slot > -1) {
        watch = wlcache[slot];
    } else {
        // create new fimwatch placeholder struct, to be filled later
        watch = (struct fimwatch){
            .pid = pid,
            .sid = sid,
            .slot = -1,
            .fd = EOF,
            .rootpathc = pathc,
            .rootpaths = paths,
            .pathc = 0,
            .ignorec = ignorec,
            .ignores = ignores,
            .event_mask = mask,
            .only_dir = only_dir,
            .recursive = recursive,
            .max_depth = max_depth
        };
    }

    // save a copy of the paths
    copy_root_paths(&watch);

#if DEBUG
    printf("  Listening for events (pid = %d, sid = %d)\n", pid, sid);
    fflush(stdout);
#endif

    // create an inotify instance and populate it with entries for paths
    fd = reinitialize(&watch);
    if (fd == EOF) {
        goto exit;
    }

    // store mq file descriptor
    if (mq == EOF) {
        goto exit;
    }
    imq = mq;

    // prepare for polling
    nfds = 2;
    // inotify input
    fds[0].fd = fd;
    fds[0].events = POLLIN;
    // anonymous pipe for manual kill
    fds[1].fd = processevtfd;
    fds[1].events = POLLIN;

    // wait for events
    for (;;) {
        pollc = ppoll(fds, nfds, NULL, &sigmask);
        if (pollc == EOF) {
            if (errno == EINTR) {
                continue;
            }
#if DEBUG
            perror("ppoll");
#endif
            goto exit;
        }

        if (pollc > 0) {
            if (fds[0].revents & POLLIN) {
                // inotify events are available
                process_inotify_events(&watch);
            }

            if (fds[1].revents & POLLIN) {
                // anonymous pipe events are available
                uint64_t value;
                int len = read(fds[1].fd, &value, sizeof(uint64_t));
                if (len != EOF &&
                    (value & FIMNOTIFY_KILL)) {
                    break;
                }
            }
        }
    }

#if DEBUG
    printf("  Listening for events stopped (pid = %d, sid = %d)\n", pid, sid);
    fflush(stdout);
#endif

exit:
    // free watch cache
    free_cache(&watch);
    // close inotify file descriptor
    if (fd != EOF) {
        close(fd);
    }

    return errno ? EXIT_FAILURE : EXIT_SUCCESS;
}
