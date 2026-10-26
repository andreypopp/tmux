/* $OpenBSD$ */

/*
 * Copyright (c) 2026 Michael Grant <mgrant@grant.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY, FITNESS AND NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHOR
 * BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION
 * OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH
 * THIS SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include <sys/types.h>
#include <sys/mman.h>
#include <sys/stat.h>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <limits.h>
#include <math.h>
#include <resolv.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

/* State attached to an input parser. */
struct kitty_state {
	char				 action;
	u_int				 format;
	char				 medium;
	char				 compression;
	char				 delete;
	u_int				 width;
	u_int				 height;
	u_int				 source_x;
	u_int				 source_y;
	u_int				 source_width;
	u_int				 source_height;
	u_int				 x_offset;
	u_int				 y_offset;
	u_int				 columns;
	u_int				 rows;
	u_int				 image_id;
	u_int				 image_number;
	u_int				 placement_id;
	u_int				 quiet;
	u_int				 data_size;
	u_int				 data_offset;
	int				 z;
	int				 no_cursor;
	int				 more;

	u_char				*data;
	size_t				 size;
	size_t				 allocated;

	TAILQ_HEAD(, kitty_image_map)	 images;
	TAILQ_ENTRY(kitty_state)	 entry;
};
TAILQ_HEAD(kitty_states, kitty_state);

/* Kitty input states holding uploaded images. */
static struct kitty_states kitty_states = TAILQ_HEAD_INITIALIZER(kitty_states);

/* Map a Kitty placement ID to an image ID. */
struct kitty_placement_map {
	u_int					 kitty_id;
	u_int					 image_id;

	TAILQ_ENTRY(kitty_placement_map)	 entry;
};

/* Map Kitty image ID to image ID and placement views. */
struct kitty_image_map {
	u_int					 kitty_id;
	u_int					 image_id;
	TAILQ_HEAD(, kitty_placement_map)	 placements;

	TAILQ_ENTRY(kitty_image_map)		 entry;
};

struct image_shm {
	char name[32];
	struct event timer;
	u_int references;
	TAILQ_ENTRY(image_shm) entry;
};
static TAILQ_HEAD(, image_shm) image_shms = TAILQ_HEAD_INITIALIZER(image_shms);

static void
image_shm_cleanup(void)
{
	struct image_shm *guard;

	TAILQ_FOREACH(guard, &image_shms, entry)
		shm_unlink(guard->name);
}

static void
image_shm_expire(__unused int fd, __unused short events, void *arg)
{
	struct image_shm *guard = arg;

	shm_unlink(guard->name);
	TAILQ_REMOVE(&image_shms, guard, entry);
	free(guard);
}

static void
image_shm_cancel(const char *name)
{
	struct image_shm *guard;

	TAILQ_FOREACH(guard, &image_shms, entry) {
		if (strcmp(guard->name, name) != 0)
			continue;
		evtimer_del(&guard->timer);
		TAILQ_REMOVE(&image_shms, guard, entry);
		guard->name[0] = '\0';
		if (guard->references == 0)
			free(guard);
		break;
	}
}

static struct image_shm *
image_shm_new(const char *name)
{
	static int registered;
	struct image_shm *guard;

	image_shm_cancel(name);
	if (!registered) {
		atexit(image_shm_cleanup);
		registered = 1;
	}
	guard = xcalloc(1, sizeof *guard);
	strlcpy(guard->name, name, sizeof guard->name);
	evtimer_set(&guard->timer, image_shm_expire, guard);
	TAILQ_INSERT_TAIL(&image_shms, guard, entry);
	return (guard);
}

void
image_shm_ref(struct image_shm *guard)
{
	guard->references++;
}

void
image_shm_unref(struct image_shm *guard)
{
	struct timeval tv = { 1, 0 };

	if (--guard->references != 0)
		return;
	if (guard->name[0] == '\0')
		free(guard);
	else
		evtimer_add(&guard->timer, &tv);
}

#define IMAGE_CAPTURE_BUDGET (8 * 1024 * 1024)
#define IMAGE_CAPTURE_MORE (sizeof "more 4294967295:4294967295:4294967295:18446744073709551615\n" - 1)

static struct kitty_image_map *kitty_image_map_find_image(struct kitty_state *,
    struct image *);

static struct kitty_image_map *kitty_image_map_find(struct kitty_state *, u_int);

static char *
kitty_capture_keys(struct image_payload *payload)
{
	char	width[32] = "", height[32] = "", *keys;

	if (payload->width != 0)
		xsnprintf(width, sizeof width, "s=%u,", payload->width);
	if (payload->height != 0)
		xsnprintf(height, sizeof height, "v=%u,", payload->height);
	xasprintf(&keys, "f=%u,%s%s%s", payload->format,
	    payload->compression == 'z' ? "o=z," : "", width, height);
	return (keys);
}

static size_t
kitty_write_chunk(char *buf, const char *keys, const u_char *data, size_t size,
    size_t offset, int more, size_t *len)
{
	size_t n = size - offset, header, encoded;

	if (n > 3072)
		n = 3072;
	header = snprintf(buf, 512, "\033_G%sm=%d%s;",
	    offset == 0 ? keys : "", offset + n < size || more,
	    offset == 0 ? "" : ",q=2");
	encoded = 4 * ((n + 2) / 3);
	b64_ntop(data == NULL ? (const u_char *)"" : data + offset, n,
	    buf + header, encoded + 1);
	memcpy(buf + header + encoded, "\033\\", 3);
	*len = header + encoded + 2;
	return (offset + n);
}

void
image_transmit_chunk(struct image_data *data, const char *keys, size_t *offset,
    char *buf, size_t *len)
{
	struct image_payload *payload = image_data_payload(data);

	*offset = kitty_write_chunk(buf, keys, payload->data, payload->size,
	    *offset, 0, len);
}

static size_t
kitty_capture_data(char **buf, size_t *len, const char *keys,
    const u_char *data, size_t size, size_t offset, int more, size_t reserve)
{
	char chunk[IMAGE_CHUNK_MAX];
	size_t next, length;

	do {
		next = kitty_write_chunk(chunk, keys, data, size, offset, more,
		    &length);
		if (*len + length + 1 + reserve >
		    IMAGE_CAPTURE_BUDGET)
			break;
		*buf = xrealloc(*buf, *len + length + 2);
		memcpy(*buf + *len, chunk, length);
		(*buf)[*len + length] = '\n';
		*len += length + 1;
		(*buf)[*len] = '\0';
		offset = next;
	} while (offset < size);
	return (offset);
}

