#include "tmux.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define TTY_QUERY_TIMEOUT 5000
#define TTY_QUERY_LIMIT 4096
#define TTY_QUERY_MAX 64

enum tty_query_kind {
	TTY_QUERY_NONE,
	TTY_QUERY_MODE,
	TTY_QUERY_PRIVATE_MODE,
	TTY_QUERY_DA,
	TTY_QUERY_DA2,
	TTY_QUERY_VERSION,
	TTY_QUERY_CURSOR,
	TTY_QUERY_KEYBOARD,
	TTY_QUERY_GRAPHICS,
	TTY_QUERY_SIZE,
	TTY_QUERY_COLOUR,
	TTY_QUERY_PALETTE,
	TTY_QUERY_CLIPBOARD,
	TTY_QUERY_THEME
};

enum tty_query_owner {
	TTY_QUERY_TMUX,
	TTY_QUERY_PANE,
	TTY_QUERY_SIDE
};

struct tty_query {
	enum tty_query_kind	 kind;
	enum tty_query_owner	 owner;
	u_int			 id;
	uint64_t		 target;
	uint64_t		 time;
	TAILQ_ENTRY(tty_query)	 entry;
};

struct tty_queries {
	TAILQ_HEAD(, tty_query)	 list;
	char			 pending[TTY_QUERY_LIMIT + 1];
	size_t			 length;
	enum tty_query_owner	 owner;
	uint64_t		 target;
};

static int
tty_query_step(const char *buf, size_t len)
{
	char ch = buf[len - 1];

	if (buf[0] != '\033')
		return (-1);
	if (len == 1)
		return (1);
	if (buf[1] == '[') {
		if (len == 2)
			return (1);
		if (ch >= 0x40 && ch <= 0x7e)
			return (0);
		if (ch < 0x20 || ch > 0x3f)
			return (-1);
	} else if (buf[1] == '_' || buf[1] == 'P' || buf[1] == ']') {
		if (len > 2 && ((buf[1] == ']' && ch == '\007') ||
		    (ch == '\\' && buf[len - 2] == '\033')))
			return (0);
	} else
		return (0);
	return (1);
}

static int
tty_query_frame(const char *buf, size_t len, size_t *size)
{
	size_t i;
	int n;

	for (i = 1; i <= len; i++) {
		n = tty_query_step(buf, i);
		if (n != 1) {
			if (n == 0)
				*size = i;
			return (n);
		}
	}
	return (1);
}

