/*
 * Parser based on Ghostty's src/terminal/osc/parsers/program_status.zig:
 * https://github.com/ghostty-org/ghostty/pull/14560
 *
 * Copyright (c) 2024 Mitchell Hashimoto, Ghostty contributors
 * Copyright (c) 2026 Andrey Popp <andreypopp@gmail.com>
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include "tmux.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#define PROGRAM_RECORDS 64

enum program_state { PROGRAM_IDLE, PROGRAM_WORKING, PROGRAM_DONE,
    PROGRAM_BLOCKED, PROGRAM_ERROR, PROGRAM_CLEAR, PROGRAM_INVALID };
static const char *program_states[] = {
    "idle", "working", "done", "blocked", "error", "clear"
};

struct program_record {
	char id[129], app[33], title[257], msg[685], kind[11];
	int title_set, msg_set, progress;
	enum program_state state;
};

struct program_status {
	struct program_record records[PROGRAM_RECORDS];
	u_int count;
	int real, dirty;
	uint64_t serial;
	struct event timer;
	struct program_payload *payload;
};

static int
program_name(const char *s)
{
	return (*s != '\0' && strspn(s,
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.+-")
	    == strlen(s));
}

static int
program_id(const char *s)
{
	char copy[129], *p, *part;
	u_int depth = 0;

	if (*s == '\0' || strlen(s) > 128)
		return (0);
	strlcpy(copy, s, sizeof copy);
	p = copy;
	while ((part = strsep(&p, "/")) != NULL) {
		if (++depth > 8 || strlen(part) > 32 || !program_name(part))
			return (0);
	}
	return (1);
}

static char *
program_trim(char *s)
{
	char *end;

	while (isspace((u_char)*s))
		s++;
	end = s + strlen(s);
	while (end > s && isspace((u_char)end[-1]))
		*--end = '\0';
	return (s);
}

static int
program_text(const char *s, size_t cap)
{
	static const char alphabet[] =
	    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
	u_char decoded[512];
	size_t n = strlen(s), unpadded = n, out = 0, i;
	unsigned int bits = 0, value = 0, cp, minimum;
	const char *digit;
	int more;

	while (unpadded > 0 && s[unpadded - 1] == '=')
		unpadded--;
	if (n - unpadded > 2 || unpadded % 4 == 1 ||
	    (n != unpadded && (n % 4 != 0 ||
	    n - unpadded != (4 - unpadded % 4) % 4)))
		return (0);
	for (i = 0; i < unpadded; i++) {
		digit = strchr(alphabet, s[i]);
		if (digit == NULL)
			return (0);
		value = (value << 6) | (digit - alphabet);
		bits += 6;
		if (bits >= 8) {
			bits -= 8;
			if (out == cap)
				return (0);
			decoded[out++] = (value >> bits) & 255;
		}
	}
	if (bits != 0 && (value & ((1U << bits) - 1)) != 0)
		return (0);
	for (i = 0; i < out;) {
		cp = decoded[i++];
		minimum = 0;
		if (cp < 0x80)
			more = 0;
		else if (cp >= 0xc2 && cp <= 0xdf) {
			more = 1; cp &= 0x1f; minimum = 0x80;
		} else if (cp >= 0xe0 && cp <= 0xef) {
			more = 2; cp &= 0x0f; minimum = 0x800;
		} else if (cp >= 0xf0 && cp <= 0xf4) {
			more = 3; cp &= 7; minimum = 0x10000;
		} else
			return (0);
		while (more-- > 0) {
			if (i == out || (decoded[i] & 0xc0) != 0x80)
				return (0);
			cp = (cp << 6) | (decoded[i++] & 0x3f);
		}
		if (cp < minimum || cp > 0x10ffff ||
		    (cp >= 0xd800 && cp <= 0xdfff) || cp < 32 ||
		    (cp >= 0x7f && cp <= 0x9f))
			return (0);
	}
	return (1);
}

static int
program_parse(const char *body, struct program_record *r)
{
	char *copy = xstrdup(body), *p = copy, *pair, *eq, *key, *v;
	u_int i;
	int valid = 0, progress;
	size_t len;

	memset(r, 0, sizeof *r);
	r->state = PROGRAM_INVALID;
	r->progress = -1;
	while ((pair = strsep(&p, ":")) != NULL) {
		eq = strchr(pair, '=');
		if (eq == NULL)
			continue;
		*eq = '\0';
		key = program_trim(pair);
		v = program_trim(eq + 1);
		len = strlen(v);
		if (strlen(key) > 16 ||
		    (strcmp(key, "app") == 0 && len > 32) ||
		    (strcmp(key, "id") == 0 && len > 128) ||
		    (strcmp(key, "title") == 0 && len > 256) ||
		    (strcmp(key, "msg") == 0 && len > 684))
			goto out;
		if (*key == '\0' || strspn(key, "abcdefghijklmnopqrstuvwxyz")
		    != strlen(key) || strspn(v,
		    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789_.,+/=-")
		    != len)
			continue;
		if (strcmp(key, "state") == 0) {
			r->state = PROGRAM_INVALID;
			for (i = 0; i < nitems(program_states); i++) {
				if (strcmp(v, program_states[i]) == 0)
					r->state = i;
			}
		} else if (strcmp(key, "id") == 0) {
			if (!program_id(v))
				goto out;
			strlcpy(r->id, v, sizeof r->id);
		} else if (strcmp(key, "app") == 0) {
			r->app[0] = '\0';
			if (program_name(v))
				strlcpy(r->app, v, sizeof r->app);
		} else if (strcmp(key, "title") == 0 || strcmp(key, "msg") == 0) {
			if (!program_text(v, strcmp(key, "title") == 0 ? 192 : 512))
				goto out;
			if (strcmp(key, "title") == 0) {
				strlcpy(r->title, v, sizeof r->title);
				r->title_set = 1;
			} else {
				strlcpy(r->msg, v, sizeof r->msg);
				r->msg_set = 1;
			}
		} else if (strcmp(key, "kind") == 0) {
			r->kind[0] = '\0';
			if (strcmp(v, "permission") == 0 || strcmp(v, "question") == 0 ||
			    strcmp(v, "auth") == 0)
				strlcpy(r->kind, v, sizeof r->kind);
		} else if (strcmp(key, "progress") == 0) {
			progress = 0;
			r->progress = -1;
			if (*v == '\0')
				continue;
			for (i = 0; v[i] != '\0'; i++) {
				if (v[i] < '0' || v[i] > '9')
					break;
				progress = progress * 10 + v[i] - '0';
				if (progress > 100)
					break;
			}
			if (v[i] == '\0')
				r->progress = progress;
		}
	}
	valid = r->state != PROGRAM_INVALID;
	if (r->state != PROGRAM_BLOCKED)
		r->kind[0] = '\0';
	if (r->state != PROGRAM_BLOCKED && r->state != PROGRAM_WORKING)
		r->progress = -1;
out:
	free(copy);
	return (valid);
}

void
program_payload_unref(struct program_payload *payload)
{
	if (payload != NULL && --payload->references == 0) {
		free(payload->text);
		free(payload);
	}
}

static int
program_compare(const void *a, const void *b)
{
	const struct program_record *const *ra = a, *const *rb = b;

	return (strcmp((*ra)->id, (*rb)->id));
}

static void
program_emit(struct window_pane *wp)
{
	struct program_status *ps = wp->program_status;
	struct program_record *order[PROGRAM_RECORDS], *r;
	struct program_payload *payload;
	struct evbuffer *buffer = evbuffer_new();
	u_int i;

	payload = xcalloc(1, sizeof *payload);
	payload->references = 1;
	payload->serial = ++ps->serial;
	evbuffer_add_printf(buffer, "{\"serial\":%llu,\"records\":[",
	    (unsigned long long)ps->serial);
	for (i = 0; i < ps->count; i++)
		order[i] = &ps->records[i];
	qsort(order, ps->count, sizeof *order, program_compare);
	for (i = 0; i < ps->count; i++) {
		r = order[i];
		evbuffer_add_printf(buffer, "%s{\"id\":\"%s\",\"state\":\"%s\"",
		    i == 0 ? "" : ",", r->id, program_states[r->state]);
		if (*r->app != '\0')
			evbuffer_add_printf(buffer, ",\"app\":\"%s\"", r->app);
		if (*r->kind != '\0')
			evbuffer_add_printf(buffer, ",\"kind\":\"%s\"", r->kind);
		if (r->progress != -1)
			evbuffer_add_printf(buffer, ",\"progress\":%d", r->progress);
		if (r->title_set)
			evbuffer_add_printf(buffer, ",\"title\":\"%s\"", r->title);
		if (r->msg_set)
			evbuffer_add_printf(buffer, ",\"msg\":\"%s\"", r->msg);
		evbuffer_add(buffer, "}", 1);
	}
	evbuffer_add(buffer, "]}", 2);
	payload->text = xmalloc(EVBUFFER_LENGTH(buffer) + 1);
	memcpy(payload->text, EVBUFFER_DATA(buffer), EVBUFFER_LENGTH(buffer));
	payload->text[EVBUFFER_LENGTH(buffer)] = '\0';
	evbuffer_free(buffer);
	program_payload_unref(ps->payload);
	ps->payload = payload;
	control_program_status(wp, payload);
}

static void
program_timer(__unused int fd, __unused short events, void *data)
{
	struct window_pane *wp = data;
	struct program_status *ps = wp->program_status;
	struct timeval tv = { 0, 100000 };

	if (ps->dirty) {
		ps->dirty = 0;
		program_emit(wp);
		evtimer_add(&ps->timer, &tv);
	}
}

static void
program_changed(struct window_pane *wp)
{
	struct program_status *ps = wp->program_status;
	struct timeval tv = { 0, 100000 };

	if (evtimer_pending(&ps->timer, NULL))
		ps->dirty = 1;
	else {
		program_emit(wp);
		evtimer_add(&ps->timer, &tv);
	}
}

static struct program_status *
program_get(struct window_pane *wp)
{
	if (wp->program_status == NULL) {
		wp->program_status = xcalloc(1, sizeof *wp->program_status);
		evtimer_set(&wp->program_status->timer, program_timer, wp);
	}
	return (wp->program_status);
}

static void
program_remove(struct program_status *ps, u_int i)
{
	ps->count--;
	memmove(&ps->records[i], &ps->records[i + 1],
	    (ps->count - i) * sizeof ps->records[0]);
}

void
program_status_report(struct window_pane *wp, const char *body, int real)
{
	struct program_record r;
	struct program_status *ps;
	u_int i;
	size_t len;
	int changed = 0;

	if (wp == NULL || !program_parse(body, &r))
		return;
	ps = program_get(wp);
	if (!real && ps->real)
		return;
	if (real)
		ps->real = 1;
	if (r.state == PROGRAM_CLEAR) {
		len = strlen(r.id);
		for (i = 0; i < ps->count;) {
			if (len == 0 || (strncmp(r.id, ps->records[i].id, len) == 0 &&
			    (ps->records[i].id[len] == '\0' || ps->records[i].id[len] == '/'))) {
				program_remove(ps, i);
				changed = 1;
			} else
				i++;
		}
		if (changed)
			program_changed(wp);
		return;
	}
	for (i = 0; i < ps->count; i++) {
		if (strcmp(ps->records[i].id, r.id) == 0) {
			program_remove(ps, i);
			break;
		}
	}
	if (ps->count == PROGRAM_RECORDS)
		program_remove(ps, 0);
	ps->records[ps->count++] = r;
	program_changed(wp);
}

void
program_status_clear(struct window_pane *wp, int reset)
{
	struct program_status *ps = wp->program_status;
	u_int i;
	int changed = 0;

	if (ps == NULL)
		return;
	if (reset)
		ps->real = 0;
	for (i = 0; i < ps->count;) {
		if (reset || (ps->records[i].state != PROGRAM_DONE &&
		    ps->records[i].state != PROGRAM_ERROR)) {
			program_remove(ps, i);
			changed = 1;
		} else
			i++;
	}
	if (changed)
		program_changed(wp);
}

void
program_status_free(struct window_pane *wp)
{
	struct program_status *ps = wp->program_status;

	if (ps == NULL)
		return;
	control_program_status_discard(wp);
	evtimer_del(&ps->timer);
	program_payload_unref(ps->payload);
	free(ps);
	wp->program_status = NULL;
}

const char *
program_status_format(struct window_pane *wp)
{
	if (wp->program_status == NULL || wp->program_status->payload == NULL)
		return ("{\"serial\":0,\"records\":[]}");
	return (wp->program_status->payload->text);
}