static char *
kitty_state_keys(struct kitty_state *ks, struct image_payload *payload,
    u_int quiet)
{
	char *format = kitty_capture_keys(payload), *keys;

	xasprintf(&keys, "a=%c,%si=%u,I=%u,p=%u,%s"
	    "x=%u,y=%u,w=%u,h=%u,X=%u,Y=%u,c=%u,r=%u,C=%d,q=%u,S=%u,z=%d,",
	    ks->action, ks->medium == 's' ? "t=d," : "",
	    ks->image_id, ks->image_number, ks->placement_id,
	    format, ks->source_x, ks->source_y, ks->source_width,
	    ks->source_height, ks->x_offset, ks->y_offset, ks->columns,
	    ks->rows, ks->no_cursor, quiet, ks->data_size, ks->z);
	free(format);
	return (keys);
}

char *
image_capture_pending(void *state)
{
	struct kitty_state	*ks = state;
	struct image_payload	 payload;
	char			*keys, *result = xstrdup("");
	size_t			 len = 0, chunks, encoded, overhead;

	if (ks == NULL || !ks->more)
		return (result);
	payload = (struct image_payload) { ks->data, ks->size,
	    ks->format, ks->width, ks->height, ks->compression };
	keys = kitty_state_keys(ks, &payload, ks->quiet);
	chunks = ks->size == 0 ? 1 : (ks->size + 3071) / 3072;
	encoded = 4 * ((ks->size + 2) / 3);
	overhead = strlen(keys) + sizeof "\033_Gm=1;\033\\\n" - 1 +
	    (chunks - 1) * (sizeof "\033_Gm=1,q=2;\033\\\n" - 1);
	if (encoded + overhead <= IMAGE_CAPTURE_BUDGET)
		kitty_capture_data(&result, &len, keys, ks->data, ks->size, 0, 1, 0);
	free(keys);
	return (result);
}

static int
kitty_capture_compare(const void *a, const void *b)
{
	const struct image_rect	*ra = a, *rb = b;

	return (ra->dst_x < rb->dst_x ? -1 : ra->dst_x > rb->dst_x);
}

struct image_capture *
image_capture_select(void *state, struct grid *gd, u_int top, u_int bottom,
    u_int *count)
{
	struct kitty_state	*ks = state;
	struct image_rect	*rects;
	struct image_capture	*selected = NULL;
	struct kitty_image_map	*map;
	struct image		*parent;
	u_int			 y, i, j, n, id, kept = 0;

	*count = 0;
	if (ks == NULL)
		return (NULL);
	for (y = top; y <= bottom; y++) {
		rects = image_grid_capture_line(gd, y, &n);
		qsort(rects, n, sizeof *rects, kitty_capture_compare);
		for (i = 0; i < n; i++) {
			for (j = 0; j < *count; j++) {
				if (selected[j].rect.image == rects[i].image)
					break;
			}
			if (j != *count) {
				if (rects[i].src_y == selected[j].rect.src_y +
				    selected[j].rect.sy && y == selected[j].rect.dst_y +
				    selected[j].rect.sy)
					selected[j].rect.sy++;
				continue;
			}
			selected = xreallocarray(selected, *count + 1, sizeof *selected);
			selected[(*count)++].rect = rects[i];
		}
		free(rects);
	}
	for (i = 0; i < *count; i++) {
		parent = image_get_parent(selected[i].rect.image);
		if (parent != NULL && image_get_type(parent) == IMAGE_TYPE_KITTY)
			selected[kept++] = selected[i];
	}
	*count = kept;
	for (i = 0; i < *count; i++) {
		parent = image_get_parent(selected[i].rect.image);
		map = kitty_image_map_find_image(ks, parent);
		for (j = 0; j < i; j++) {
			if (image_get_parent(selected[j].rect.image) == parent)
				break;
		}
		if (j < i)
			id = selected[j].id;
		else if (map != NULL)
			id = map->kitty_id;
		else {
			id = 0x80000000U | (image_get_id(parent) & 0x7fffffffU);
			for (;;) {
				for (j = 0; j < i && selected[j].id != id; j++)
					;
				if (kitty_image_map_find(ks, id) == NULL && j == i)
					break;
				id = 0x80000000U | ((id + 1) & 0x7fffffffU);
			}
		}
		selected[i].id = id;
	}
	return (selected);
}

char *
image_capture_line(void *state, struct grid *gd, struct screen *s, u_int y,
    struct image_capture *rects, u_int count, struct grid_cell **gc, int flags)
{
	struct kitty_state		*ks = state;
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	struct image_rect		*r;
	struct image_geom		*ig;
	const struct grid_line		*gl = grid_peek_line(gd, y);
	struct evbuffer			*out;
	char				*text, *result;
	u_int				 i, x = 0, end, px, py, pw, ph, columns, rows;