static enum tty_query_kind
tty_query_parse(const char *buf, size_t len, int reply, u_int *id)
{
	char		 tmp[TTY_QUERY_LIMIT + 1], *body, *end, *p, *next;
	u_int		 value, second;
	int		 used = 0, action = 0;

	if (len > TTY_QUERY_LIMIT) {
		if (buf[1] != ']' && buf[1] != '_' && buf[1] != 'P')
			return (TTY_QUERY_NONE);
		len = TTY_QUERY_LIMIT;
	}
	memcpy(tmp, buf, len);
	tmp[len] = '\0';
	body = tmp + 2;
	*id = 0;
	if (tmp[1] == '[') {
		p = body;
		if (*p == '?')
			p++;
		if ((!reply && sscanf(p, "%u$p%n", &value, &used) == 1) ||
		    (reply && sscanf(p, "%u;%u$y%n", &value, &second, &used) == 2)) {
			if (used != 0 && p[used] == '\0') {
				*id = value;
				return (*body == '?' ? TTY_QUERY_PRIVATE_MODE :
				    TTY_QUERY_MODE);
			}
		}
		end = tmp + len - 1;
		if (reply && (*body == '?' || *body == '>') &&
		    isdigit((u_char)body[1]) &&
		    strspn(body + 1, "0123456789;") == len - 4) {
			if (*end == 'c')
				return (*body == '?' ? TTY_QUERY_DA : TTY_QUERY_DA2);
			if (*body == '?' && *end == 'u')
				return (TTY_QUERY_KEYBOARD);
		}
		if (!reply && (strcmp(body, "c") == 0 || strcmp(body, "0c") == 0))
			return (TTY_QUERY_DA);
		if (!reply && (strcmp(body, ">c") == 0 || strcmp(body, ">0c") == 0))
			return (TTY_QUERY_DA2);
		if (!reply && strcmp(body, ">q") == 0)
			return (TTY_QUERY_VERSION);
		if ((!reply && strcmp(body, "6n") == 0) ||
		    (reply && sscanf(body, "%u;%uR%n", &value, &second, &used) == 2 &&
		    used != 0 && body[used] == '\0'))
			return (TTY_QUERY_CURSOR);
		if (!reply && strcmp(body, "?u") == 0)
			return (TTY_QUERY_KEYBOARD);
		used = 0;
		if (sscanf(body, "%ut%n", &value, &used) == 1 && used != 0 &&
		    body[used] == '\0' && !reply) {
			if (value == 14 || value == 16 || value == 18) {
				*id = value - 10;
				return (TTY_QUERY_SIZE);
			}
		}
		if (reply && sscanf(body, "%u;%u;%ut%n", &value, &second,
		    id, &used) == 3 &&
		    used != 0 && body[used] == '\0') {
			*id = value;
			return (TTY_QUERY_SIZE);
		}
		if ((!reply && strcmp(body, "?996n") == 0) ||
		    (reply && strncmp(body, "?997;", 5) == 0 && *end == 'n'))
			return (TTY_QUERY_THEME);
	} else if (tmp[1] == 'P' && reply && strncmp(body, ">|", 2) == 0)
		return (TTY_QUERY_VERSION);
	else if (tmp[1] == '_' && *body == 'G') {
		end = strchr(++body, ';');
		if (end == NULL)
			return (TTY_QUERY_NONE);
		*end = '\0';
		while ((p = strsep(&body, ",")) != NULL) {
			if (strcmp(p, "a=q") == 0)
				action = 1;
			if (strncmp(p, "i=", 2) == 0) {
				value = strtoul(p + 2, &next, 10);
				if (next != p + 2 && *next == '\0')
					*id = value;
			}
		}
		if (reply || action)
			return (TTY_QUERY_GRAPHICS);
	} else if (tmp[1] == ']') {
		end = tmp + len;
		if (!reply)
			end -= (tmp[len - 1] == '\007' ? 1 : 2);
		*end = '\0';
		if (sscanf(body, "%u;%n", &value, &used) != 1 || used == 0)
			return (TTY_QUERY_NONE);
		body += used;
		if ((value == 10 || value == 11) && (reply || strcmp(body, "?") == 0)) {
			*id = value;
			return (TTY_QUERY_COLOUR);
		}
		if (value == 4 && sscanf(body, "%u;%n", id, &used) == 1 &&
		    used != 0 && (reply || strcmp(body + used, "?") == 0))
			return (TTY_QUERY_PALETTE);
		if (value == 52 && (p = strchr(body, ';')) != NULL &&
		    (reply || strcmp(p + 1, "?") == 0))
			return (TTY_QUERY_CLIPBOARD);
	}
	return (TTY_QUERY_NONE);
}

void
tty_query_puts(struct tty *tty, const char *s)
{
	tty_query_add(tty, s, strlen(s), NULL);
	tty_puts(tty, s);
}

