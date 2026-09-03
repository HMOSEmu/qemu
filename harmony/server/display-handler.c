#include "qemu/osdep.h"
#include "qemu/main-loop.h"
#include "qemu/option.h"
#include "harmony-server.h"
#include "display-handler.h"
#include <jpeglib.h>
#include "qemu/error-report.h"

#define HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_DEFAULT 30
#define HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_INC  10
#define HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_MAX  3000
#define HARMONY_SERVER_DISPLAY_QUAlITY_DEFAULT 50
#define HARMONY_SERVER_DISPLAY_QUAlITY_MAX 100

static void harmony_server_display_refresh_surface(HarmonyServerDisplay *hsd)
{
    int width, height;

    if (hsd->guest_fb == NULL || hsd->server_fb == NULL) {
        return;
    }

    width = pixman_image_get_width(hsd->guest_fb);
    height = pixman_image_get_height(hsd->guest_fb);
    int server_stride, line_bytes, guest_ll, guest_stride, y;
    uint8_t *guest_row0 = NULL, *server_row0;
    pixman_image_t *tmpbuf = NULL;
    uint8_t *guest_ptr, *server_ptr;

    server_row0 = (uint8_t *)pixman_image_get_data(hsd->server_fb);
    server_stride = guest_stride = guest_ll =
        pixman_image_get_stride(hsd->server_fb);
    
    if (hsd->guest_format != HARMONY_SERVER_DISPLAY_FB_FORMAT) {
        int w = pixman_image_get_width(hsd->server_fb);
        tmpbuf = qemu_pixman_linebuf_create(HARMONY_SERVER_DISPLAY_FB_FORMAT, w);
    } else {
        int guest_bpp =
            PIXMAN_FORMAT_BPP(pixman_image_get_format(hsd->guest_fb));
        guest_row0 = (uint8_t *)pixman_image_get_data(hsd->guest_fb);
        guest_stride = pixman_image_get_stride(hsd->guest_fb);
        guest_ll = pixman_image_get_width(hsd->guest_fb)
                   * DIV_ROUND_UP(guest_bpp, 8);
    }
    line_bytes = MIN(server_stride, guest_ll);

    for (y = 0; y < height; ++y) {
        server_ptr = server_row0 + y * server_stride;

        if (hsd->guest_format != HARMONY_SERVER_DISPLAY_FB_FORMAT) {
            qemu_pixman_linebuf_fill(tmpbuf, hsd->guest_fb, width, 0, y);
            guest_ptr = (uint8_t *)pixman_image_get_data(tmpbuf);
        } else {
            guest_ptr = guest_row0 + y * guest_stride;
        }

        memcpy(server_ptr, guest_ptr, line_bytes);
    }

    qemu_pixman_image_unref(tmpbuf);

    hsd->fb_seq++;
}

static void harmony_server_display_lock_queue(HarmonyServerDisplayJobQueue *queue)
{
    qemu_mutex_lock(&queue->mutex);
}

static void harmony_server_display_unlock_queue(HarmonyServerDisplayJobQueue *queue)
{
    qemu_mutex_unlock(&queue->mutex);
}

static HarmonyServerDisplayJob *harmony_server_display_job_new(HarmonyClientDisplay *hcd)
{
    HarmonyServerDisplayJob *job = g_new0(HarmonyServerDisplayJob, 1);

    assert(hcd->hc->magic == HARMONY_SERVER_MAGIC);

    job->hcd = hcd;
    job->seq = hcd->hsd->fb_seq;

    return job;
}

static void harmony_server_display_job_push(HarmonyServerDisplay *hsd, 
                                        HarmonyServerDisplayJob *job)
{
    HarmonyServerDisplayJobQueue *queue = hsd->queue;

    harmony_server_display_lock_queue(queue);
    if (queue->exit) {
        g_free(job);
    } else {
        QTAILQ_INSERT_TAIL(&queue->jobs, job, next);
        qemu_cond_broadcast(&queue->cond);
    }
    harmony_server_display_unlock_queue(queue);
}