	for (i = 0; i < count; i++) {
		if (rects[i].rect.dst_y == y)
			break;
	}
	if (i == count)
		return (grid_string_cells(gd, 0, y, gd->sx, gc, flags, s));
	out = evbuffer_new();
	for (; i < count && rects[i].rect.dst_y == y; i++) {
		r = &rects[i].rect;
		map = kitty_image_map_find_image(ks, image_get_parent(r->image));
		text = grid_string_cells(gd, x, y, r->dst_x - x, gc,
		    flags & ~GRID_STRING_TRIM_SPACES, s);
		evbuffer_add(out, text, strlen(text));
		free(text);
		end = flags & GRID_STRING_EMPTY_CELLS ? gl->cellsize : gl->cellused;
		if (end > r->dst_x)
			end = r->dst_x;
		if (end < x)
			end = x;
		for (; end < r->dst_x; end++)
			evbuffer_add(out, " ", 1);
		x = r->dst_x;
		image_get_pixel_area(r->image, r->src_x, r->src_y, r->sx,
		    r->sy, &px, &py, &pw, &ph);
		ig = image_get_geometry(r->image);
		columns = r->sx;
		rows = r->sy;
		if (ig->scaled_width == ig->src_w &&
		    (double)(r->src_x + r->sx) * ig->canvas_width / ig->sx >
		    ig->canvas_xoff + ig->scaled_width)
			columns = 0;
		if (ig->scaled_height == ig->src_h &&
		    (double)(r->src_y + r->sy) * ig->canvas_height / ig->sy >
		    ig->canvas_yoff + ig->scaled_height)
			rows = 0;
		evbuffer_add_printf(out, "\033_Ga=p,i=%u,",
		    rects[i].id);
		if (ig->z != 0)
			evbuffer_add_printf(out, "z=%d,", ig->z);
		if (r->src_x == 0 && ig->canvas_xoff != 0)
			evbuffer_add_printf(out, "X=%u,", ig->canvas_xoff);
		if (r->src_y == 0 && ig->canvas_yoff != 0)
			evbuffer_add_printf(out, "Y=%u,", ig->canvas_yoff);
		if (map != NULL) {
			TAILQ_FOREACH(kpl, &map->placements, entry) {
				if (kpl->image_id == image_get_id(r->image)) {
					if (kpl->kitty_id != 0)
						evbuffer_add_printf(out, "p=%u,", kpl->kitty_id);
					break;
				}
			}
		}
		evbuffer_add_printf(out,
		    "q=2,C=1,c=%u,r=%u,x=%u,y=%u,w=%u,h=%u\033\\",
		    columns, rows, px + ig->src_x, py + ig->src_y, pw, ph);
	}
	text = grid_string_cells(gd, x, y, gd->sx - x, gc, flags, s);
	evbuffer_add(out, text, strlen(text));
	free(text);
	result = xstrndup(EVBUFFER_DATA(out), EVBUFFER_LENGTH(out));
	evbuffer_free(out);
	return (result);
}

static int
kitty_capture_shm(char **result, size_t *len, const char *keys,
    struct image_payload *payload, size_t reserve)
{
	static u_int	 counter;
	char		 name[32], encoded[45], *line;
	void		*base;
	size_t		 length;
	int		 fd;

	do {
		xsnprintf(name, sizeof name, "/kt%ld-%u", (long)getpid(), ++counter);
		b64_ntop((u_char *)name, strlen(name), encoded, sizeof encoded);
		xasprintf(&line, "\033_G%st=s,S=%zu,q=2;%s\033\\\n",
		    keys, payload->size, encoded);
		length = strlen(line);
		if (*len + length + reserve > IMAGE_CAPTURE_BUDGET) {
			free(line);
			return (0);
		}
		fd = shm_open(name, O_CREAT|O_EXCL|O_RDWR, 0600);
		if (fd == -1)
			free(line);
	} while (fd == -1 && errno == EEXIST);
	if (fd == -1)
		return (-1);
	if (ftruncate(fd, payload->size) != 0) {
		close(fd);
		shm_unlink(name);
		free(line);
		return (-1);
	}
	base = mmap(NULL, payload->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0);
	close(fd);
	if (base == MAP_FAILED) {
		shm_unlink(name);
		free(line);
		return (-1);
	}
	memcpy(base, payload->data, payload->size);
	munmap(base, payload->size);
	{
		struct image_shm *guard = image_shm_new(name);
		struct timeval tv = { 10, 0 };

		evtimer_add(&guard->timer, &tv);
	}
	*result = xrealloc(*result, *len + length + 1);
	memcpy(*result + *len, line, length + 1);
	*len += length;
	free(line);
	return (1);
}

static void
kitty_capture_append(char **result, size_t *len, const char *line)
{
	size_t length = strlen(line);

	*result = xrealloc(*result, *len + length + 1);
	memcpy(*result + *len, line, length + 1);
	*len += length;
}

char *
image_capture_transmit(struct window_pane *wp, struct image_capture *rects, u_int count,
    const char *token, size_t *len, int shm)
{
	struct image_capture	**images = NULL;
	struct image		*parent;
	struct image_payload	 payload;
	struct image_frame_state	 state;
	u_int			 i, j, nimages = 0, start = 0, id, gen, frame = 0;
	int			 gap;
	size_t			 offset = 0, next, reserve;
	char			*keys, *format, *result = xstrdup(""), tail;
	char			*line, *restore, *root;
	char			 canonical[64];

	for (i = 0; i < count; i++) {
		parent = image_get_parent(rects[i].rect.image);
		for (j = 0; j < nimages; j++) {
			if (image_get_parent(images[j]->rect.image) == parent)
				break;
		}
		if (j != nimages)
			continue;
		images = xreallocarray(images, nimages + 1, sizeof *images);
		images[nimages++] = &rects[i];
	}
	if (token != NULL) {
		if (sscanf(token, "%u:%u:%u:%zu%c", &id, &frame, &gen, &offset, &tail) != 4)
			goto changed;
		xsnprintf(canonical, sizeof canonical, "%u:%u:%u:%zu", id, frame, gen, offset);
		if (strcmp(canonical, token) != 0 || offset % 3072 != 0)
			goto changed;
		for (start = 0; start < nimages; start++) {
			if (image_get_id(image_get_parent(images[start]->rect.image)) == id)
				break;
		}
		if (start == nimages)
			goto changed;
		parent = image_get_parent(images[start]->rect.image);
		if (frame >= image_frame_count(parent) ||
		    gen != image_frame_generation(parent, frame))
			goto changed;
		image_frame_payload(parent, frame, &payload);
		if (offset >= payload.size)
			goto changed;
	}
	for (i = start; i < nimages; i++) {
		parent = image_get_parent(images[i]->rect.image);
		if (image_frame_now(parent))
			wp->flags |= PANE_REDRAW;
		restore = xstrdup("");
		if (image_frame_restore(parent, &state)) {
			if (state.root_gap > 0)
				xasprintf(&root, ",r=1,z=%d", state.root_gap);
			else
				root = xstrdup("");
			free(restore);
			xasprintf(&restore, "\033_Ga=a,i=%u%s,c=%u,s=%d,v=%u,q=2\033\\\n",
			    images[i]->id, root, state.current, state.state, state.loops);
			free(root);
		}
		reserve = strlen(restore) + IMAGE_CAPTURE_MORE;
		for (; frame < image_frame_count(parent); frame++) {
			image_frame_payload(parent, frame, &payload);
			format = kitty_capture_keys(&payload);
			if (frame == 0)
				xasprintf(&keys, "a=t,i=%u,%s%s",
				    images[i]->id, format, shm ? "" : "q=2,");
			else {
				gap = image_frame_gap(parent, frame);
				xasprintf(&keys, "a=f,i=%u,%sX=1,z=%d,%s",
				    images[i]->id, format, gap == 0 ? -1 : gap,
				    shm ? "" : "q=2,");
			}
			free(format);
			if (shm) {
				int status = kitty_capture_shm(&result, len, keys, &payload, reserve);

				if (status == -1) {
					free(keys);
					free(restore);
					goto changed;
				}
				next = status ? payload.size : 0;
			} else
				next = kitty_capture_data(&result, len, keys, payload.data,
				    payload.size, offset, 0, reserve);
			free(keys);
			if (next < payload.size) {
				xasprintf(&line, "more %u:%u:%u:%zu\n",
				    image_get_id(parent), frame,
				    image_frame_generation(parent, frame), next);
				kitty_capture_append(&result, len, line);
				free(line);
				free(restore);
				goto done;
			}
			offset = 0;
		}
		frame = 0;
		kitty_capture_append(&result, len, restore);
		free(restore);
	}
done:
	free(images);
	return (result);

changed:
	free(images);
	free(result);
	return (NULL);
}

