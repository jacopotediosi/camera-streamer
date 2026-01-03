#include "device/links.h"
#include "device/device.h"
#include "device/buffer.h"
#include "device/buffer_list.h"
#include "device/buffer_lock.h"
#include "util/opts/log.h"
#include "util/opts/fourcc.h"

#include <inttypes.h>

#define CAPTURE_TIMEOUT_US (1000*1000)
#define STALE_TIMEOUT_US (1000*1000*1000)
#define N_FDS 50

#define MAX_QUEUED_ON_KEYED MAX_BUFFER_QUEUE
#define MAX_QUEUED_ON_NON_KEYED 1
#define MAX_CAPTURED_ON_CAMERA 2
#define MAX_CAPTURED_ON_M2M 2

typedef struct link_pool_s
{
  struct pollfd fds[N_FDS];
  link_t *links[N_FDS];
  buffer_list_t *capture_lists[N_FDS];
  buffer_list_t *output_lists[N_FDS];
} link_pool_t;

static bool link_needs_buffer_by_callbacks(link_t *link)
{
  bool needs = false;

  for (int j = 0; j < link->n_callbacks; j++) {
    if (link->callbacks[j].check_streaming && link->callbacks[j].check_streaming()) {
      needs = true;
    }

    if (link->callbacks[j].buf_lock) {
      bool buf_lock_needs = buffer_lock_needs_buffer(link->callbacks[j].buf_lock);
      if (buf_lock_needs) {
        LOG_INFO(link->capture_list, "link_needs_buffer: %s needs buffer (refs=%d)",
          link->callbacks[j].buf_lock->name, link->callbacks[j].buf_lock->refs);
        needs = true;
      }
    }
  }

  return needs;
}

static bool link_needs_buffer_by_sinks(link_t *link)
{
  bool needs = false;

  for (int j = 0; j < link->n_output_lists; j++) {
    buffer_list_t *output_list = link->output_lists[j];

    if (!output_list->dev->paused) {
      needs = true;
      LOG_INFO(link->capture_list, "link_needs_buffer_by_sinks: %s not paused", output_list->name);
    }
  }

  return needs;
}

static int links_count(link_t *all_links)
{
  int n = 0;
  for (n = 0; all_links[n].capture_list; n++);
  return n;
}

static void links_process_paused(link_t *all_links, bool force_active)
{
  // This traverses in reverse order as it requires to first fix outputs
  // and go back into captures

  for (int i = links_count(all_links); i-- > 0; ) {
    link_t *link = &all_links[i];
    buffer_list_t *capture_list = link->capture_list;

    bool paused = true;
    bool needs_by_callbacks = link_needs_buffer_by_callbacks(link);
    bool needs_by_sinks = link_needs_buffer_by_sinks(link);

    if (force_active) {
      paused = false;
    }

    if (needs_by_callbacks) {
      paused = false;
    }

    if (needs_by_sinks) {
      paused = false;
    }

    bool was_paused = capture_list->dev->paused;
    capture_list->dev->paused = paused;
    
    // Log state changes
    if (was_paused != paused) {
      LOG_INFO(capture_list, "Paused state changed: %d -> %d (force=%d, needs_callbacks=%d, needs_sinks=%d)",
        was_paused, paused, force_active, needs_by_callbacks, needs_by_sinks);
    }
  }
}