static void harmony_server_display_update_client(HarmonyClientDisplay *hcd)
{
    HarmonyServerDisplay *hsd = hcd->hsd;
    HarmonyClient *hc = hcd->hc;
    HarmonyServerDisplayJob *job;

    if (hc->disconnecting) {
        harmony_server_disconnect_finish(hc);
        return;
    }

    job = harmony_server_display_job_new(hcd);
    harmony_server_display_job_push(hsd, job);
}

static void harmony_server_display_refresh(DisplayChangeListener *dcl)
{
    HarmonyServerDisplay *hsd = container_of(dcl, HarmonyServerDisplay, dcl);
    HarmonyClientDisplay *hcd, *hcdn;

    if (QTAILQ_EMPTY(&hsd->clients)) {
        update_displaychangelistener(&hsd->dcl, HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_MAX);
        return;
    }

    if (!hsd->has_dirty) {
        hsd->dcl.update_interval += HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_INC;
        if (hsd->dcl.update_interval > HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_MAX) {
            hsd->dcl.update_interval = HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_MAX;
        }
        return;
    }

    hsd->has_dirty = 0;

    graphic_hw_update(hsd->dcl.con);

    if (harmony_server_trylock_display(hsd)) {
        update_displaychangelistener(&hsd->dcl, hsd->refresh_interval);
        return;
    }

    harmony_server_display_refresh_surface(hsd);
    harmony_server_unlock_display(hsd);
    QTAILQ_FOREACH_SAFE(hcd, &hsd->clients, next, hcdn) {
        harmony_server_display_update_client(hcd);
    }

    hsd->dcl.update_interval /= 2;
    if (hsd->dcl.update_interval < hsd->refresh_interval) {
        hsd->dcl.update_interval = hsd->refresh_interval;
    }
}

static void harmony_server_display_update(DisplayChangeListener *dcl, 
                                        int x, int y, int w, int h)
{
    HarmonyServerDisplay *hsd = container_of(dcl, HarmonyServerDisplay, dcl);
    
    hsd->has_dirty += 1;
}

static int harmony_server_display_width(HarmonyServerDisplay *hsd)
{
    return surface_width(hsd->ds);
}

static int harmony_server_display_height(HarmonyServerDisplay *hsd)
{
    return surface_height(hsd->ds);
}

static void harmony_server_display_update_surface(HarmonyServerDisplay *hsd)
{
    /*
     * The JPEG worker thread reads hsd->server_fb under hsd->mutex
     * (see harmony_server_display_worker_thread_loop).  Replacing the
     * image without holding that mutex frees it while the worker is
     * still walking it inside pixman, which crashes pixman with
     * "unknown image type" / NULL deref.  Take the mutex so an encode
     * in flight finishes before the image is dropped.
     */
    harmony_server_lock_display(hsd);

    qemu_pixman_image_unref(hsd->server_fb);
    hsd->server_fb = NULL;

    if (QTAILQ_EMPTY(&hsd->clients)) {
        harmony_server_unlock_display(hsd);
        return;
    }

    hsd->width = harmony_server_display_width(hsd);
    hsd->height = harmony_server_display_height(hsd);

    if (hsd->width > 0 && hsd->height > 0) {
        hsd->server_fb = pixman_image_create_bits(HARMONY_SERVER_DISPLAY_FB_FORMAT,
                                                  hsd->width, hsd->height,
                                                  NULL, 0);
    } else {
        warn_report("harmony server: invalid display surface %dx%d, "
                    "framebuffer not updated", hsd->width, hsd->height);
    }

    harmony_server_unlock_display(hsd);
}

static void harmony_server_display_switch(DisplayChangeListener *dcl, 
                                        DisplaySurface *surface)
{
    HarmonyServerDisplay *hsd = container_of(dcl, HarmonyServerDisplay, dcl);

    hsd->ds = surface;

    qemu_pixman_image_unref(hsd->guest_fb);
    hsd->guest_fb = pixman_image_ref(surface->image);
    hsd->guest_format = surface_format(surface);

    harmony_server_display_update_surface(hsd);
}

static const DisplayChangeListenerOps dcl_ops = {
    .dpy_name             = "harmony server display",
    .dpy_refresh          = harmony_server_display_refresh,
    .dpy_gfx_update       = harmony_server_display_update,
    .dpy_gfx_switch       = harmony_server_display_switch,
    .dpy_gfx_check_format = qemu_pixman_check_format,
};

