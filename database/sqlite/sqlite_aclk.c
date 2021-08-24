// SPDX-License-Identifier: GPL-3.0-or-later

#include "sqlite_functions.h"
#include "sqlite_aclk.h"
#include "sqlite_aclk_chart.h"
#include "sqlite_aclk_alert.h"
#include "sqlite_aclk_node.h"

#ifndef ACLK_NG
#include "../../aclk/legacy/agent_cloud_link.h"
#else
#include "../../aclk/aclk.h"
#include "../../aclk/aclk_charts_api.h"
#include "../../aclk/aclk_alarm_api.h"
#endif

const char *aclk_sync_config[] = {
    NULL,
    "CREATE TABLE IF NOT EXISTS delete_dimension(host_id, chart_id, dim_id, chart_name, dimension_id, dimension_name);"
    "CREATE TRIGGER IF NOT EXISTS tr_del_dim AFTER DELETE ON dimension " \
    "BEGIN INSERT INTO delete_dimension (host_id, chart_id, dim_id, chart_name, dimension_id, dimension_name) " \
    "SELECT c.host_id, c.chart_id, old.dim_id, c.type||'.'||c.id, old.id, old.name from chart c where c.chart_id = old.chart_id; end;",
    NULL
};

int aclk_architecture = 0;

uv_mutex_t aclk_async_lock;
struct aclk_database_worker_config  *aclk_thread_head = NULL;

static inline int claimed()
{
    int rc;
    rrdhost_aclk_state_lock(localhost);
    rc = (localhost->aclk_state.claimed_id != NULL);
    rrdhost_aclk_state_unlock(localhost);
    return rc;
}

void aclk_add_worker_thread(struct aclk_database_worker_config *wc)
{
    if (unlikely(!wc))
        return;

    uv_mutex_lock(&aclk_async_lock);
    if (unlikely(!wc->host)) {
        wc->next = aclk_thread_head;
        aclk_thread_head = wc;
    }
    uv_mutex_unlock(&aclk_async_lock);
    return;
}

void aclk_del_worker_thread(struct aclk_database_worker_config *wc)
{
    if (unlikely(!wc))
        return;

    uv_mutex_lock(&aclk_async_lock);
    struct aclk_database_worker_config **tmp = &aclk_thread_head;
    while (*tmp && (*tmp) != wc)
        tmp = &(*tmp)->next;
    if (*tmp)
        *tmp = wc->next;
    uv_mutex_unlock(&aclk_async_lock);
    return;
}

int aclk_worker_thread_exists(char *guid)
{
    int rc = 0;
    uv_mutex_lock(&aclk_async_lock);

    struct aclk_database_worker_config *tmp = aclk_thread_head;

    while (tmp && !rc) {
        rc = strcmp(tmp->uuid_str, guid) == 0;
        tmp = tmp->next;
    }
    uv_mutex_unlock(&aclk_async_lock);
    return rc;
}

void aclk_database_init_cmd_queue(struct aclk_database_worker_config *wc)
{
    wc->cmd_queue.head = wc->cmd_queue.tail = 0;
    wc->queue_size = 0;
    fatal_assert(0 == uv_cond_init(&wc->cmd_cond));
    fatal_assert(0 == uv_mutex_init(&wc->cmd_mutex));
}

void aclk_database_enq_cmd_nowake(struct aclk_database_worker_config *wc, struct aclk_database_cmd *cmd)
{
    unsigned queue_size;

    /* wait for free space in queue */
    uv_mutex_lock(&wc->cmd_mutex);
    while ((queue_size = wc->queue_size) == ACLK_DATABASE_CMD_Q_MAX_SIZE) {
        uv_cond_wait(&wc->cmd_cond, &wc->cmd_mutex);
    }
    fatal_assert(queue_size < ACLK_DATABASE_CMD_Q_MAX_SIZE);
    /* enqueue command */
    wc->cmd_queue.cmd_array[wc->cmd_queue.tail] = *cmd;
    wc->cmd_queue.tail = wc->cmd_queue.tail != ACLK_DATABASE_CMD_Q_MAX_SIZE - 1 ?
                         wc->cmd_queue.tail + 1 : 0;
    wc->queue_size = queue_size + 1;
    uv_mutex_unlock(&wc->cmd_mutex);
}