static bool links_enqueue_capture_buffers(link_t *link, int *timeout_next_ms)
{
  buffer_list_t *capture_list = link->capture_list;
  buffer_t *capture_buf = NULL;
  uint64_t now_us = get_monotonic_time_us(NULL, NULL);

  if (now_us - capture_list->last_enqueued_us > STALE_TIMEOUT_US && capture_list->dev->output_list == NULL) {
    LOG_INFO(capture_list, "Stale detected. Restarting streaming... (n_callbacks=%d, n_output_lists=%d, last_enqueued_ago_ms=%llu)",
      link->n_callbacks, link->n_output_lists,
      (unsigned long long)(now_us - capture_list->last_enqueued_us) / 1000);
    
    // DEBUG: Log which buffer_locks are connected to this link
    for (int j = 0; j < link->n_callbacks; j++) {
      if (link->callbacks[j].buf_lock) {
        LOG_INFO(capture_list, "  -> Connected buffer_lock: %s (refs=%d, counter=%d)",
          link->callbacks[j].buf_lock->name,
          link->callbacks[j].buf_lock->refs,
          link->callbacks[j].buf_lock->counter);
      }
    }
    
    buffer_list_set_stream(capture_list, false);
    buffer_list_set_stream(capture_list, true);
    
    // DEBUG: Check buffer state after restart
    int enqueued_after = buffer_list_count_enqueued(capture_list);
    buffer_t *slot = buffer_list_find_slot(capture_list);
    LOG_INFO(capture_list, "After restart: enqueued=%d/%d, has_slot=%d, output_list=%s",
      enqueued_after, capture_list->nbufs, slot != NULL,
      capture_list->dev->output_list ? capture_list->dev->output_list->name : "none");
  }

  // skip if all enqueued
  capture_buf = buffer_list_find_slot(capture_list);
  if (capture_buf == NULL) {
    LOG_INFO(capture_list, "No slot available (all buffers busy or enqueued)");
    return false;
  }

  // skip if trying to enqueue to fast
  if (capture_list->fmt.interval_us > 0 && now_us - capture_list->last_enqueued_us < capture_list->fmt.interval_us) {
    *timeout_next_ms = MIN(*timeout_next_ms, (capture_list->last_enqueued_us + capture_list->fmt.interval_us - now_us) / 1000);

    LOG_DEBUG(capture_list, "skipping dequeue: %.1f / %.1f. enqueued=%d",
      (now_us - capture_list->last_enqueued_us) / 1000.0f,
      capture_list->fmt.interval_us / 1000.0f,
      buffer_list_count_enqueued(capture_list));
    return false;
  }

  if (capture_list->fmt.interval_us > 0) {
    LOG_DEBUG(capture_list, "since last: %.1f / %.1f. enqueued=%d",
      (now_us - capture_list->last_enqueued_us) / 1000.0f,
      capture_list->fmt.interval_us / 1000.0f,
      buffer_list_count_enqueued(capture_list));
  }

  // enqueue new capture buffer
  buffer_list_t *output_list = capture_list->dev->output_list;

  // no output, just give back capture_buf
  if (!output_list) {
    // limit amount of buffers enqueued by camera
    if (buffer_list_count_enqueued(capture_list) >= MAX_CAPTURED_ON_CAMERA) {
      LOG_INFO(capture_list, "No enqueue: no output_list, already at max (%d)", MAX_CAPTURED_ON_CAMERA);
      return false;
    }
    
    LOG_INFO(capture_list, "Enqueueing buffer (no output_list path), enqueued_before=%d",
      buffer_list_count_enqueued(capture_list));
    buffer_consumed(capture_buf, "enqueued");
    LOG_INFO(capture_list, "After buffer_consumed: enqueued=%d",
      buffer_list_count_enqueued(capture_list));
    if (capture_list->fmt.interval_us > 0)
      return false;
    return true;
  }

  // limit amount of buffers enqueued by m2m
  if (buffer_list_count_enqueued(output_list) >= MAX_CAPTURED_ON_M2M) {
    LOG_INFO(capture_list, "No enqueue: output_list enqueued >= MAX_CAPTURED_ON_M2M (%d)", MAX_CAPTURED_ON_M2M);
    return false;
  }

  // try to find matching output slot, ignore if not present
  if (!buffer_list_find_slot(output_list)) {
    LOG_INFO(capture_list, "No enqueue: no slot in output_list %s", output_list->name);
    return false;
  }

  bool can_enqueue = false;

  // try to look for output, if there's a matching capture to be consumed
  buffer_t *queued_capture_for_output_buf = buffer_list_pop_from_queue(output_list);
  if (queued_capture_for_output_buf) {
    LOG_INFO(capture_list, "Got queued capture for output, enqueueing...");
    // then push a capture from source into output for this capture
    if (buffer_list_enqueue(output_list, queued_capture_for_output_buf)) {
      buffer_consumed(capture_buf, "enqueued");
      if (capture_list->fmt.interval_us <= 0)
        can_enqueue = true;
    } else {
      queued_capture_for_output_buf->buf_list->stats.dropped++;
    }

    // release this buffer
    buffer_consumed(queued_capture_for_output_buf, "from-queue");
  } else {
    LOG_INFO(capture_list, "No enqueue: no queued capture in output_list %s (queue empty)", output_list->name);
  }

  return can_enqueue;
}