/* Select Kitty output. */
int
kitty_select(__unused struct tty *tty)
{
	return (0);
}

/* Parse Kitty graphics control data. */
static int
kitty_parse_control_data(struct kitty_state *ks, const u_char *data,
    size_t size, const char **m)
{
	char		*copy, *ptr, *next, *value;
	const char	*errstr;
	u_int		 number;
	char		 key;

	*m = NULL;

	copy = ptr = xstrndup(data, size);
	while ((next = strsep(&ptr, ",")) != NULL) {
		if (*next == '\0') {
			if (ptr == NULL)
				break;
			*m = (*m == NULL ? "EINVAL" : *m);
			continue;
		}
		key = next[0];
		if (next[1] != '=' || next[2] == '\0') {
			*m = (*m == NULL ? "EINVAL" : *m);
			continue;
		}
		value = next + 2;

		switch (key) {
		case 'a':
		case 't':
		case 'o':
		case 'd':
			if (value[1] != '\0') {
				*m = (*m == NULL ? "EINVAL" : *m);
				break;
			}
			switch (key) {
			case 'a':
				ks->action = value[0];
				break;
			case 't':
				ks->medium = value[0];
				break;
			case 'o':
				ks->compression = value[0];
				break;
			case 'd':
				ks->delete = value[0];
				break;
			}
			break;
		case 'z':
			ks->z = strtonum(value, INT_MIN, INT_MAX, &errstr);
			if (errstr != NULL)
				*m = (*m == NULL ? "EINVAL" : *m);
			break;
		case 'U':
		case 'P':
		case 'Q':
		case 'H':
		case 'V':
			if (strtonum(value, INT_MIN, UINT_MAX, &errstr) != 0)
				*m = (*m == NULL ? "ENOTSUP" : *m);
			else if (errstr != NULL)
				*m = (*m == NULL ? "ENOTSUP" : *m);
			break;
		case 'f':
		case 's':
		case 'v':
		case 'i':
		case 'I':
		case 'p':
		case 'q':
		case 'm':
		case 'S':
		case 'O':
		case 'x':
		case 'y':
		case 'w':
		case 'h':
		case 'X':
		case 'Y':
		case 'c':
		case 'r':
		case 'C':
			number = strtonum(value, 0, UINT_MAX, &errstr);
			if (errstr != NULL) {
				*m = (*m == NULL ? "EINVAL" : *m);
				break;
			}
			switch (key) {
			case 'f':
				ks->format = number;
				break;
			case 's':
				ks->width = number;
				break;
			case 'v':
				ks->height = number;
				break;
			case 'i':
				ks->image_id = number;
				break;
			case 'I':
				ks->image_number = number;
				break;
			case 'p':
				ks->placement_id = number;
				break;
			case 'q':
				ks->quiet = number;
				break;
			case 'm':
				ks->more = (number != 0);
				break;
			case 'S':
				ks->data_size = number;
				break;
			case 'O':
				ks->data_offset = number;
				break;
			case 'x':
				ks->source_x = number;
				break;
			case 'y':
				ks->source_y = number;
				break;
			case 'w':
				ks->source_width = number;
				break;
			case 'h':
				ks->source_height = number;
				break;
			case 'X':
				ks->x_offset = number;
				break;
			case 'Y':
				ks->y_offset = number;
				break;
			case 'c':
				ks->columns = number;
				break;
			case 'r':
				ks->rows = number;
				break;
			case 'C':
				ks->no_cursor = (number != 0);
				break;
			}
			break;
		}
	}
	free(copy);
	if (*m != NULL)
		return (-1);
	return (0);
}

/* Free Kitty graphics parser state. */
void
kitty_free_state(void *state)
{
	struct kitty_state		*ks = state;
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;

	if (ks == NULL)
		return;
	TAILQ_REMOVE(&kitty_states, ks, entry);
	while ((map = TAILQ_FIRST(&ks->images)) != NULL) {
		TAILQ_REMOVE(&ks->images, map, entry);
		while ((kpl = TAILQ_FIRST(&map->placements)) != NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			free(kpl);
		}
		image_free(image_find(map->image_id));
		free(map);
	}
	free(ks->data);
	free(ks);
}

/* Remove a Kitty image. */
void
kitty_remove_image(struct image *im)
{
	struct kitty_state		*ks;
	struct kitty_image_map		*map, *next;
	struct kitty_placement_map	*kpl, *kpl1;
	u_int				 id = image_get_id(im);

	TAILQ_FOREACH(ks, &kitty_states, entry) {
		TAILQ_FOREACH_SAFE(map, &ks->images, entry, next) {
			TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
				if (map->image_id != id && kpl->image_id != id)
					continue;
				TAILQ_REMOVE(&map->placements, kpl, entry);
				free(kpl);
			}
			if (map->image_id != id)
				continue;
			TAILQ_REMOVE(&ks->images, map, entry);
			image_free(im);
			free(map);
		}
	}
}