int aclk_database_enq_cmd_noblock(struct aclk_database_worker_config *wc, struct aclk_database_cmd *cmd)
{
    unsigned queue_size;

    /* wait for free space in queue */
    uv_mutex_lock(&wc->cmd_mutex);
    if ((queue_size = wc->queue_size) == ACLK_DATABASE_CMD_Q_MAX_SIZE) {
        uv_mutex_unlock(&wc->cmd_mutex);
        return 1;
    }

    fatal_assert(queue_size < ACLK_DATABASE_CMD_Q_MAX_SIZE);
    /* enqueue command */
    wc->cmd_queue.cmd_array[wc->cmd_queue.tail] = *cmd;
    wc->cmd_queue.tail = wc->cmd_queue.tail != ACLK_DATABASE_CMD_Q_MAX_SIZE - 1 ?
                         wc->cmd_queue.tail + 1 : 0;
    wc->queue_size = queue_size + 1;
    uv_mutex_unlock(&wc->cmd_mutex);

    /* wake up event loop */
    //fatal_assert(0 == uv_async_send(&wc->async));
    return 0;
}

void aclk_database_enq_cmd(struct aclk_database_worker_config *wc, struct aclk_database_cmd *cmd)
{
    unsigned queue_size;

    /* wait for free space in queue */
    uv_mutex_lock(&wc->cmd_mutex);
    while ((queue_size = wc->queue_size) == ACLK_DATABASE_CMD_Q_MAX_SIZE) {
        uv_cond_wait(&wc->cmd_cond, &wc->cmd_mutex);
    }
    fatal_assert(queue_size < ACLK_DATABASE_CMD_Q_MAX_SIZE);
    /* enqueue command */
    wc->cmd_queue.cmd_array[wc->cmd_queue.tail] = *cmd;
    wc->cmd_queue.tail = wc->cmd_queue.tail != ACLK_DATABASE_CMD_Q_MAX_SIZE - 1 ?
                         wc->cmd_queue.tail + 1 : 0;
    wc->queue_size = queue_size + 1;
    uv_mutex_unlock(&wc->cmd_mutex);

    /* wake up event loop */
    int rc = uv_async_send(&wc->async);
    if (unlikely(rc))
        debug(D_ACLK_SYNC, "Failed to wake up event loop");
}

struct aclk_database_cmd aclk_database_deq_cmd(struct aclk_database_worker_config* wc)
{
    struct aclk_database_cmd ret;
    unsigned queue_size;

    uv_mutex_lock(&wc->cmd_mutex);
    queue_size = wc->queue_size;
    if (queue_size == 0) {
        ret.opcode = ACLK_DATABASE_NOOP;
        ret.completion = NULL;
    } else {
        /* dequeue command */
        ret = wc->cmd_queue.cmd_array[wc->cmd_queue.head];
        if (queue_size == 1) {
            wc->cmd_queue.head = wc->cmd_queue.tail = 0;
        } else {
            wc->cmd_queue.head = wc->cmd_queue.head != ACLK_DATABASE_CMD_Q_MAX_SIZE - 1 ?
                                 wc->cmd_queue.head + 1 : 0;
        }
        wc->queue_size = queue_size - 1;

        /* wake up producers */
        uv_cond_signal(&wc->cmd_cond);
    }
    uv_mutex_unlock(&wc->cmd_mutex);

    return ret;
}

int aclk_start_sync_thread(void *data, int argc, char **argv, char **column)
{
    char uuid_str[GUID_LEN + 1];
    UNUSED(data);
    UNUSED(argc);
    UNUSED(column);

    uuid_unparse_lower(*((uuid_t *) argv[0]), uuid_str);

    if (rrdhost_find_by_guid(uuid_str, 0) == localhost)
        return 0;

    info("DEBUG: Start thread for %s", uuid_str);
    sql_create_aclk_table(NULL, (uuid_t *) argv[0], (uuid_t *) argv[1]);
    return 0;
}

void sql_aclk_sync_init(void)
{
    char *err_msg = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode != RRD_MEMORY_MODE_DBENGINE) {
            return;
        }
        error_report("Database has not been initialized");
        return;
    }

    info("SQLite aclk sync initialization");

    for (int i = 0; aclk_sync_config[i]; i++) {
        debug(D_ACLK_SYNC, "Executing %s", aclk_sync_config[i]);
        rc = sqlite3_exec(db_meta, aclk_sync_config[i], 0, 0, &err_msg);
        if (rc != SQLITE_OK) {
            error_report("SQLite error aclk sync initialization setup, rc = %d (%s)", rc, err_msg);
            error_report("SQLite failed statement %s", aclk_sync_config[i]);
            sqlite3_free(err_msg);
            return;
        }
    }
    info("SQLite aclk sync initialization completed");
    fatal_assert(0 == uv_mutex_init(&aclk_async_lock));

    rc = sqlite3_exec(db_meta, "SELECT ni.host_id, ni.node_id FROM host h, node_instance ni WHERE "
        "h.host_id = ni.host_id AND ni.node_id IS NOT NULL;", aclk_start_sync_thread, NULL, NULL);

    return;
}

static void async_cb(uv_async_t *handle)
{
    uv_stop(handle->loop);
    uv_update_time(handle->loop);
    debug(D_ACLK_SYNC, "%s called, active=%d.", __func__, uv_is_active((uv_handle_t *)handle));
}