static void links_process_capture_buffers(link_t *all_links, int *timeout_next_ms)
{
  for (int i = 0; all_links[i].capture_list; i++) {
    link_t *link = &all_links[i];
    buffer_list_t *capture_list = link->capture_list;

    if (capture_list->dev->paused)
      continue;

    while (links_enqueue_capture_buffers(link, timeout_next_ms)) {
    }
  }
}

static int links_build_fds(link_t *all_links, link_pool_t *link_pool)
{
  int n = 0;

  for (int i = 0; all_links[i].capture_list; i++) {
    link_t *link = &all_links[i];
    buffer_list_t *capture_list = link->capture_list;

    if (n >= N_FDS) {
      return -EINVAL;
    }

    int count_enqueued = buffer_list_count_enqueued(capture_list);
    bool can_dequeue = count_enqueued > 0;

    if (buffer_list_pollfd(capture_list, &link_pool->fds[n], can_dequeue) == 0) {
      link_pool->capture_lists[n] = capture_list;
      link_pool->links[n] = link;
      n++;
    }

    for (int j = 0; j < link->n_output_lists; j++) {
      buffer_list_t *output_list = link->output_lists[j];

      int count_output_enqueued = buffer_list_count_enqueued(output_list);
      if (count_output_enqueued == 0) {
        continue;
      }

      if (n >= N_FDS) {
        return -EINVAL;
      }

      int count_capture_enqueued = buffer_list_count_enqueued(output_list->dev->capture_lists[0]);

      // Can something be dequeued?
      if (buffer_list_pollfd(output_list, &link_pool->fds[n], count_output_enqueued > count_capture_enqueued) == 0) {
        link_pool->output_lists[n] = output_list;
        link_pool->links[n] = NULL;
        n++;
      }
    }
  }

  return n;
}

static int links_enqueue_from_capture_list(buffer_list_t *capture_list, link_t *link)
{
  if (!link) {
    LOG_ERROR(capture_list, "Missing link for capture_list");
  }

  buffer_t *buf = buffer_list_dequeue(capture_list);
  if (!buf) {
    LOG_ERROR(capture_list, "No buffer dequeued from capture_list?");
  }

  LOG_INFO(capture_list, "Frame dequeued (n_callbacks=%d, n_output_lists=%d, is_keyframe=%d)",
    link->n_callbacks, link->n_output_lists, buf->flags.is_keyframe);

  if (buf->flags.is_last) {
    LOG_INFO(buf, "Received last buffer. Restarting streaming...");
    buffer_list_set_stream(capture_list, false);
    buffer_list_set_stream(capture_list, true);
    return 0;
  }

  uint64_t now_us = get_monotonic_time_us(NULL, NULL);
  if ((now_us - buf->captured_time_us) > CAPTURE_TIMEOUT_US) {
    LOG_INFO(buf, "Capture image is outdated. Skipped. Now: %" PRIu64 ", vs %" PRIu64 ".",
      now_us, buf->captured_time_us);
    return 0;
  }

  bool dropped = false;

  int max_bufs_queued = buf->flags.is_keyed ? MAX_QUEUED_ON_KEYED : MAX_QUEUED_ON_NON_KEYED;

  for (int j = 0; j < link->n_output_lists; j++) {
    if (link->output_lists[j]->dev->paused) {
      LOG_INFO(capture_list, "Skipping output %s: device paused", link->output_lists[j]->name);
      continue;
    }
    if (buf->flags.is_keyframe) {
      buffer_list_clear_queue(link->output_lists[j]);
    }
    bool pushed = buffer_list_push_to_queue(link->output_lists[j], buf, max_bufs_queued);
    LOG_INFO(capture_list, "Push to %s: %s (queue_size=%d)",
      link->output_lists[j]->name, pushed ? "OK" : "FAILED/FULL",
      link->output_lists[j]->n_queued_bufs);
    if (!pushed) {
      dropped = true;
    }
  }

  if (dropped) {
    capture_list->stats.dropped++;
  }

  for (int j = 0; j < link->n_callbacks; j++) {
    if (link->callbacks[j].on_buffer) {
      link->callbacks[j].on_buffer(buf);
    }

    if (link->callbacks[j].buf_lock) {
      LOG_INFO(capture_list, "Delivering frame to buffer_lock %s (refs=%d)",
        link->callbacks[j].buf_lock->name, link->callbacks[j].buf_lock->refs);
      buffer_lock_capture(link->callbacks[j].buf_lock, buf);
    }
  }

  return 0;

error:
  return -1;
}