void
tty_query_add(struct tty *tty, const char *buf, size_t len,
    const struct tty_ctx *ctx)
{
	struct tty_queries	*qs = tty->queries;
	struct tty_query	*q;
	const char		*next;
	enum tty_query_kind	 kind;
	size_t			 i;
	u_int			 id, count;
	int			 n;

	if (tty->flags & TTY_BLOCK)
		return;
	if (qs == NULL) {
		qs = tty->queries = xcalloc(1, sizeof *qs);
		TAILQ_INIT(&qs->list);
	}
	for (i = 0; i < len; i++) {
		if (qs->length == 0) {
			next = memchr(buf + i, '\033', len - i);
			if (next == NULL)
				break;
			i = next - buf;
			qs->owner = TTY_QUERY_TMUX;
			qs->target = 0;
			if (ctx != NULL && ctx->wp != NULL) {
				qs->owner = TTY_QUERY_PANE;
				qs->target = ctx->wp->id;
			} else if (ctx != NULL &&
			    ctx->s == &tty->client->side_status.jobscreen) {
				qs->owner = TTY_QUERY_SIDE;
				qs->target = tty->client->side_status.generation;
			}
		}
		qs->pending[qs->length++] = buf[i];
		n = tty_query_step(qs->pending, qs->length);
		if (n == 1 && qs->length < TTY_QUERY_LIMIT)
			continue;
		if (n == 0) {
			kind = tty_query_parse(qs->pending, qs->length, 0, &id);
			if (kind != TTY_QUERY_NONE) {
				count = 0;
				TAILQ_FOREACH(q, &qs->list, entry)
					count++;
				if (count == TTY_QUERY_MAX) {
					q = TAILQ_FIRST(&qs->list);
					TAILQ_REMOVE(&qs->list, q, entry);
					free(q);
				}
				q = xcalloc(1, sizeof *q);
				q->kind = kind;
				q->id = id;
				q->owner = qs->owner;
				q->target = qs->target;
				q->time = get_timer();
				log_debug("%s: query %d/%u owner %d/%llu",
				    tty->client->name, q->kind, q->id, q->owner,
				    (unsigned long long)q->target);
				TAILQ_INSERT_TAIL(&qs->list, q, entry);
			}
		}
		qs->length = 0;
	}
}

int
tty_query_reply(struct tty *tty, const char *buf, size_t len, size_t *size)
{
	struct tty_queries	*qs = tty->queries;
	struct tty_query	*q;
	struct side_status_line	*ss = &tty->client->side_status;
	struct window_pane	*wp;
	enum tty_query_kind	 kind;
	u_int			 id;
	int			 n, consumed = 1;

	if (qs == NULL || TAILQ_EMPTY(&qs->list))
		return (-1);
	n = tty_query_frame(buf, len, size);
	if (n != 0)
		return (n);
	if (buf[1] == '[') {
		if (strchr("ycRutn", buf[*size - 1]) == NULL)
			return (-1);
	} else if (buf[1] != 'P' && buf[1] != '_' && buf[1] != ']')
		return (-1);
	kind = tty_query_parse(buf, *size, 1, &id);
	if (kind == TTY_QUERY_NONE)
		return (-1);
	TAILQ_FOREACH(q, &qs->list, entry) {
		if (q->kind == kind && q->id == id)
			break;
	}
	if (q == NULL)
		return (-1);
	log_debug("%s: reply %d/%u owner %d/%llu", tty->client->name,
	    q->kind, q->id, q->owner, (unsigned long long)q->target);
	if (get_timer() - q->time < TTY_QUERY_TIMEOUT) {
		switch (q->owner) {
		case TTY_QUERY_TMUX:
			consumed = 0;
			break;
		case TTY_QUERY_PANE:
			wp = window_pane_find_by_id(q->target);
			if (wp != NULL && wp->fd != -1)
				bufferevent_write(wp->event, buf, *size);
			break;
		case TTY_QUERY_SIDE:
			if (ss->job != NULL && q->target == ss->generation)
				bufferevent_write(job_get_event(ss->job), buf, *size);
			break;
		}
	}
	TAILQ_REMOVE(&qs->list, q, entry);
	free(q);
	return (consumed ? 0 : -1);
}

void
tty_query_free(struct tty *tty)
{
	struct tty_queries	*qs = tty->queries;
	struct tty_query	*q;

	if (qs == NULL)
		return;
	while ((q = TAILQ_FIRST(&qs->list)) != NULL) {
		TAILQ_REMOVE(&qs->list, q, entry);
		free(q);
	}
	free(qs);
	tty->queries = NULL;
}