static void timer_metrics_cb(uv_timer_t* handle)
{
    struct aclk_database_worker_config *wc = handle->data;
    uv_stop(handle->loop);
    uv_update_time(handle->loop);

    info("DEBUG: Metrics callback for %s", wc->uuid_str);
}

#define TIMER_PERIOD_MS (1000)

static void timer_cb(uv_timer_t* handle)
{
    struct aclk_database_worker_config *wc = handle->data;
    uv_stop(handle->loop);
    uv_update_time(handle->loop);

    struct aclk_database_cmd cmd;
    cmd.opcode = ACLK_DATABASE_TIMER;
    cmd.completion = NULL;
    aclk_database_enq_cmd_noblock(wc, &cmd);

    if (wc->cleanup_after && wc->cleanup_after < now_realtime_sec()) {
        cmd.opcode = ACLK_DATABASE_CLEANUP;
        cmd.completion = NULL;
        aclk_database_enq_cmd_noblock(wc, &cmd);

        cmd.opcode = ACLK_DATABASE_UPD_STATS;
        cmd.completion = NULL;
        if (!aclk_database_enq_cmd_noblock(wc, &cmd))
            wc->cleanup_after += ACLK_DATABASE_CLEANUP_INTERVAL;
    }

    if (wc->chart_updates) {
        cmd.opcode = ACLK_DATABASE_PUSH_CHART;
        cmd.count = ACLK_MAX_CHART_BATCH;
        cmd.completion = NULL;
        cmd.param1 = ACLK_MAX_CHART_BATCH_COUNT;
        aclk_database_enq_cmd_noblock(wc, &cmd);
    }

    if (wc->alert_updates) {
        cmd.opcode = ACLK_DATABASE_PUSH_ALERT;
        cmd.count = ACLK_MAX_ALERT_UPDATES;
        cmd.completion = NULL;
        aclk_database_enq_cmd_noblock(wc, &cmd);
    }
}

#define MAX_CMD_BATCH_SIZE (256)