static int links_dequeue_from_output_list(buffer_list_t *output_list)
{
  buffer_t *buf = buffer_list_dequeue(output_list);
  if (!buf) {
    LOG_ERROR(buf, "No buffer dequeued from sink?");
  }

  return 0;

error:
  return -1;
}

static void print_pollfds(struct pollfd *fds, int n)
{
  if (!getenv("DEBUG_FDS")) {
    return;
  }

  for (int i = 0; i < n; i++) {
    printf("poll(i=%i, fd=%d, events=%08x, revents=%08x)\n", i, fds[i].fd, fds[i].events, fds[i].revents);
  }
  printf("pollfds = %d\n", n);
}

static int links_step(link_t *all_links, bool force_active, int timeout_now_ms, int *timeout_next_ms)
{
  link_pool_t pool = {
    .fds = {{0}},
    .links = {0},
    .capture_lists = {0},
    .output_lists = {0}
  };

  links_process_paused(all_links, force_active);
  links_process_capture_buffers(all_links, timeout_next_ms);

  int n = links_build_fds(all_links, &pool);
  print_pollfds(pool.fds, n);
  int ret = poll(pool.fds, n, timeout_now_ms);
  print_pollfds(pool.fds, n);

  if (ret < 0 && errno != EINTR) {
    LOG_INFO(NULL, "poll() failed: errno=%d (%s)", errno, strerror(errno));
    return errno;
  }

  // Log poll results when something happens
  if (ret > 0) {
    for (int i = 0; i < n; i++) {
      if (pool.fds[i].revents) {
        buffer_list_t *buf_list = pool.capture_lists[i] ? pool.capture_lists[i] : pool.output_lists[i];
        LOG_DEBUG(buf_list, "poll returned: revents=%s%s%s%s (fd=%d)",
          pool.fds[i].revents & POLLIN ? "IN " : "",
          pool.fds[i].revents & POLLOUT ? "OUT " : "",
          pool.fds[i].revents & POLLHUP ? "HUP " : "",
          pool.fds[i].revents & POLLERR ? "ERR " : "",
          pool.fds[i].fd);
      }
    }
  }

  for (int i = 0; i < n; i++) {
    buffer_list_t *capture_list = pool.capture_lists[i];
    buffer_list_t *output_list = pool.output_lists[i];
    buffer_list_t *buf_list = capture_list ? capture_list : output_list;
    link_t *link = pool.links[i];

    LOG_DEBUG(buf_list, "pool event=%08x revent=%s%s%s%s%s%08x streaming=%d enqueued=%d/%d paused=%d",
      pool.fds[i].events,
      !pool.fds[i].revents ? "NONE/" : "",
      pool.fds[i].revents & POLLIN ? "IN/" : "",
      pool.fds[i].revents & POLLOUT ? "OUT/" : "",
      pool.fds[i].revents & POLLHUP ? "HUP/" : "",
      pool.fds[i].revents & POLLERR ? "ERR/" : "",
      pool.fds[i].revents,
      buf_list->streaming,
      buffer_list_count_enqueued(buf_list),
      buf_list->nbufs,
      buf_list->dev->paused);

    if (pool.fds[i].revents & POLLIN) {
      if (links_enqueue_from_capture_list(capture_list, link) < 0) {
        return -1;
      }
    }

    // Dequeue buffers that were processed
    if (pool.fds[i].revents & POLLOUT) {
      if (links_dequeue_from_output_list(output_list) < 0) {
        return -1;
      }
    }

    if (pool.fds[i].revents & POLLHUP) {
      LOG_INFO(buf_list, "Device disconnected.");
      return -1;
    }

    if (pool.fds[i].revents & POLLERR) {
      LOG_INFO(buf_list, "Got an error");
      return -1;
    }
  }
  return 0;
}

static int links_stream(link_t *all_links, bool do_stream)
{
  for (int i = 0; all_links[i].capture_list; i++) {
    bool streaming = true;
    link_t *link = &all_links[i];

    if (buffer_list_set_stream(link->capture_list, streaming) < 0) {
      LOG_ERROR(link->capture_list, "Failed to start streaming");
    }

    for (int j = 0; j < link->n_output_lists; j++) {
      if (buffer_list_set_stream(link->output_lists[j], streaming) < 0) {
        LOG_ERROR(link->output_lists[j], "Failed to start streaming");
      }
    }
  }

  return 0;

error:
  return -1;
}