static void jpeg_init_destination(j_compress_ptr cinfo)
{
    HarmonyServerDisplay *hsd = cinfo->client_data;
    Buffer *buffer = &hsd->jpeg;

    cinfo->dest->next_output_byte = (JOCTET *)buffer->buffer + buffer->offset;
    cinfo->dest->free_in_buffer = (size_t)(buffer->capacity - buffer->offset);
}

static boolean jpeg_empty_output_buffer(j_compress_ptr cinfo)
{
    HarmonyServerDisplay *hsd = cinfo->client_data;
    Buffer *buffer = &hsd->jpeg;

    buffer->offset = buffer->capacity;
    buffer_reserve(buffer, 2048);
    jpeg_init_destination(cinfo);
    return TRUE;
}

static void jpeg_term_destination(j_compress_ptr cinfo)
{
    HarmonyServerDisplay *hsd = cinfo->client_data;
    Buffer *buffer = &hsd->jpeg;

    buffer->offset = buffer->capacity - cinfo->dest->free_in_buffer;
}

static void harmony_server_display_compress_jpeg(HarmonyClientDisplay *hcd)
{
    HarmonyServerDisplay *hsd = hcd->hsd;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    struct jpeg_destination_mgr manager;
    pixman_image_t *linebuf;
    JSAMPROW row[1];
    uint8_t *buf;
    int dy;
    int width, height;

    if (hsd->server_fb == NULL) {
        return;
    }

    width = pixman_image_get_width(hsd->server_fb);
    height = pixman_image_get_height(hsd->server_fb);

    buffer_reset(&hsd->jpeg);
    buffer_reserve(&hsd->jpeg, 2048);

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    cinfo.client_data = hsd;
    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, hsd->quality, true);

    manager.init_destination = jpeg_init_destination;
    manager.empty_output_buffer = jpeg_empty_output_buffer;
    manager.term_destination = jpeg_term_destination;
    cinfo.dest = &manager;

    jpeg_start_compress(&cinfo, true);

    linebuf = qemu_pixman_linebuf_create(PIXMAN_BE_r8g8b8, width);
    buf = (uint8_t *)pixman_image_get_data(linebuf);
    row[0] = buf;
    for (dy = 0; dy < height; dy++) {
        qemu_pixman_linebuf_fill(linebuf, hsd->server_fb, width, 0, dy);
        jpeg_write_scanlines(&cinfo, row, 1);
    }
    qemu_pixman_image_unref(linebuf);

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);

    hsd->jpeg_seq = hsd->fb_seq;

    harmony_server_write(hcd->hc, hsd->jpeg.buffer, hsd->jpeg.offset);
}

static void harmony_server_display_async_encode_start(HarmonyClientDisplay *orig, 
                                                    HarmonyClientDisplay *local)
{
    local->hsd = orig->hsd;
    buffer_init(&local->hc->output, "harmony-server-display-worker-output");
    local->hc->magic = HARMONY_SERVER_MAGIC;
    local->hc->disconnecting = FALSE;
    local->hc->sioc = NULL;
    local->hc->ioc = NULL;
}

static void harmony_server_display_async_encode_end(HarmonyClientDisplay *orig, 
                                                HarmonyClientDisplay *local)
{
    buffer_free(&local->hc->output);
    local->hc->magic = 0;
}

