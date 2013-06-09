#include <uwsgi.h>

extern struct uwsgi_server uwsgi;

/*

	Chunked input implementation

	--chunked-input-limit <bytes> (default 1MB)

	--chunked-input-timeout <s> (default --socket-timeout)

	chunk = uwsgi.chunked_read([timeout])

	timeout = -1 (wait forever)
	timeout = 0 (default)

*/

static ssize_t uwsgi_chunked_input_recv(struct wsgi_request *wsgi_req, int timeout, int nb) {

	if (timeout == 0) timeout = uwsgi.chunked_input_timeout;
	if (timeout == 0) timeout = uwsgi.shared->options[UWSGI_OPTION_SOCKET_TIMEOUT];

	int ret = -1;

	for(;;) {
		ssize_t rlen = wsgi_req->socket->proto_read_body(wsgi_req, wsgi_req->chunked_input_buf->buf + wsgi_req->chunked_input_buf->pos, wsgi_req->chunked_input_buf->len - wsgi_req->chunked_input_buf->pos);
		if (rlen > 0) return rlen;
		if (rlen == 0) return -1;
		if (rlen < 0) {
                        if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINPROGRESS) {
				if (nb) return -1; 
                                goto wait;
                        }
                        uwsgi_error("uwsgi_chunked_input_recv()");
                        return -1;
                }

wait:
                ret = uwsgi.wait_read_hook(wsgi_req->fd, timeout);
                if (ret > 0) {
			rlen = wsgi_req->socket->proto_read_body(wsgi_req, wsgi_req->chunked_input_buf->buf + wsgi_req->chunked_input_buf->pos, wsgi_req->chunked_input_buf->len - wsgi_req->chunked_input_buf->pos);
			if (rlen > 0) return rlen;
			if (rlen <= 0) return -1;
		}
                if (ret < 0) {
			uwsgi_error("uwsgi_chunked_input_recv()");
                }
		return -1;
	}

        return -1;
}

static ssize_t uwsgi_chunked_readline(struct wsgi_request *wsgi_req) {
	size_t i;
	int found = 0;
	for(i=0;i<wsgi_req->chunked_input_buf->pos;i++) {
		if (found) {
			if (wsgi_req->chunked_input_buf->buf[i] == '\n') {
				if (uwsgi_buffer_decapitate(wsgi_req->chunked_input_buf, i+1)) return -1;
				// strtoul will stop at \r
				return strtoul(wsgi_req->chunked_input_buf->buf, NULL, 16);
			}
			return -1;
		}
		if ((wsgi_req->chunked_input_buf->buf[i] >= '0' && wsgi_req->chunked_input_buf->buf[i] <= '9') || 
			(wsgi_req->chunked_input_buf->buf[i] >= 'a' && wsgi_req->chunked_input_buf->buf[i] <= 'z') ||
			(wsgi_req->chunked_input_buf->buf[i] >= 'A' && wsgi_req->chunked_input_buf->buf[i] <= 'Z')) continue;
		if (wsgi_req->chunked_input_buf->buf[i] == '\r') { found = 1; continue; }
		return -1;
	}

	return -2;
}

/*

	0 -> waiting for \r\n
	1 -> waiting for whole body

*/

char *uwsgi_chunked_read(struct wsgi_request *wsgi_req, size_t *len, int timeout, int nb) {
	char *ret;
	ssize_t chunk_len = 0;
	if (!wsgi_req->chunked_input_buf) {
		wsgi_req->chunked_input_buf = uwsgi_buffer_new(uwsgi.page_size);
		wsgi_req->chunked_input_buf->limit = uwsgi.chunked_input_limit;
		wsgi_req->chunked_input_want = 1;
	}

	// the whole chunk stream has been consumed
	if (wsgi_req->chunked_input_complete) {
		*len = 0;
		return wsgi_req->chunked_input_buf->buf;
	}

	for(;;) {
		if (wsgi_req->chunked_input_want || wsgi_req->chunked_input_buf->pos == 0) {
			if (uwsgi_buffer_fix(wsgi_req->chunked_input_buf, uwsgi.page_size)) return NULL;
			ssize_t rlen = uwsgi_chunked_input_recv(wsgi_req, timeout, nb);	
			if (rlen <= 0) return NULL;
			// update buffer position
			wsgi_req->chunked_input_buf->pos += rlen;
			wsgi_req->chunked_input_want = 0;

			if (wsgi_req->chunked_input_need > 0) {
				if ((size_t)rlen > wsgi_req->chunked_input_need) {
					wsgi_req->chunked_input_need = 0;
				}
				else {
					wsgi_req->chunked_input_need -= rlen;
				}
				if (wsgi_req->chunked_input_need > 0) wsgi_req->chunked_input_want = 1;
			}
		}

		if (wsgi_req->chunked_input_want) continue;

		// ok we have a frame, let's parse it
		if (wsgi_req->chunked_input_buf->pos > 0) {
			switch(wsgi_req->chunked_input_parser_status) {
				case 0:
					chunk_len = uwsgi_chunked_readline(wsgi_req);
					if (chunk_len == -2) {
						wsgi_req->chunked_input_want = 1;
						break;
					}
					else if (chunk_len < 0) {
						return NULL;
					}
					else if (chunk_len == 0) {
						*len = 0;
						wsgi_req->chunked_input_complete = 1;
						return wsgi_req->chunked_input_buf->buf;
					}
					// if here the buffer has been already decapitated
					if ((size_t)(chunk_len+2) > wsgi_req->chunked_input_buf->pos) {
						wsgi_req->chunked_input_need = (chunk_len+2) - wsgi_req->chunked_input_buf->pos;
						wsgi_req->chunked_input_parser_status = 1;
						wsgi_req->chunked_input_want = 1;
						break;
					}
					*len = chunk_len;
					ret = wsgi_req->chunked_input_buf->buf;
					if (uwsgi_buffer_decapitate(wsgi_req->chunked_input_buf, chunk_len+2)) return NULL;
					return ret;
				case 1:
					if ((size_t)(chunk_len+2) > wsgi_req->chunked_input_buf->pos) {
                                                wsgi_req->chunked_input_need = (chunk_len+2) - wsgi_req->chunked_input_buf->pos;
						wsgi_req->chunked_input_want = 1;
						break;
                                        }	
					*len = chunk_len;
                                        ret = wsgi_req->chunked_input_buf->buf;
                                        if (uwsgi_buffer_decapitate(wsgi_req->chunked_input_buf, chunk_len+2)) return NULL;
					wsgi_req->chunked_input_parser_status = 0;
                                        return ret;
					
			}
		}
	}
}