void aclk_database_worker(void *arg)
{
    struct aclk_database_worker_config *wc = arg;
    uv_loop_t *loop;
    int shutdown, ret;
    enum aclk_database_opcode opcode;
    uv_timer_t timer_req;
    //uv_timer_t timer_metrics;
    struct aclk_database_cmd cmd;
    unsigned cmd_batch_size;

    aclk_database_init_cmd_queue(wc);
    //uv_thread_set_name_np(wc->thread, wc->uuid_str);

    char threadname[NETDATA_THREAD_NAME_MAX+1];
    if (wc->host)
        snprintfz(threadname, NETDATA_THREAD_NAME_MAX, "AS_%s", wc->host->hostname);
    else {
        snprintfz(threadname, NETDATA_THREAD_NAME_MAX, "AS_%s", wc->uuid_str);
        threadname[11] = '\0';
    }
    uv_thread_set_name_np(wc->thread, threadname);

    loop = wc->loop = mallocz(sizeof(uv_loop_t));
    ret = uv_loop_init(loop);
    if (ret) {
        error("uv_loop_init(): %s", uv_strerror(ret));
        goto error_after_loop_init;
    }
    loop->data = wc;

    ret = uv_async_init(wc->loop, &wc->async, async_cb);
    if (ret) {
        error("uv_async_init(): %s", uv_strerror(ret));
        goto error_after_async_init;
    }
    wc->async.data = wc;

    ret = uv_timer_init(loop, &timer_req);
    if (ret) {
        error("uv_timer_init(): %s", uv_strerror(ret));
        goto error_after_timer_init;
    }
    timer_req.data = wc;
    fatal_assert(0 == uv_timer_start(&timer_req, timer_cb, TIMER_PERIOD_MS, TIMER_PERIOD_MS));

//    ret = uv_timer_init(loop, &timer_metrics);
//    if (ret) {
//        error("uv_timer_init(): %s", uv_strerror(ret));
//        goto error_after_timer_init;
//    }
//    timer_metrics.data = wc;
//    fatal_assert(0 == uv_timer_start(&timer_metrics, timer_metrics_cb, 10000, 10000));

    wc->error = 0;
    shutdown = 0;

    wc->node_info_send = (wc->host && !localhost);
    aclk_add_worker_thread(wc);

    info("Starting ACLK sync thread for host %s -- scratch area %lu bytes", wc->host_guid, sizeof(*wc));
    sql_get_last_chart_sequence(wc, cmd);
    while (likely(shutdown == 0)) {
        uv_run(loop, UV_RUN_DEFAULT);

        if (netdata_exit)
            shutdown = 1;

        /* wait for commands */
        cmd_batch_size = 0;
        do {
            if (unlikely(cmd_batch_size >= MAX_CMD_BATCH_SIZE))
                break;
            cmd = aclk_database_deq_cmd(wc);
            opcode = cmd.opcode;
            ++cmd_batch_size;
            db_lock();
            switch (opcode) {
                case ACLK_DATABASE_NOOP:
                    /* the command queue was empty, do nothing */
                    break;
                case ACLK_DATABASE_CLEANUP:
                    debug(D_ACLK_SYNC, "Database cleanup for %s", wc->uuid_str);
                    sql_maint_aclk_sync_database(wc, cmd);
                    if (wc->host == localhost)
                        sql_check_aclk_table_list(wc);
                    break;
                case ACLK_DATABASE_CHECK:
                    debug(D_ACLK_SYNC, "Checking database dimensions for %s", wc->uuid_str);
                    sql_check_dimension_state(wc, cmd);
                    break;
                case ACLK_DATABASE_CHECK_ROTATION:
                    debug(D_ACLK_SYNC, "Checking database for rotation %s", wc->uuid_str);
                    sql_check_rotation_state(wc, cmd);
                    break;
                case ACLK_DATABASE_PUSH_CHART:
                    debug(D_ACLK_SYNC, "Pushing chart info to the cloud for node %s", wc->uuid_str);
                    aclk_push_chart_event(wc, cmd);
                    break;
                case ACLK_DATABASE_PUSH_CHART_CONFIG:
                    debug(D_ACLK_SYNC, "Pushing chart config info to the cloud for node %s", wc->uuid_str);
                    aclk_push_chart_config(wc, cmd);
                    break;
                case ACLK_DATABASE_CHART_ACK:
                    debug(D_ACLK_SYNC, "ACK chart SEQ for %s to %"PRIu64, wc->uuid_str, (uint64_t) cmd.param1);
                    sql_set_chart_ack(wc, cmd);
                    break;
                case ACLK_DATABASE_RESET_CHART:
                    debug(D_ACLK_SYNC, "RESET chart SEQ for %s to %"PRIu64, wc->uuid_str, (uint64_t) cmd.param1);
                    sql_reset_chart_event(wc, cmd);
                    break;
                case ACLK_DATABASE_RESET_NODE:
                    debug(D_ACLK_SYNC,"Resetting the node instance id of host %s", (char *) cmd.data);
                    aclk_reset_node_event(wc, cmd);
                    break;
                case ACLK_DATABASE_STATUS_CHART:
                    debug(D_ACLK_SYNC,"Requesting chart status for %s", wc->uuid_str);
                    aclk_status_chart_event(wc, cmd);
                    break;
                case ACLK_DATABASE_ADD_CHART:
                    debug(D_ACLK_SYNC,"Adding chart event for %s", wc->uuid_str);
                    aclk_add_chart_event(wc, cmd);
                    break;
                case ACLK_DATABASE_ADD_ALERT:
                    debug(D_ACLK_SYNC,"Adding alert event for %s", wc->uuid_str);
                    aclk_add_alert_event(wc, cmd);
                    break;
                case ACLK_DATABASE_PUSH_ALERT_CONFIG:
                    debug(D_ACLK_SYNC,"Pushing chart config info to the cloud for node %s", wc->uuid_str);
                    aclk_push_alert_config_event(wc, cmd);
                    break;
                case ACLK_DATABASE_PUSH_ALERT:
                    debug(D_ACLK_SYNC, "Pushing alert info to the cloud for node %s", wc->uuid_str);
                    aclk_push_alert_event(wc, cmd);
                    break;
                case ACLK_DATABASE_ALARM_HEALTH_LOG:
                    debug(D_ACLK_SYNC, "Pushing alarm health log to the cloud for node %s", wc->uuid_str);
                    aclk_push_alarm_health_log(wc, cmd);
                    break;
                case ACLK_DATABASE_ADD_DIMENSION:
                    debug(D_ACLK_SYNC,"Adding dimension event for %s", wc->uuid_str);
                    aclk_add_dimension_event(wc, cmd);
                    break;
                case ACLK_DATABASE_NODE_INFO:
                    debug(D_ACLK_SYNC,"Sending node info for %s", wc->uuid_str);
                    sql_build_node_info(wc, cmd);
                    break;
                case ACLK_DATABASE_DELETE_HOST:     // aclk related tables need to be deleted
                    debug(D_ACLK_SYNC,"Cleaning ACLK tables for %s", (char *) cmd.data);
                    sql_delete_aclk_table_list(wc, cmd);
                    break;
                case ACLK_DATABASE_UPD_STATS:
                    sql_update_metric_statistics(wc, cmd);
                    break;
                case ACLK_DATABASE_TIMER:
                    if (unlikely(localhost && !wc->host)) {
                        //char *agent_id = is_agent_claimed();
                        if (claimed()) {
                            wc->host = rrdhost_find_by_guid(wc->host_guid, 0);
                            if (wc->host) {
                                info("HOST %s detected as active and claimed !!!", wc->host->hostname);
                                //char threadname[NETDATA_THREAD_NAME_MAX+1];
                                snprintfz(threadname, NETDATA_THREAD_NAME_MAX, "AS_%s", wc->host->hostname);
                                uv_thread_set_name_np(wc->thread, threadname);
                                wc->host->dbsync_worker = wc;
                                aclk_del_worker_thread(wc);
                                wc->node_info_send = 1;
//                                if (wc->host->node_id) {
//                                    cmd.opcode = ACLK_DATABASE_NODE_INFO;
//                                    cmd.completion = NULL;
//                                    aclk_database_enq_cmd(wc, &cmd);
//                                }
                                //cmd.opcode = ACLK_DATABASE_UPD_STATS;
                                //cmd.completion = NULL;
                                //aclk_database_enq_cmd(wc, &cmd);
                            }
                            //freez(agent_id);
                        }
                    }
                    if (wc->node_info_send && wc->host && localhost && claimed()) {
                        cmd.opcode = ACLK_DATABASE_NODE_INFO;
                        cmd.completion = NULL;
                        info("DEBUG: Sending node info for %s", wc->host_guid);
                        wc->node_info_send = aclk_database_enq_cmd_noblock(wc, &cmd);
                    }
                    break;
                case ACLK_DATABASE_DEDUP_CHART:
                    debug(D_ACLK_SYNC,"Running chart deduplication for %s", wc->uuid_str);
                    sql_chart_deduplicate(wc, cmd);
                    break;
                case ACLK_DATABASE_SYNC_CHART_SEQ:
                    debug(D_ACLK_SYNC,"Calculatting chart sequence for %s", wc->uuid_str);
                    sql_get_last_chart_sequence(wc, cmd);
                    break;
                case ACLK_DATABASE_SHUTDOWN:
                    shutdown = 1;
                    fatal_assert(0 == uv_timer_stop(&timer_req));
                    uv_close((uv_handle_t *)&timer_req, NULL);

//                    fatal_assert(0 == uv_timer_stop(&timer_metrics));
//                    uv_close((uv_handle_t *)&timer_metrics, NULL);
                    break;
                default:
                    debug(D_ACLK_SYNC, "%s: default.", __func__);
                    break;
            }
            db_unlock();
            if (cmd.completion)
                aclk_complete(cmd.completion);
        } while (opcode != ACLK_DATABASE_NOOP);
    }

    /* cleanup operations of the event loop */
    info("Shutting down ACLK_DATABASE engine event loop.");

    /*
     * uv_async_send after uv_close does not seem to crash in linux at the moment,
     * it is however undocumented behaviour and we need to be aware if this becomes
     * an issue in the future.
     */
    uv_close((uv_handle_t *)&wc->async, NULL);
    uv_run(loop, UV_RUN_DEFAULT);

    info("Shutting down ACLK_DATABASE engine event loop complete.");
    /* TODO: don't let the API block by waiting to enqueue commands */
    uv_cond_destroy(&wc->cmd_cond);
/*  uv_mutex_destroy(&wc->cmd_mutex); */
    //fatal_assert(0 == uv_loop_close(loop));
    int rc;

    do {
        rc = uv_loop_close(loop);
//        info("DEBUG: LOOP returns %d", rc);
    } while (rc != UV_EBUSY);

    freez(loop);

    rrd_wrlock();
    if (likely(wc->host))
        wc->host->dbsync_worker = NULL;
    freez(wc);
    rrd_unlock();
    return;

error_after_timer_init:
    uv_close((uv_handle_t *)&wc->async, NULL);
error_after_async_init:
    fatal_assert(0 == uv_loop_close(loop));
error_after_loop_init:
    freez(loop);

    wc->error = UV_EAGAIN;
}