static int harmony_server_display_worker_thread_loop(HarmonyServerDisplay *hsd)
{
    HarmonyServerDisplayJobQueue *queue = hsd->queue;
    HarmonyServerDisplayJob *job;
    HarmonyClientDisplay hcd;
    HarmonyClient hc;

    hcd.hc = &hc;

    harmony_server_display_lock_queue(queue);
    while (QTAILQ_EMPTY(&queue->jobs) && !queue->exit) {
        qemu_cond_wait(&queue->cond, &queue->mutex);
    }
    job = QTAILQ_FIRST(&queue->jobs);
    harmony_server_display_unlock_queue(queue);

    if (queue->exit) {
        return -1;
    }

    assert(job->hcd->hc->magic == HARMONY_SERVER_MAGIC);

    harmony_server_lock_output(job->hcd->hc);
    if (job->hcd->hc->ioc == NULL) {
        harmony_server_unlock_output(job->hcd->hc);
        goto disconnected;
    }
    harmony_server_unlock_output(job->hcd->hc);

    harmony_server_display_async_encode_start(job->hcd, &hcd);

    harmony_server_lock_display(hsd);
    if (job->hcd->hc->ioc == NULL) {
        harmony_server_unlock_display(hsd);
        harmony_server_display_async_encode_end(job->hcd, &hcd);
        goto disconnected;
    }

    if (job->seq <= hsd->jpeg_seq) {
        harmony_server_write(hcd.hc, hsd->jpeg.buffer, hsd->jpeg.offset);
    } else {
        harmony_server_display_compress_jpeg(&hcd);
    }
    harmony_server_unlock_display(hsd);

    harmony_server_lock_output(job->hcd->hc);
    if (job->hcd->hc->ioc != NULL) {
        buffer_move(&job->hcd->jpeg, &hcd.hc->output);
        harmony_server_display_async_encode_end(job->hcd, &hcd);

        qemu_bh_schedule(job->hcd->bh);
    }  else {
        buffer_reset(&hcd.hc->output);
        harmony_server_display_async_encode_end(job->hcd, &hcd);
    }
    harmony_server_unlock_output(job->hcd->hc);

disconnected:
    harmony_server_display_lock_queue(queue);
    QTAILQ_REMOVE(&queue->jobs, job, next);
    harmony_server_display_unlock_queue(queue);
    qemu_cond_broadcast(&queue->cond);
    g_free(job);
    return 0;
}

static void harmony_server_display_queue_init(HarmonyServerDisplay *hsd)
{
    HarmonyServerDisplayJobQueue *q = g_new0(HarmonyServerDisplayJobQueue, 1);

    qemu_cond_init(&q->cond);
    qemu_mutex_init(&q->mutex);
    QTAILQ_INIT(&q->jobs);

    hsd->queue = q;
}

static void *harmony_server_display_worker_thread(void *arg)
{
    HarmonyServerDisplay *hsd = arg;

    qemu_thread_get_self(&hsd->queue->thread);

    while (!harmony_server_display_worker_thread_loop(hsd));

    return NULL;
}

static void harmony_server_display_start_worker_thread(HarmonyServerDisplay *hsd)
{
    if (hsd->queue->thread.thread)
        return;

    qemu_thread_create(&hsd->queue->thread, 
                    "harmony_server_display_worker", 
                    harmony_server_display_worker_thread, hsd,
                    QEMU_THREAD_DETACHED);
}

static void harmony_server_display_consume_buffer(HarmonyClientDisplay *hcd)
{
    harmony_server_lock_output(hcd->hc);
    if (hcd->jpeg.offset) {
        if (hcd->hc->ioc != NULL && buffer_empty(&hcd->hc->output)) {
            if (hcd->hc->ioc_tag) {
                g_source_remove(hcd->hc->ioc_tag);
            }
            if (hcd->hc->disconnecting == FALSE) {
                hcd->hc->ioc_tag = qio_channel_add_watch(
                    hcd->hc->ioc, G_IO_IN | G_IO_HUP | G_IO_ERR | G_IO_OUT,
                    harmony_server_client_io, hcd->hc, NULL);
            }
        }
        // always send the latest jpeg
        buffer_reset(&hcd->hc->output);
        buffer_move(&hcd->hc->output, &hcd->jpeg);
    }
    harmony_server_unlock_output(hcd->hc);
}

static void harmony_server_display_bh(void *opaque)
{
    HarmonyClientDisplay *hcd = opaque;

    assert(hcd->hc->magic == HARMONY_SERVER_MAGIC);
    harmony_server_display_consume_buffer(hcd);
}