/* Find an image mapping by Kitty image ID. */
static struct kitty_image_map *
kitty_image_map_find(struct kitty_state *ks, u_int id)
{
	struct kitty_image_map	*map;

	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->kitty_id == id)
			return (map);
	}
	return (NULL);
}

static struct kitty_image_map *
kitty_image_map_find_image(struct kitty_state *ks, struct image *parent)
{
	struct kitty_image_map	*map;

	TAILQ_FOREACH(map, &ks->images, entry) {
		if (map->image_id == image_get_id(parent))
			return (map);
	}
	return (NULL);
}

/* Replace the image associated with a Kitty image ID. */
static struct kitty_image_map *
kitty_image_map_set(struct kitty_state *ks, struct screen_write_ctx *ctx,
    u_int id, struct image *im)
{
	struct kitty_image_map		*map;
	struct kitty_placement_map	*kpl;
	struct image			*old;

	if (id == 0)
		return (NULL);
	map = kitty_image_map_find(ks, id);
	if (map != NULL) {
		old = image_find(map->image_id);
		image_set_order_id(im, image_get_order_id(old));
		while ((kpl = TAILQ_FIRST(&map->placements)) != NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			image_clear(ctx, kpl->image_id);
			free(kpl);
		}
		image_free(image_find(map->image_id));
	} else {
		map = xcalloc(1, sizeof *map);
		map->kitty_id = id;
		TAILQ_INIT(&map->placements);
		TAILQ_INSERT_HEAD(&ks->images, map, entry);
	}
	image_ref(im);
	map->image_id = image_get_id(im);
	return (map);
}

/* Replace a named placement, or add a new anonymous placement. */
static void
kitty_placement_map_set(struct screen_write_ctx *ctx,
    struct kitty_image_map *map, u_int id, struct image *im)
{
	struct kitty_placement_map	*kpl, *kpl1;

	if (map == NULL)
		return;
	TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
		if (image_find(kpl->image_id) == NULL) {
			TAILQ_REMOVE(&map->placements, kpl, entry);
			free(kpl);
			continue;
		}
		if (id != 0 && kpl->kitty_id == id)
			break;
	}
	if (kpl == NULL) {
		kpl = xcalloc(1, sizeof *kpl);
		kpl->kitty_id = id;
		TAILQ_INSERT_TAIL(&map->placements, kpl, entry);
	} else
		image_clear(ctx, kpl->image_id);
	kpl->image_id = image_get_id(im);
}

/* Remove mappings for absent placements. */
static u_int
kitty_prune_placements(struct kitty_image_map *map, struct screen *s)
{
	struct kitty_placement_map	*kpl, *kpl1;
	u_int				 removed = 0;
	struct grid			*sgd = s->saved_grid;

	TAILQ_FOREACH_SAFE(kpl, &map->placements, entry, kpl1) {
		if (image_grid_has_image(s->grid, kpl->image_id))
			continue;
		if (sgd != NULL && image_grid_has_image(sgd, kpl->image_id))
			continue;
		TAILQ_REMOVE(&map->placements, kpl, entry);
		free(kpl);
		removed++;
	}
	return (removed);
}

/* Delete selected Kitty placements. */
static void
kitty_delete_images(struct kitty_state *ks, struct screen_write_ctx *ctx)
{
	struct screen			*s = ctx->s;
	struct grid			*gd = s->grid;
	struct kitty_image_map		*map, *map1;
	struct kitty_placement_map	*kpl;
	u_int				 removed;
	char				 how = ks->delete;
	int				 release;

	release = (how >= 'A' && how <= 'Z');
	if (release)
		how += 'a' - 'A';
	TAILQ_FOREACH(map, &ks->images, entry)
		kitty_prune_placements(map, s);
	if (how == 'a')
		image_grid_clear_visible(gd, IMAGE_TYPE_KITTY);

	TAILQ_FOREACH_SAFE(map, &ks->images, entry, map1) {
		removed = 0;
		if (how == 'i') {
			if (map->kitty_id != ks->image_id)
				continue;
			TAILQ_FOREACH(kpl, &map->placements, entry) {
				if (ks->placement_id != 0 &&
				    kpl->kitty_id != ks->placement_id)
					continue;
				removed += image_grid_clear(gd, kpl->image_id);
			}
			if (ks->placement_id == 0)
				removed++;
		}
		removed += kitty_prune_placements(map, s);
		if (!release || removed == 0 || !TAILQ_EMPTY(&map->placements))
			continue;
		TAILQ_REMOVE(&ks->images, map, entry);
		image_free(image_find(map->image_id));
		free(map);
	}
	if (ctx->wp != NULL)
		ctx->wp->flags |= PANE_REDRAW;
}

/* Decode and append one base64-encoded Kitty payload chunk. */
static int
kitty_append_data(struct kitty_state *ks, const u_char *data, size_t size)
{
	u_char	*d;
	size_t	 dsize, allocated, limit;

	limit = options_get_number(global_options, "image-size-limit");
	if (size > limit || ks->size > limit)
		return (-1);

	d = image_base64_decode(data, size, limit, &dsize);
	if (d == NULL)
		return (-1);
	if (dsize == 0) {
		free(d);
		return (0);
	}
	if (dsize > limit - ks->size) {
		free(d);
		return (-1);
	}

	if (ks->size + dsize > ks->allocated) {
		allocated = ks->allocated;
		if (allocated == 0)
			allocated = 4096;
		while (allocated < ks->size + dsize) {
			if (allocated > limit / 2)
				allocated = limit;
			else
				allocated *= 2;
		}
		ks->data = xrealloc(ks->data, allocated);
		ks->allocated = allocated;
	}
	memcpy(ks->data + ks->size, d, dsize);
	ks->size += dsize;

	free(d);
	return (0);
}