// -------------------------------------------------------------

void aclk_set_architecture(int mode)
{
    aclk_architecture = mode;
}


//struct interval_duration {
//    uint32_t update_every;
//    uint32_t retention;
//};
//
//struct retention_updated {
//    char *claim_id;
//    char *node_id;
//
//    RRD_MEMORY_MODE memory_mode;
//
//    struct interval_duration *interval_durations;
//    int interval_duration_count;
//
//    struct timeval rotation_timestamp;
//};

#define SELECT_HOST_DIMENSION_LIST  "SELECT d.dim_id, c.update_every, c.type||'.'||c.id FROM chart c, dimension d, host h " \
        "WHERE d.chart_id = c.chart_id AND c.host_id = h.host_id AND c.host_id = @host_id ORDER BY c.update_every ASC;"

#define SELECT_HOST_CHART_LIST  "SELECT distinct h.host_id, c.update_every, c.type||'.'||c.id FROM chart c, host h " \
        "WHERE c.host_id = h.host_id AND c.host_id = @host_id ORDER BY c.update_every ASC;"

void sql_update_metric_statistics(struct aclk_database_worker_config *wc, struct aclk_database_cmd cmd)
{
    UNUSED(cmd);

    int rc;

    if (unlikely(!wc->host))
        return;

    char *claim_id = is_agent_claimed();
    if (unlikely(!claim_id))
        return;

    sqlite3_stmt *res = NULL;

    if (wc->host->rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
        rc = sqlite3_prepare_v2(db_meta, SELECT_HOST_DIMENSION_LIST, -1, &res, 0);
    else
        rc = sqlite3_prepare_v2(db_meta, SELECT_HOST_CHART_LIST, -1, &res, 0);

    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to fetch host dimensions");
        return;
    }

    if (wc->host)
        rc = sqlite3_bind_blob(res, 1, &wc->host->host_uuid , sizeof(wc->host->host_uuid), SQLITE_STATIC);
    else {
        uuid_t host_uuid;
        rc = uuid_parse(wc->host_guid, host_uuid);
        if (unlikely(rc))
            goto failed;
        rc = sqlite3_bind_blob(res, 1, &host_uuid, sizeof(host_uuid), SQLITE_STATIC);
    }
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host parameter to fetch host dimensions");
        goto failed;
    }

    time_t  start_time = LONG_MAX;
    time_t  first_entry_t;
    uint32_t update_every = 0;

    struct retention_updated rotate_data;

    memset(&rotate_data, 0, sizeof(rotate_data));

    int max_intervals = 32;

    rotate_data.interval_duration_count = 0;
    rotate_data.interval_durations = callocz(max_intervals, sizeof(*rotate_data.interval_durations));

    now_realtime_timeval(&rotate_data.rotation_timestamp);
    rotate_data.memory_mode = wc->host->rrd_memory_mode;
    rotate_data.claim_id = claim_id;
    rotate_data.node_id = strdupz(wc->node_id);

    while (sqlite3_step(res) == SQLITE_ROW) {
        if (!update_every || update_every != (uint32_t) sqlite3_column_int(res, 1)) {
            if (update_every) {
                debug(D_ACLK_SYNC,"Update %s for %u oldest time = %ld", wc->host_guid, update_every, start_time);
                rotate_data.interval_durations[rotate_data.interval_duration_count].retention = rotate_data.rotation_timestamp.tv_sec - start_time;
                rotate_data.interval_duration_count++;
            }
            update_every = (uint32_t) sqlite3_column_int(res, 1);
            rotate_data.interval_durations[rotate_data.interval_duration_count].update_every = update_every;
            start_time = LONG_MAX;
        }
#ifdef ENABLE_DBENGINE
        time_t  last_entry_t;
        if (wc->host->rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            rc = rrdeng_metric_latest_time_by_uuid((uuid_t *)sqlite3_column_blob(res, 0), &first_entry_t, &last_entry_t);
        else
#endif
        {
            RRDSET *st = NULL;
            rc = (st = rrdset_find(wc->host, (const char *)sqlite3_column_text(res, 2))) ? 0 : 1;
            if (!rc) {
                first_entry_t = rrdset_first_entry_t(st);
//                info("DEBUG: Scanning SET = %s --> %ld", st->name, first_entry_t);
            }
        }

        if (likely(!rc && first_entry_t))
            start_time = MIN(start_time, first_entry_t);
    }
    if (update_every) {
        debug(D_ACLK_SYNC, "Update %s for %u oldest time = %ld", wc->host_guid, update_every, start_time);
        rotate_data.interval_durations[rotate_data.interval_duration_count].retention = rotate_data.rotation_timestamp.tv_sec - start_time;
        rotate_data.interval_duration_count++;
    }

    info("DEBUG: Scan update every for host");
    for (int i = 0; i < rotate_data.interval_duration_count; ++i) {
        info("DEBUG:  %d --> Update %s for %u  Retention = %u", i, wc->host_guid,
             rotate_data.interval_durations[i].update_every, rotate_data.interval_durations[i].retention);
    };
    aclk_retention_updated(&rotate_data);
    freez(rotate_data.node_id);
    freez(rotate_data.claim_id);
    freez(rotate_data.interval_durations);

failed:
    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when reading host dimensions");
    return;
}