static gboolean harmony_server_display_connect(HarmonyClient *hc, void *opaque)
{
    HarmonyServerDisplay *hsd = opaque;
    bool first_client = QTAILQ_EMPTY(&hsd->clients);
    HarmonyClientDisplay *hcd = g_new0(HarmonyClientDisplay, 1);
    hcd->hsd = hsd;
    hcd->hc = hc;
    hcd->bh = qemu_bh_new(harmony_server_display_bh, hcd);
    buffer_init(&hcd->jpeg, "harmony-client-display-jpeg");

    QTAILQ_INSERT_TAIL(&hsd->clients, hcd, next);
    if (first_client) {
        harmony_server_display_update_surface(hsd);
    }

    hsd->has_dirty += 1;

    return TRUE;
}

static int harmony_server_display_read(HarmonyClient *hc, void *opaque, uint8_t *data, size_t len) 
{
    return len;
}

static bool harmony_server_display_has_job_locked(HarmonyClientDisplay *hcd)
{
    HarmonyServerDisplay *hsd = hcd->hsd;
    HarmonyServerDisplayJobQueue *queue = hsd->queue;
    HarmonyServerDisplayJob *job;

    QTAILQ_FOREACH(job, &queue->jobs, next) {
        if (job->hcd == hcd) {
            return true;
        }
    }

    return false;
}

static void harmony_server_display_drain_jobs(HarmonyClientDisplay *hcd)
{
    HarmonyServerDisplayJobQueue *queue = hcd->hsd->queue;

    harmony_server_display_lock_queue(queue);
    while (harmony_server_display_has_job_locked(hcd)) {
        qemu_cond_wait(&queue->cond, &queue->mutex);
    }
    harmony_server_display_unlock_queue(queue);
}

static void harmony_server_display_disconnect(HarmonyClient *hc, void *opaque) 
{
    HarmonyServerDisplay *hsd = opaque;
    HarmonyClientDisplay *hcd;
    bool found = false;

    QTAILQ_FOREACH(hcd, &hsd->clients, next) {
        if (hcd->hc == hc) { 
            found = true;
            break;
        }
    }

    if (found) {
        QTAILQ_REMOVE(&hsd->clients, hcd, next);

        harmony_server_display_drain_jobs(hcd);

        buffer_free(&hcd->jpeg);

        if (hcd->bh != NULL) {
            qemu_bh_delete(hcd->bh);
        }

        g_free(hcd);
    }
}

HarmonyServerDisplay* harmony_server_display_init(QemuOpts *opts)
{
    HarmonyServerDisplay *hsd = g_new0(HarmonyServerDisplay, 1);
    qemu_mutex_init(&hsd->mutex);
    QTAILQ_INIT(&hsd->clients);
    hsd->fb_seq = 0;
    hsd->jpeg_seq = 0;
    int quality = qemu_opt_get_number(opts, "quality", HARMONY_SERVER_DISPLAY_QUAlITY_DEFAULT);
    if (quality > 0 && quality <= HARMONY_SERVER_DISPLAY_QUAlITY_MAX) {
        hsd->quality = quality;
    } else {
        warn_report("invalid quality(1-%d):%d, use default:%d", \
            HARMONY_SERVER_DISPLAY_QUAlITY_MAX, quality, \
            HARMONY_SERVER_DISPLAY_QUAlITY_DEFAULT);
        hsd->quality = HARMONY_SERVER_DISPLAY_QUAlITY_DEFAULT;
    }
    int interval = qemu_opt_get_number(opts, "refresh_interval", HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_DEFAULT);
    if (interval > 0 && interval <= HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_MAX) {
        hsd->refresh_interval = interval;
    } else {
        warn_report("invalid refresh_interval(1-%d):%d, use default:%d", \
            HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_MAX, interval, \
            HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_DEFAULT);
        hsd->refresh_interval = HARMONY_SERVER_DISPLAY_REFRESH_INTERVAL_DEFAULT;
    }

    harmony_server_display_queue_init(hsd);

    harmony_server_add_path_handler("/display", 
                                harmony_server_display_connect, 
                                harmony_server_display_read, 
                                harmony_server_display_disconnect, 
                                hsd);

    hsd->dcl.ops = &dcl_ops;
    register_displaychangelistener(&hsd->dcl);

    harmony_server_display_start_worker_thread(hsd);

    return hsd;
}
