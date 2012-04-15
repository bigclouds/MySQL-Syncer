
#include <rs_config.h>
#include <rs_core.h>
#include <rs_slave.h>



static int  rs_create_dump_cmd(rs_slave_info_t *s, char *buf, uint32_t *len);
static void rs_free_io_thread(void *data);


/*
 *   rs_register_slave
 *   @s struct rs_slave_info_t
 *
 *   Connect to master, register a slave
 *
 *   No return value
 */
void *rs_start_io_thread(void *data) 
{
    int                     r, sock_err, i;
    socklen_t               len;
    uint32_t                l, pack_len, cur_len;
    ssize_t                 n;
    char                    cbuf[PATH_MAX + 1 + UINT32_LEN];
    struct                  sockaddr_in svr_addr;
    rs_slave_info_t         *si;
    rs_ring_buffer_data_t   *d;

    /* init var */
    i = 0;
    rs_memzero(&svr_addr, sizeof(svr_addr));
    len = sizeof(int);
    si = (rs_slave_info_t *) data;

    /* push cleanup handle */
    pthread_cleanup_push(rs_free_io_thread, si);

    if(si == NULL) {
        rs_log_err(0, "io thread can not get slave info struct"); 
        goto free;
    }

    /* connect to master */
    svr_addr.sin_family = AF_INET;

    if(si->listen_port <= 0) {
        rs_log_err(0, "listen_port must greate than zero"); 
        goto free;
    }

    svr_addr.sin_port = htons(si->listen_port);

    if(si->listen_addr == NULL) {
        rs_log_err(0, "listen_addr must not be null"); 
        goto free;
    }

    if (inet_pton(AF_INET, si->listen_addr, &(svr_addr.sin_addr)) != 1) {
        rs_log_err(rs_errno, "inet_pton(\"%s\") failed", si->listen_addr); 
        goto free;
    }

    for( ;; ) {

        si->svr_fd = socket(AF_INET, SOCK_STREAM, 0);

        if(si->svr_fd == -1) {
            rs_log_err(rs_errno, "socket() failed, svr_fd");
            goto free;
        }

        if(connect(si->svr_fd, (const struct sockaddr *) &svr_addr, 
                    sizeof(svr_addr)) == -1) {
            if(i % 60 == 0) {
                i = 0;
                rs_log_err(rs_errno, "connect() failed, addr = %s:%d", 
                        si->listen_addr, si->listen_port);
            }

            i += RS_RETRY_CONNECT_SLEEP_SEC;

            goto retry;
        }

        if(rs_create_dump_cmd(si, cbuf, &l) != RS_OK) {
            goto free;
        }

        /* send command */
        rs_log_info("send cmd = %s, dump_file = %s, dump_pos = %u", 
                cbuf, si->dump_file, si->dump_pos);

        n = rs_write(si->svr_fd, cbuf, l); 

        if(n != (ssize_t) l) {
            goto retry;
        }

        /* add to ring buffer */
        for( ;; ) {

            r = rs_set_ring_buffer(si->ring_buf, &d);

            if(r == RS_FULL) {
                if(i % 60 == 0) {
                    i = 0;
                    rs_log_info("io thread wait ring buffer consumed");
                }

                i += RS_RING_BUFFER_FULL_SLEEP_SEC;

                if(getsockopt(si->svr_fd, SOL_SOCKET, SO_ERROR, 
                            (void *) &sock_err, &len) == -1)
                {
                    sock_err = rs_errno;
                }

                if(sock_err) {
                    rs_log_info("server fd is closed");
                    goto retry;
                }

                sleep(RS_RING_BUFFER_FULL_SLEEP_SEC);
            }

            if(r == RS_ERR) {
                goto free;
            }

            if(r == RS_OK) {

                cur_len = 0;

                /* get packet length */ 
                n = rs_read(si->svr_fd, &pack_len, 4);

                if(n < 0) {
                    rs_log_debug(0, "rs_read() failed, pack_len");
                    goto retry;
                }

                if(n == 0) {
                    rs_log_info("master shutdown");
                    goto retry;
                }

                rs_log_info("get cmd packet length = %u", pack_len);

                /* while get a full packet */
                while(cur_len != pack_len) {

                    n = rs_read(si->svr_fd, d->data + cur_len, 
                            pack_len - cur_len);

                    if(n < 0) {
                        rs_log_debug(0, "rs_read() failed, pack_data");
                        goto retry;
                    }

                    if(n == 0) {
                        rs_log_info("master shutdown");
                        goto retry;
                    }

                    cur_len += (uint32_t) n;
                }

                d->len = pack_len;

                rs_set_ring_buffer_advance(si->ring_buf);

                continue;
            }

        } // EXIT RING BUFFER 

retry:

        /* close svr_fd retry connect */
        (void) rs_close(si->svr_fd);

        si->svr_fd = -1;

        sleep(RS_RETRY_CONNECT_SLEEP_SEC);
    }


free:

    pthread_cleanup_pop(1);

    return NULL;
}

/*
 *
 *  rs_create_dump_cmd
 *  @s struct rs_slave_infot
 *  @len store the length of command
 *
 *  Create request dump cmd
 *
 *  On success, RS_OK is returned. On error, RS_ERR is returned
 */
static int rs_create_dump_cmd(rs_slave_info_t *si, char *buf, uint32_t *len) 
{
    int l;

    l = snprintf(buf, RS_REGISTER_SLAVE_CMD_LEN, "%s,%u", si->dump_file, 
            si->dump_pos);

    if(l < 0) {
        rs_log_err(rs_errno, "snprintf() failed, dump_cmd"); 
        return RS_ERR;
    }

    *len = (uint32_t) l;

    return RS_OK;
}

static void rs_free_io_thread(void *data)
{
    rs_slave_info_t *si;

    si = (rs_slave_info_t *) data;

    if(si != NULL) {
        kill(rs_pid, SIGQUIT);
    }
}