void sql_create_aclk_table(RRDHOST *host, uuid_t *host_uuid, uuid_t *node_id)
{
    char uuid_str[GUID_LEN + 1];
    char host_guid[GUID_LEN + 1];

    uuid_unparse_lower(*host_uuid, host_guid);
    uuid_unparse_lower_fix(host_uuid, uuid_str);

    /// check if we have it in the pool
    if (aclk_worker_thread_exists(uuid_str)) {
        //info("DEBUG: %s exists in the pool, not creating", uuid_str);
        return;
    }

    BUFFER *sql = buffer_create(1024);

    buffer_sprintf(sql, TABLE_ACLK_CHART, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, TABLE_ACLK_CHART_PAYLOAD, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, TABLE_ACLK_CHART_LATEST, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, INDEX_ACLK_CHART, uuid_str, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, INDEX_ACLK_CHART_LATEST, uuid_str, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, TRIGGER_ACLK_CHART_PAYLOAD, uuid_str, uuid_str, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, TABLE_ACLK_ALERT, uuid_str, uuid_str, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql, INDEX_ACLK_ALERT, uuid_str, uuid_str);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);


    buffer_free(sql);
    // Spawn db thread for processing (event loop)
    if (likely(host) && unlikely(host->dbsync_worker))
        return;

    struct aclk_database_worker_config *wc = callocz(1, sizeof(struct aclk_database_worker_config));
    if (likely(host))
        host->dbsync_worker = (void *) wc;
    wc->host = host;
    wc->chart_updates = 0;
    wc->alert_updates = 0;
    wc->startup_time = now_realtime_sec();
    wc->cleanup_after = wc->startup_time + ACLK_DATABASE_CLEANUP_FIRST;
    strcpy(wc->uuid_str, uuid_str);
    strcpy(wc->host_guid, host_guid);
    if (node_id && !uuid_is_null(*node_id))
        uuid_unparse_lower(*node_id, wc->node_id);
    fatal_assert(0 == uv_thread_create(&(wc->thread), aclk_database_worker, wc));
}