/* Reply to a Kitty graphics sequence. */
static void
kitty_send_reply(struct input_ctx *ictx, struct kitty_state *ks, const char *m)
{
	char	i[96] = "";
	char	I[96] = "";
	char	p[96] = "";
	char	r[96] = "";

	if (ks->quiet >= 2)
		return;
	if ((ks->action == 'd' || ks->action == 'a') && strcmp(m, "OK") == 0)
		return;
	if (ks->quiet == 1 && strcmp(m, "OK") == 0)
		return;
	if (ks->image_id == 0 && ks->image_number == 0 && ks->action != 'q')
		return;

	if (ks->image_id != 0 || ks->image_number == 0)
		xsnprintf(i, sizeof i, "i=%u", ks->image_id);
	if (ks->image_number != 0) {
		if (*i != '\0')
			strlcat(i, ",", sizeof i);
		xsnprintf(I, sizeof I, "I=%u", ks->image_number);
	}
	if (ks->placement_id != 0)
		xsnprintf(p, sizeof p, ",p=%u", ks->placement_id);
	if (ks->action == 'f' && ks->rows != 0)
		xsnprintf(r, sizeof r, ",r=%u", ks->rows);
	input_reply(ictx, 0, "\033_G%s%s%s%s;%s\033\\", i, I, p, r, m);
}

/* Create a Kitty placement with shared source pixels and stored geometry. */
static struct image *
kitty_place_image(struct image *src, struct kitty_state *ks, u_int xpixel,
    u_int ypixel)
{
	struct image_geom	*sg = image_get_geometry(src);
	enum image_type		 type = image_get_type(src);
	struct image_geom	 ig = { 0 };
	u_char			*pixels;
	size_t			 stride, limit;
	uint64_t		 cw, ch;
	double			 size;

	ig.z = ks->z;
	ig.src_x = ks->source_x;
	ig.src_y = ks->source_y;

	/* Clip the source rectangle. */
	if (ig.src_x >= sg->src_w || ig.src_y >= sg->src_h)
		return (NULL);
	ig.src_w = sg->src_w - ig.src_x;
	if (ks->source_width != 0 && ig.src_w > ks->source_width)
		ig.src_w = ks->source_width;
	ig.src_h = sg->src_h - ig.src_y;
	if (ks->source_height != 0 && ig.src_h > ks->source_height)
		ig.src_h = ks->source_height;

	/* Clamp offsets to the starting cell. */
	if (xpixel == 0)
		xpixel = 8;
	if (ypixel == 0)
		ypixel = 16;
	ig.canvas_xoff = ks->x_offset;
	if (ig.canvas_xoff >= xpixel)
		ig.canvas_xoff = xpixel - 1;
	ig.canvas_yoff = ks->y_offset;
	if (ig.canvas_yoff >= ypixel)
		ig.canvas_yoff = ypixel - 1;

	/* Work out the size. */
	ig.sx = ks->columns;
	ig.sy = ks->rows;
	ig.scaled_width = ig.src_w;
	ig.scaled_height = ig.src_h;
	if (ig.sx == 0 && ig.sy == 0) {
		if (ig.canvas_xoff > UINT_MAX - ig.src_w ||
		    ig.canvas_yoff > UINT_MAX - ig.src_h)
			return (NULL);
		image_size_in_cells(ig.src_w + ig.canvas_xoff,
		    ig.src_h + ig.canvas_yoff, xpixel, ypixel, &ig.sx, &ig.sy);
	} else if (ig.sy == 0) {
		ig.scaled_width = (double)ig.sx * xpixel - ig.canvas_xoff;
		ig.scaled_height = ig.src_h * ig.scaled_width / ig.src_w;

		size = ceil((ig.scaled_height + ig.canvas_yoff) / ypixel);
		if (size > USHRT_MAX)
			return (NULL);
		ig.sy = size;
	} else if (ig.sx == 0) {
		ig.scaled_height = (double)ig.sy * ypixel - ig.canvas_yoff;
		ig.scaled_width = ig.src_w * ig.scaled_height / ig.src_h;

		size = ceil((ig.scaled_width + ig.canvas_xoff) / xpixel);
		if (size > USHRT_MAX)
			return (NULL);
		ig.sx = size;
	} else {
		ig.scaled_width = (double)ig.sx * xpixel - ig.canvas_xoff;
		ig.scaled_height = (double)ig.sy * ypixel - ig.canvas_yoff;
	}
	limit = options_get_number(global_options, "image-size-limit");
	if (ig.sx > USHRT_MAX ||
	    ig.sy > USHRT_MAX ||
	    ig.sx * ig.sy > limit)
		return (NULL);

	/* Record geometry in the input terminal's pixel coordinates. */
	cw = (uint64_t)ig.sx * xpixel;
	ch = (uint64_t)ig.sy * ypixel;
	if (cw > UINT_MAX || ch > UINT_MAX)
		return (NULL);
	ig.canvas_width = cw;
	ig.canvas_height = ch;
	pixels = image_get_pixels(src, &stride, NULL);
	return (image_create_with_parent(type, src, &ig, stride, pixels));
}

static const char *
kitty_read_shm(struct kitty_state *ks, struct window_pane *wp,
    size_t end, void **mapping, size_t *maplen)
{
	char		 name[32];
	struct stat	 sb;
	size_t		 size, raw = 0, limit, offset = ks->data_offset;
	int		 fd;
#ifdef __APPLE__
	void		*base;
	size_t		 delta;
#else
	ssize_t		 n;
	size_t		 done;
#endif

	if (ks->size == 0 || ks->size > 31 ||
	    memchr(ks->data, '\0', ks->size) != NULL ||
	    memchr(ks->data + (ks->data[0] == '/'), '/',
	    ks->size - (ks->data[0] == '/')) != NULL)
		return ("EINVAL");
	memcpy(name, ks->data, ks->size);
	name[ks->size] = '\0';
	fd = shm_open(name, O_RDONLY, 0);
	if (fd == -1)
		return ("EBADF");
	image_shm_cancel(name);
	if (control_image_guards(wp, end, NULL, 1) == 0)
		shm_unlink(name);
	else
		control_image_guards(wp, end, image_shm_new(name), 1);
	if (fstat(fd, &sb) != 0 || sb.st_size <= 0 ||
	    (uint64_t)sb.st_size > SIZE_MAX || offset >= (uint64_t)sb.st_size)
		goto bad;
	limit = options_get_number(global_options, "image-size-limit");
	if (ks->format == 24 || ks->format == 32) {
		raw = image_raw_size(ks->format, ks->width, ks->height, limit);
		if (raw == 0)
			goto bad;
	}
	size = sb.st_size - offset;
	if (ks->data_size != 0)
		size = ks->data_size;
	else if (ks->compression != 'z' && raw != 0)
		size = raw;
	if (size == 0 || size > limit || size > (uint64_t)sb.st_size - offset)
		goto bad;
	if (ks->compression == 'o' && raw != 0 && size != raw)
		goto bad;
#ifdef __APPLE__
	delta = offset % getpagesize();
	base = mmap(NULL, size + delta, PROT_READ, MAP_SHARED, fd, offset - delta);
	if (base == MAP_FAILED)
		goto bad;
	free(ks->data);
	if (ks->format == 32 && ks->compression == 'o') {
		*mapping = base;
		*maplen = size + delta;
		ks->data = (u_char *)base + delta;
	} else {
		ks->data = xmalloc(size);
		memcpy(ks->data, (u_char *)base + delta, size);
		munmap(base, size + delta);
	}
#else
	*mapping = NULL;
	*maplen = 0;
	free(ks->data);
	ks->data = xmalloc(size);
	for (done = 0; done < size; done += n) {
		n = pread(fd, ks->data + done, size - done, offset + done);
		if (n == -1 && errno == EINTR) {
			n = 0;
			continue;
		}
		if (n <= 0)
			goto bad;
	}
#endif
	close(fd);
	ks->size = size;
	return ("OK");

bad:
	close(fd);
	return ("EBADF");
}