static void links_refresh_stats(link_t *all_links, uint64_t *last_refresh_us)
{
  uint64_t now_us = get_monotonic_time_us(NULL, NULL);

  if (now_us - *last_refresh_us < 1000*1000)
    return;

  *last_refresh_us = now_us;

  if (log_options.stats) {
    printf("Statistics:");

    for (int i = 0; all_links[i].capture_list; i++) {
      buffer_list_t *capture_list = all_links[i].capture_list;
      buffer_stats_t *now = &capture_list->stats;
      buffer_stats_t *prev = &capture_list->stats_last;

      printf(" [%8s %2d FPS/%2d D/%3dms/%3dms/Dev%3.fms/%c/Q%d:O%d:C%d]",
        capture_list->dev->name,
        (now->frames - prev->frames) / log_options.stats,
        (now->dropped - prev->dropped) / log_options.stats,
        capture_list->last_capture_time_us > 0 ? capture_list->last_capture_time_us / 1000 : -1,
        capture_list->last_in_queue_time_us > 0 ? capture_list->last_in_queue_time_us / 1000 : -1,
        // (float)(now->max_dequeued_us / 1000),
        // (float)(now->avg_dequeued_us / 1000),
        (float)(now->stddev_dequeued_us / 1000),
        capture_list->streaming ? (capture_list->dev->paused ? 'P' : 'S') : 'X',
        capture_list->dev->output_list ? capture_list->dev->output_list->n_queued_bufs : 0,
        capture_list->dev->output_list ? buffer_list_count_enqueued(capture_list->dev->output_list) : 0,
        buffer_list_count_enqueued(capture_list)
      );
    }

    printf("\n");
    fflush(stdout);
  }

  for (int i = 0; all_links[i].capture_list; i++) {
    buffer_list_t *capture_list = all_links[i].capture_list;
    capture_list->stats_last = capture_list->stats;

    capture_list->stats.max_dequeued_us = 0;
    capture_list->stats.avg_dequeued_us = 0;
    capture_list->stats.stddev_dequeued_us = 0;
    capture_list->stats.frames_since_reset = 0;

    if (now_us - capture_list->last_dequeued_us > 1000) {
      capture_list->last_capture_time_us = 0;
      capture_list->last_in_queue_time_us = 0;
    }
  }
}

int links_loop(link_t *all_links, bool force_active, bool *running)
{
  *running = true;

  if (links_stream(all_links, true) < 0) {
    return -1;
  }

  int timeout_ms = LINKS_LOOP_INTERVAL;
  uint64_t last_refresh_us = get_monotonic_time_us(NULL, NULL);
  int ret = 0;

  while(*running && ret == 0) {
    int timeout_now_ms = timeout_ms;
    timeout_ms = LINKS_LOOP_INTERVAL;

    ret = links_step(all_links, force_active, timeout_now_ms, &timeout_ms);
    links_refresh_stats(all_links, &last_refresh_us);
  }

  links_stream(all_links, false);
  return ret;
}

static void links_dump_buf_list(char *output, buffer_list_t *buf_list)
{
  sprintf(output + strlen(output), "%s[%dx%d/%s/%d]", \
    buf_list->name, buf_list->fmt.width, buf_list->fmt.height, \
    fourcc_to_string(buf_list->fmt.format).buf, buf_list->fmt.nbufs);
}

void links_dump(link_t *all_links)
{
  char line[4096];

  for (int n = 0; all_links[n].capture_list; n++) {
    link_t *link = &all_links[n];

    line[0] = 0;
    links_dump_buf_list(line, link->capture_list);
    strcat(line, " => [");
    for (int j = 0; j < link->n_output_lists; j++) {
      if (j > 0)
        strcat(line, ", ");
      links_dump_buf_list(line, link->output_lists[j]);
    }

    for (int j = 0; j < link->n_callbacks; j++) {
      if (link->output_lists[0] || j > 0)
        strcat(line, ", ");
      strcat(line, link->callbacks[j].name);
    }
    strcat(line, "]");

    LOG_INFO(NULL, "Link %d: %s", n, line);
  }
}