void sql_maint_aclk_sync_database(struct aclk_database_worker_config *wc, struct aclk_database_cmd cmd)
{
    UNUSED(cmd);
//    static time_t  last_database_check = 0;

//    time_t  now = now_realtime_sec();
//
//    if (unlikely(!last_database_check))
//        goto done;
//
//    if (now - last_database_check < 120)
//        return;

    info("DEBUG: Checking database for %s", wc->uuid_str);

    //last_database_check = now;

    BUFFER *sql = buffer_create(1024);

    buffer_sprintf(sql,"DELETE FROM aclk_chart_%s WHERE date_submitted IS NOT NULL AND date_updated < strftime('%%s','now','-%d seconds');", wc->uuid_str, ACLK_DELETE_ACK_INTERNAL);
    db_execute(buffer_tostring(sql));
    buffer_flush(sql);

    buffer_sprintf(sql,"DELETE FROM aclk_chart_payload_%s WHERE unique_id NOT IN "
        "(SELECT unique_id FROM aclk_chart_%s) AND unique_id NOT IN (SELECT unique_id FROM aclk_chart_latest_%s);",
          wc->uuid_str,  wc->uuid_str, wc->uuid_str);
    db_execute(buffer_tostring(sql));

    buffer_free(sql);
    return;
}

#define SQL_SELECT_HOST_BY_UUID  "SELECT host_id FROM host WHERE host_id = @host_id;"

static int is_host_available(uuid_t *host_id)
{
    sqlite3_stmt *res = NULL;
    int rc;

    if (unlikely(!db_meta)) {
        if (default_rrd_memory_mode == RRD_MEMORY_MODE_DBENGINE)
            error_report("Database has not been initialized");
        return 1;
    }

    rc = sqlite3_prepare_v2(db_meta, SQL_SELECT_HOST_BY_UUID, -1, &res, 0);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to prepare statement to select node instance information for a node");
        return 1;
    }

    rc = sqlite3_bind_blob(res, 1, host_id, sizeof(*host_id), SQLITE_STATIC);
    if (unlikely(rc != SQLITE_OK)) {
        error_report("Failed to bind host_id parameter to select node instance information");
        goto failed;
    }
    rc = sqlite3_step(res);

    failed:
    if (unlikely(sqlite3_finalize(res) != SQLITE_OK))
        error_report("Failed to finalize the prepared statement when checking host existence");

    return (rc == SQLITE_ROW);
}

//void sql_drop_host_aclk_table_list(uuid_t *host_uuid)
//{
//    int rc;
//    char uuid_str[GUID_LEN + 1];
//
//    uuid_unparse_lower_fix(host_uuid, uuid_str);
//
//    BUFFER *sql = buffer_create(1024);
//    buffer_sprintf(
//        sql,"SELECT 'drop '||type||' IF EXISTS '||name||';' FROM sqlite_schema " \
//        "WHERE name LIKE 'aclk_%%_%s' AND type IN ('table', 'trigger', 'index');", uuid_str);
//
//    sqlite3_stmt *res = NULL;
//
//    rc = sqlite3_prepare_v2(db_meta, buffer_tostring(sql), -1, &res, 0);
//    if (rc != SQLITE_OK) {
//        error_report("Failed to prepare statement to clean up aclk tables");
//        goto fail;
//    }
//    buffer_flush(sql);
//
//    while (sqlite3_step(res) == SQLITE_ROW)
//        buffer_strcat(sql, (char *) sqlite3_column_text(res, 0));
//
//    rc = sqlite3_finalize(res);
//    if (unlikely(rc != SQLITE_OK))
//        error_report("Failed to finalize statement to clean up aclk tables, rc = %d", rc);
//
//    db_execute(buffer_tostring(sql));
//
//    fail:
//    buffer_free(sql);
//}