static const char *
kitty_frame_error(void)
{
	switch (errno) {
	case ENOENT:
		return ("ENOENT");
	case ENOSPC:
		return ("ENOSPC");
	default:
		return ("EINVAL");
	}
}

/* Parse and reply to a Kitty graphics sequence. */
void
kitty_parse_input(struct input_ctx *ictx, struct window_pane *wp,
    struct screen_write_ctx *ctx, u_int bg, void **state, const u_char *buf,
    size_t len, size_t end)
{
	struct window		*w = wp->window;
	struct kitty_state	*ks = *state;
	struct kitty_state	 cks = { .action = 't' };
	struct image_geom	 ig = { 0 };
	struct image_payload	 payload;
	struct kitty_image_map	*map = NULL;
	const u_char		*found;
	const char		*m;
	u_char			*p = NULL;
	size_t			 csize, psize, limit, raw;
	u_int			 sx, sy;
	uint64_t		 cw, ch;
	struct image		*src = NULL, *im = NULL;
	int			 error;
	void			*mapping = NULL;
	size_t			 maplen = 0;
	char			*keys;

	buf++;
	len--;

	if (ks == NULL) {
		ks = xcalloc(1, sizeof *ks);
		TAILQ_INIT(&ks->images);
		TAILQ_INSERT_TAIL(&kitty_states, ks, entry);
		*state = ks;
	}

	found = memchr(buf, ';', len);
	if (found == NULL) {
		csize = len;
		psize = 0;
	} else {
		csize = found - buf;
		psize = len - csize - 1;
	}

	if (ks->more) {
		kitty_parse_control_data(&cks, buf, csize, &m);
		if (cks.action != 'd') {
			ks->more = cks.more;
			if (cks.quiet != 0)
				ks->quiet = cks.quiet;
			if (m != NULL)
				goto out;
			goto parsed;
		}
		free(ks->data);
		ks->data = NULL;
		ks->size = 0;
		ks->allocated = 0;
		ks->more = 0;
	}

	if (!ks->more) {
		ks->action = 't';
		ks->format = 32;
		ks->medium = 'd';
		ks->compression = 'o';
		ks->delete = 'a';
		ks->width = 0;
		ks->height = 0;
		ks->source_x = 0;
		ks->source_y = 0;
		ks->source_width = 0;
		ks->source_height = 0;
		ks->x_offset = 0;
		ks->y_offset = 0;
		ks->columns = 0;
		ks->rows = 0;
		ks->image_id = 0;
		ks->image_number = 0;
		ks->placement_id = 0;
		ks->quiet = 0;
		ks->data_size = 0;
		ks->data_offset = 0;
		ks->no_cursor = 0;
		ks->z = 0;
	}
	ks->more = 0;

	error = kitty_parse_control_data(ks, buf, csize, &m);
	if (error != 0)
		goto out;
parsed:
	m = "EINVAL";
	if (ks->image_id == 0 && ks->image_number == 0)
		ks->placement_id = 0;
	if (ks->image_number != 0) {
		if (ks->image_id != 0)
			goto out;
		m = "ENOTSUP";
		goto out;
	}
	if ((ks->medium != 'd' && ks->medium != 's') ||
	    (ks->medium == 's' && ks->action != 't' &&
	    ks->action != 'T' && ks->action != 'q' && ks->action != 'f')) {
		m = "ENOTSUP";
		goto out;
	}
	if (ks->action != 't' && ks->action != 'T' &&
	    ks->action != 'q' && ks->action != 'p' && ks->action != 'd' &&
	    ks->action != 'f' && ks->action != 'a' && ks->action != 'c') {
		m = "ENOTSUP";
		goto out;
	}
	if (ks->action == 'a' || ks->action == 'c' ||
	    (ks->action == 'd' && (ks->delete == 'f' || ks->delete == 'F'))) {
		enum image_animation_state animation_state = 0;
		u_int count;

		if (ks->image_id == 0) {
			if (ks->action == 'd')
				m = "OK";
			goto out;
		}
		map = kitty_image_map_find(ks, ks->image_id);
		if (map == NULL || (src = image_find(map->image_id)) == NULL) {
			m = ks->action == 'd' ? "OK" : "ENOENT";
			goto out;
		}
		image_ref(src);
		count = image_frame_count(src);
		if (ks->action == 'a') {
			if (ks->width >= IMAGE_STOPPED && ks->width <= IMAGE_RUNNING)
				animation_state = ks->width;
			image_frame_control(src, ks->rows, ks->z, ks->columns,
			    animation_state, ks->height);
		} else if (ks->action == 'c') {
			if (image_frame_compose(src, ks->rows, ks->columns,
			    ks->source_x, ks->source_y, ks->source_width,
			    ks->source_height, ks->x_offset, ks->y_offset,
			    ks->no_cursor) != 0) {
				m = kitty_frame_error();
				goto out;
			}
		} else if (count > 1)
			image_frame_delete(src, ks->rows);
		else if (ks->delete == 'F') {
			ks->delete = 'I';
			kitty_delete_images(ks, ctx);
		}
		wp->flags |= PANE_REDRAW;
		m = "OK";
		goto out;
	}
	if (ks->action == 'd') {
		switch (ks->delete) {
		case 'a':
		case 'A':
		case 'i':
		case 'I':
			break;
		default:
			m = "ENOTSUP";
			goto out;
		}
		kitty_delete_images(ks, ctx);
		m = "OK";
		goto out;
	}
	if (psize != 0 && kitty_append_data(ks, found + 1, psize) != 0)
		goto out;
	if (ks->more)
		return;

	if (ks->action == 'p') {
		map = kitty_image_map_find(ks, ks->image_id);
		if (map == NULL) {
			m = "ENOENT";
			goto out;
		}
		src = image_find(map->image_id);
		image_ref(src);

		im = kitty_place_image(src, ks, w->xpixel, w->ypixel);
		if (im == NULL)
			goto out;
		goto place;
	}

	if (ks->action == 'f') {
		if (ks->image_id == 0)
			goto out;
		map = kitty_image_map_find(ks, ks->image_id);
		if (map == NULL || (src = image_find(map->image_id)) == NULL) {
			m = "ENOENT";
			goto out;
		}
		image_ref(src);
	}

	if (ks->compression != 'o' && ks->compression != 'z')
		goto out;
	if (ks->medium == 's') {
		m = kitty_read_shm(ks, wp, end, &mapping, &maplen);
		if (strcmp(m, "OK") != 0)
			goto out;
		m = "EINVAL";
	}
	limit = options_get_number(global_options, "image-size-limit");
	payload = (struct image_payload) { ks->data, ks->size,
	    ks->format, ks->width, ks->height, ks->compression };
	switch (ks->format) {
	case 24:
	case 32:
		raw = image_raw_size(ks->format, ks->width, ks->height, limit);
		if (mapping != NULL || (ks->format == 32 && ks->compression == 'o' &&
		    raw != 0 && raw == ks->size)) {
			p = ks->data;
			ks->data = NULL;
		} else
			p = image_raw_decode(ks->data, ks->size, limit,
			    ks->format, ks->width, ks->height, ks->compression == 'z');
		break;
	case 100:
		p = image_png_decode(ks->data, ks->size, limit,
		    ks->compression == 'z', ks->medium == 's' ? SIZE_MAX :
		    ks->data_size, &ks->width, &ks->height);
		break;
	}
	if (p == NULL)
		goto out;

	if (ks->action == 'f') {
		struct image_data *data = NULL;
		struct image_geom *geometry = image_get_geometry(src);
		int mapped_pixels = mapping != NULL;

		if (ks->width > geometry->src_w || ks->height > geometry->src_h)
			goto frame_done;
		if (ks->medium == 's' && control_image_guards(wp, end, NULL, 0) != 0) {
			data = image_data_create(src, &payload, mapping, maplen);
			if (data == NULL) {
				m = "ENOSPC";
				goto frame_done;
			}
			mapping = NULL;
			ks->data = NULL;
		}
		error = image_frame_edit(src, &ks->rows, ks->columns, ks->y_offset,
		    p, ks->width, ks->height, ks->source_x, ks->source_y,
		    ks->x_offset == 1, ks->z);
		if (error != 0) {
			m = kitty_frame_error();
			goto frame_done;
		}
		wp->flags |= PANE_REDRAW;
		m = "OK";
		if (data != NULL) {
			keys = kitty_state_keys(ks, &payload, 2);
			control_write_image(wp, end, data, keys);
			free(keys);
		}
frame_done:
		if (data != NULL && p == payload.data)
			p = NULL;
		image_data_free(data);
		if (!mapped_pixels)
			free(p);
		goto out;
	}

	cw = (w->xpixel == 0 ? 8 : w->xpixel);
	ch = (w->ypixel == 0 ? 16 : w->ypixel);
	image_size_in_cells(ks->width, ks->height, cw, ch, &sx, &sy);
	cw *= sx;
	ch *= sy;
	if (cw > UINT_MAX || ch > UINT_MAX) {
		if (mapping == NULL)
			free(p);
		goto out;
	}
	ig.src_w = ig.scaled_width = ks->width;
	ig.src_h = ig.scaled_height = ks->height;
	ig.canvas_width = cw;
	ig.canvas_height = ch;
	ig.sx = sx;
	ig.sy = sy;
	src = image_create(IMAGE_TYPE_KITTY, &ig, p);
	if (src == NULL) {
		m = errno == ENOSPC ? "ENOSPC" : "EINVAL";
		if (mapping == NULL)
			free(p);
		goto out;
	}

	if (ks->data != NULL && ks->size != 0)
		payload.data = xrealloc(ks->data, ks->size);
	error = image_set_data(src, &payload, mapping, maplen);
	mapping = NULL;
	ks->data = NULL;
	if (error != 0) {
		m = "ENOSPC";
		goto out;
	}
	ks->size = ks->allocated = 0;

	if (ks->action == 'T') {
		im = kitty_place_image(src, ks, w->xpixel, w->ypixel);
		if (im == NULL)
			goto out;
	}
	if (ks->action != 'q')
		map = kitty_image_map_set(ks, ctx, ks->image_id, src);
	if (ks->medium == 's' && ks->action != 'q') {
		keys = kitty_state_keys(ks, &payload, 2);
		control_write_image(wp, end, image_get_data(src), keys);
		free(keys);
	}

place:
	if (im != NULL) {
		image_set_order_id(im, image_get_order_id(src));
		kitty_placement_map_set(ctx, map, ks->placement_id, im);
		image_write(ctx, im, bg, ks->no_cursor);
	}
	m = "OK";

out:
	kitty_send_reply(ictx, ks, m);
	if (im != NULL)
		image_free(im);
	if (src != NULL)
		image_free(src);

	if (mapping != NULL)
		munmap(mapping, maplen);
	else
		free(ks->data);
	ks->data = NULL;
	ks->size = 0;
	ks->allocated = 0;
	ks->more = 0;
}
