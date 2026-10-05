#include "tmux.h"

#include <stdlib.h>
#include <string.h>

#define TTY_QUERY_EXPIRY 5000
#define TTY_QUERY_LIMIT 4096
#define TTY_QUERY_MAX 64

struct tty_query_key {
	char intro, prefix, interm[3], final;
	char echo[64];
};

enum tty_query_owner {
	TTY_QUERY_TMUX,
	TTY_QUERY_PANE,
	TTY_QUERY_SIDE
};

struct tty_query {
	struct tty_query_key	 key;
	enum tty_query_owner	 owner;
	uint64_t		 target;
	uint64_t		 time;
	TAILQ_ENTRY(tty_query)	 entry;
};

struct tty_queries {
	TAILQ_HEAD(, tty_query)	 list;
	char			 pending[TTY_QUERY_LIMIT + 1];
	size_t			 length;
	char			 last;
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

enum tty_query_echo { QUERY_NONE, QUERY_SAME, QUERY_MINUS10, QUERY_PLUS2 };

static const struct {
	char prefix;
	const char *interm;
	char final;
	int param;
	char intro, reply_prefix;
	const char *reply_interm;
	char reply_final;
	enum tty_query_echo echo;
} tty_query_table[] = {
	{ 0, "", 'c', 0, '[', '?', "", 'c', QUERY_NONE },
	{ '>', "", 'c', 0, '[', '>', "", 'c', QUERY_NONE },
	{ '=', "", 'c', 0, 'P', 0, "!", '|', QUERY_NONE },
	{ '>', "", 'q', -1, 'P', 0, "", '|', QUERY_NONE },
	{ 0, "", 'n', 5, '[', 0, "", 'n', QUERY_NONE },
	{ 0, "", 'n', 6, '[', 0, "", 'R', QUERY_NONE },
	{ '?', "", 'n', 6, '[', '?', "", 'R', QUERY_NONE },
	{ '?', "", 'n', 62, '[', 0, "*", '{', QUERY_NONE },
	{ '?', "", 'n', 63, 'P', 0, "!", '~', QUERY_SAME },
	{ '?', "", 'n', -1, '[', '?', "", 'n', QUERY_NONE },
	{ 0, "$", 'p', -1, '[', 0, "$", 'y', QUERY_SAME },
	{ '?', "$", 'p', -1, '[', '?', "$", 'y', QUERY_SAME },
	{ '?', "", 'u', -1, '[', '?', "", 'u', QUERY_NONE },
	{ '?', "", 'S', -1, '[', '?', "", 'S', QUERY_SAME },
	{ '>', "", 'm', -1, '[', '>', "", 'm', QUERY_SAME },
	{ '>', "", 'f', -1, '[', '>', "", 'f', QUERY_SAME },
	{ 0, "", 't', 11, '[', 0, "", 't', QUERY_MINUS10 },
	{ 0, "", 't', -2, '[', 0, "", 't', QUERY_MINUS10 },
	{ 0, "", 't', 20, ']', 0, "", 'L', QUERY_NONE },
	{ 0, "", 't', 21, ']', 0, "", 'l', QUERY_NONE },
	{ 0, "", 'x', -1, '[', 0, "", 'x', QUERY_PLUS2 },
	{ 0, "\"", 'v', -1, '[', 0, "\"", 'w', QUERY_NONE },
	{ 0, "'", '|', -1, '[', 0, "&", 'w', QUERY_NONE },
	{ 0, "#", '|', -1, '[', 0, "", 'm', QUERY_NONE },
	{ 0, "*", 'y', -1, 'P', 0, "!", '~', QUERY_SAME },
	{ 0, "$", 'w', 1, 'P', 0, "$", 'u', QUERY_SAME },
	{ 0, "$", 'w', 2, 'P', 0, "$", 'u', QUERY_SAME },
	{ 0, "&", 'u', -1, 'P', 0, "!", 'u', QUERY_NONE }
};

static const char *tty_query_osc[] = {
	"60;", "61;", "62;", "1337;ReportCellSize=", "1337;ReportVariable="
};

static int
tty_query_key(const char *buf, size_t len, struct tty_query_key *key,
    int request, u_int item)
{
	char tmp[TTY_QUERY_LIMIT + 1], *p, *end, *params, *value, *next;
	size_t i, n;
	int param, action = 0, quiet = 0;

	memset(key, 0, sizeof *key);
	if (len < 3)
		return (0);
	if (len > TTY_QUERY_LIMIT)
		len = TTY_QUERY_LIMIT;
	memcpy(tmp, buf, len);
	tmp[len] = '\0';
	key->intro = tmp[1];
	p = tmp + 2;
	if (key->intro == ']') {
		if (tmp[len - 1] == '\007')
			tmp[len - 1] = '\0';
		else if (tmp[len - 2] == '\033' && tmp[len - 1] == '\\')
			tmp[len - 2] = '\0';
		if (!request) {
			strlcpy(key->echo, p, sizeof key->echo);
			return (1);
		}
		param = strtoul(p, &end, 10);
		if (*end == ';' && (param == 4 || param == 5 ||
		    (param >= 10 && param <= 19))) {
			p = end + 1;
			while ((value = strsep(&p, ";")) != NULL) {
				if (param == 4 || param == 5) {
					next = strsep(&p, ";");
					if (next == NULL)
						break;
				} else
					next = value;
				if (strcmp(next, "?") == 0 && item-- == 0) {
					if (param == 4 || param == 5)
						xsnprintf(key->echo, sizeof key->echo,
						    "%d;%s;", param, value);
					else
						xsnprintf(key->echo, sizeof key->echo,
						    "%d;", param);
					return (1);
				}
				if (param != 4 && param != 5)
					param++;
			}
			return (0);
		}
		if (item != 0)
			return (0);
		for (end = p + 1; *end != '\0'; end++) {
			if (*end == '?' && (end[-1] == ';' || end[-1] == '=')) {
				*end = '\0';
				if (strlen(p) >= sizeof key->echo)
					return (0);
				strlcpy(key->echo, p, sizeof key->echo);
				return (1);
			}
		}
		for (i = 0; i < nitems(tty_query_osc); i++) {
			n = strlen(tty_query_osc[i]) - 1;
			if (strncmp(p, tty_query_osc[i], n) == 0 &&
			    (p[n] == '\0' || p[n] == tty_query_osc[i][n])) {
				strlcpy(key->echo, tty_query_osc[i], sizeof key->echo);
				return (1);
			}
		}
		return (0);
	}
	if (request && item != 0 && key->intro != 'P')
		return (0);
	if (key->intro == '_') {
		if (*p++ != 'G' || (end = strchr(p, ';')) == NULL)
			return (0);
		key->final = 'G';
		*end = '\0';
		while ((value = strsep(&p, ",")) != NULL) {
			if (strcmp(value, "a=q") == 0)
				action = 1;
			if (strcmp(value, "q=2") == 0)
				quiet = 1;
			if (strncmp(value, "i=", 2) == 0)
				strlcpy(key->echo, value + 2, sizeof key->echo);
		}
		return (!request || action || (*key->echo != '\0' && !quiet));
	}
	if (key->intro != '[' && key->intro != 'P')
		return (0);
	if (*p >= '<' && *p <= '?')
		key->prefix = *p++;
	if (key->intro == 'P')
		key->prefix = 0;
	params = p;
	while (*p >= '0' && *p <= ';')
		p++;
	param = strtoul(params, &next, 10);
	for (n = 0; *p >= ' ' && *p <= '/'; p++) {
		if (n == sizeof key->interm - 1)
			return (0);
		key->interm[n++] = *p;
	}
	if (*p < '@' || *p > '~')
		return (0);
	key->final = *p;
	if (!request) {
		if ((key->intro == '[' && (key->final == 'y' ||
		    key->final == 'S' || key->final == 'm' ||
		    key->final == 'f' || key->final == 't' ||
		    key->final == 'x')) ||
		    (key->intro == 'P' && (key->final == '~' ||
		    (key->final == 'u' && strcmp(key->interm, "$") == 0))))
			xsnprintf(key->echo, sizeof key->echo, "%d",
			    key->intro == '[' && key->final == 't' && param == 2 ? 1 : param);
	}
	if (key->intro == 'P') {
		if (request) {
			if ((strcmp(key->interm, "$") != 0 || *p != 'q') &&
			    (strcmp(key->interm, "+") != 0 || (*p != 'q' && *p != 'Q')))
				return (0);
			key->final = (*p == 'Q' ? 'R' : 'r');
		}
		if (key->interm[0] == '+') {
			p++;
			while (request && item-- != 0) {
				p += strcspn(p, ";\033");
				if (*p != ';')
					return (0);
				p++;
			}
			n = strcspn(p, "=;\033");
			if (n >= sizeof key->echo)
				return (0);
			memcpy(key->echo, p, n);
		} else if (request && item != 0)
			return (0);
		return (1);
	}
	if (!request)
		return (1);
	for (i = 0; i < nitems(tty_query_table); i++) {
		if (key->prefix != tty_query_table[i].prefix ||
		    strcmp(key->interm, tty_query_table[i].interm) != 0 ||
		    key->final != tty_query_table[i].final ||
		    (tty_query_table[i].param >= 0 && param != tty_query_table[i].param) ||
		    (tty_query_table[i].param == -2 && param != 13 && param != 14 &&
		    param != 15 && param != 16 && param != 18 && param != 19))
			continue;
		key->intro = tty_query_table[i].intro;
		key->prefix = tty_query_table[i].reply_prefix;
		strlcpy(key->interm, tty_query_table[i].reply_interm, sizeof key->interm);
		key->final = tty_query_table[i].reply_final;
		if (key->intro == 'P' && key->final == '~' &&
		    tty_query_table[i].prefix == '?')
			param = *next == ';' ? strtoul(next + 1, NULL, 10) : 0;
		if (tty_query_table[i].echo != QUERY_NONE) {
			if (tty_query_table[i].echo == QUERY_MINUS10)
				param -= 10;
			else if (tty_query_table[i].echo == QUERY_PLUS2)
				param += 2;
			xsnprintf(key->echo, sizeof key->echo, "%d", param);
		}
		if (key->intro == ']') {
			key->echo[0] = key->final;
			key->final = 0;
		}
		return (1);
	}
	return (0);
}

static void
tty_query_expire(struct tty *tty, uint64_t age)
{
	struct tty_queries *qs = tty->queries;
	struct tty_query *q;
	uint64_t now = get_timer();

	if (qs == NULL)
		return;
	while ((q = TAILQ_FIRST(&qs->list)) != NULL && now - q->time >= age) {
		TAILQ_REMOVE(&qs->list, q, entry);
		free(q);
	}
	if (TAILQ_EMPTY(&qs->list) && (qs->length == 0 || age == 0)) {
		free(qs);
		tty->queries = NULL;
	}
}

static struct tty_query *
tty_query_find(struct tty *tty, const char *buf, size_t len)
{
	struct tty_query *q;
	struct tty_query_key key;

	if (!tty_query_key(buf, len, &key, 0, 0))
		return (NULL);
	TAILQ_FOREACH(q, &tty->queries->list, entry) {
		if (q->key.intro == key.intro && q->key.prefix == key.prefix &&
		    q->key.final == key.final && strcmp(q->key.interm, key.interm) == 0 &&
		    (*q->key.echo == '\0' || (key.intro == ']' ?
		    strncmp(q->key.echo, key.echo, strlen(q->key.echo)) == 0 :
		    strcmp(q->key.echo, key.echo) == 0) ||
		    (key.intro == 'P' && key.interm[0] == '+' && *key.echo == '\0')))
			return (q);
	}
	return (NULL);
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
	struct tty_queries	*qs;
	struct tty_query	*q;
	struct tty_query_key	 key;
	size_t			 i;
	u_int			 count, item;
	int			 n;

	if (tty->flags & TTY_BLOCK)
		return;
	tty_query_expire(tty, TTY_QUERY_EXPIRY);
	qs = tty->queries;
	if (qs == NULL) {
		qs = tty->queries = xcalloc(1, sizeof *qs);
		TAILQ_INIT(&qs->list);
	}
	for (i = 0; i < len; i++) {
		if (qs->length == 0) {
			if (buf[i] != '\033')
				continue;
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
		if (qs->length < TTY_QUERY_LIMIT) {
			qs->pending[qs->length++] = buf[i];
			n = tty_query_step(qs->pending, qs->length);
		} else
			n = (qs->pending[1] == '[' && buf[i] >= '@' && buf[i] <= '~') ||
			    (qs->pending[1] == ']' && buf[i] == '\007') ||
			    (qs->last == '\033' && buf[i] == '\\') ? 0 : 1;
		qs->last = buf[i];
		if (n == 1)
			continue;
		if (n == 0) {
			for (item = 0; item < TTY_QUERY_MAX &&
			    tty_query_key(qs->pending, qs->length, &key, 1, item); item++) {
				count = 0;
				TAILQ_FOREACH(q, &qs->list, entry)
					count++;
				if (count == TTY_QUERY_MAX) {
					q = TAILQ_FIRST(&qs->list);
					TAILQ_REMOVE(&qs->list, q, entry);
					free(q);
				}
				q = xcalloc(1, sizeof *q);
				q->key = key;
				q->owner = qs->owner;
				q->target = qs->target;
				q->time = get_timer();
				TAILQ_INSERT_TAIL(&qs->list, q, entry);
			}
		}
		qs->length = 0;
	}
	tty_query_expire(tty, TTY_QUERY_EXPIRY);
}

int
tty_query_reply(struct tty *tty, const char *buf, size_t len, size_t *size,
    int apply)
{
	struct tty_queries *qs;
	struct tty_query *q;
	struct side_status_line *ss = &tty->client->side_status;
	struct window_pane *wp;
	int n;

	tty_query_expire(tty, TTY_QUERY_EXPIRY);
	qs = tty->queries;
	if (qs == NULL || TAILQ_EMPTY(&qs->list) || len < 2 || strchr("[P]_", buf[1]) == NULL)
		return (-1);
	if (!apply) {
		n = tty_query_frame(buf, len, size);
		if (n != 0)
			return (n);
	} else
		*size = len;
	q = tty_query_find(tty, buf, *size);
	if (q == NULL)
		return (-1);
	if (!apply)
		return (q->owner == TTY_QUERY_TMUX ? 2 : 0);
	n = q->owner == TTY_QUERY_TMUX ? -1 : 0;
	switch (q->owner) {
	case TTY_QUERY_TMUX:
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
	TAILQ_REMOVE(&qs->list, q, entry);
	free(q);
	tty_query_expire(tty, TTY_QUERY_EXPIRY);
	return (n);
}

void
tty_query_free(struct tty *tty)
{
	tty_query_expire(tty, 0);
}