void sql_delete_aclk_table_list(struct aclk_database_worker_config *wc, struct aclk_database_cmd cmd)
{
    UNUSED(wc);

    int rc;

    uuid_t host_id;

    char *host_guid = (char *)cmd.data;

    info("DEBUG: Checking if I should delete aclk tables for node %s", host_guid);
    rc = uuid_parse(host_guid, host_id);
    if (rc)
        goto done;

    if (is_host_available(&host_id)) {
        info("DEBUG: Host %s exists, cannot delete aclk sync tables", host_guid);
        goto done;
    }

    info("DEBUG: Host %s does NOT exist, can delete aclk sync tables", host_guid);
//    sql_drop_host_aclk_table_list(&host_id);

//    int rc;
//    char uuid_str[GUID_LEN + 1];

    host_guid[8] = '_';
    host_guid[13] = '_';
    host_guid[18] = '_';
    host_guid[23] = '_';
//    uuid_unparse_lower_fix(host_uuid, uuid_str);

    sqlite3_stmt *res = NULL;
    BUFFER *sql = buffer_create(1024);

    buffer_sprintf(sql,"SELECT 'drop '||type||' IF EXISTS '||name||';' FROM sqlite_schema " \
        "WHERE name LIKE 'aclk_%%_%s' AND type IN ('table', 'trigger', 'index');", host_guid);

    rc = sqlite3_prepare_v2(db_meta, buffer_tostring(sql), -1, &res, 0);
    if (rc != SQLITE_OK) {
        error_report("Failed to prepare statement to clean up aclk tables");
        goto fail;
    }
    buffer_flush(sql);

    while (sqlite3_step(res) == SQLITE_ROW)
        buffer_strcat(sql, (char *) sqlite3_column_text(res, 0));

    rc = sqlite3_finalize(res);
    if (unlikely(rc != SQLITE_OK))
        error_report("Failed to finalize statement to clean up aclk tables, rc = %d", rc);

    db_execute(buffer_tostring(sql));

fail:
    buffer_free(sql);

done:
    freez(host_guid);
    return;
}

static int sql_check_aclk_table(void *data, int argc, char **argv, char **column)
{
    struct aclk_database_worker_config *wc = data;
    UNUSED(argc);
    UNUSED(column);

    info("DEBUG: Scheduling aclk sync table check for node %s", (char *) argv[0]);
    struct aclk_database_cmd cmd;
    cmd.opcode = ACLK_DATABASE_DELETE_HOST;
    cmd.completion = NULL;
    cmd.data = strdupz((char *) argv[0]);
    aclk_database_enq_cmd_noblock(wc, &cmd);
    return 0;
}

#define SQL_SELECT_ACLK_ACTIVE_LIST "SELECT REPLACE(SUBSTR(name,19),'_','-') FROM sqlite_schema " \
        "WHERE name LIKE 'aclk_chart_latest_%' AND type IN ('table');"

void sql_check_aclk_table_list(struct aclk_database_worker_config *wc)
{
    info("DEBUG: Cleaning tables for nodes that do not exist");
    (int) sqlite3_exec(db_meta, SQL_SELECT_ACLK_ACTIVE_LIST, sql_check_aclk_table, (void *) wc, NULL);
    return;
}


//void sql_aclk_drop_all_table_list()
//{
//    int rc;
//
//    BUFFER *sql = buffer_create(1024);
//    buffer_strcat(sql, "SELECT host_id FROM host;");
//    sqlite3_stmt *res = NULL;
//
//    rc = sqlite3_prepare_v2(db_meta, buffer_tostring(sql), -1, &res, 0);
//    if (rc != SQLITE_OK) {
//        error_report("Failed to prepare statement to clean up aclk tables");
//        goto fail;
//    }
//    while (sqlite3_step(res) == SQLITE_ROW) {
//        sql_drop_host_aclk_table_list((uuid_t *)sqlite3_column_blob(res, 0));
//    }
//
//    rc = sqlite3_finalize(res);
//    if (unlikely(rc != SQLITE_OK))
//        error_report("Failed to finalize statement to clean up aclk tables, rc = %d", rc);
//
//    fail:
//    buffer_free(sql);
//    return;
//}
