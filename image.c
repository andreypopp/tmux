/* $OpenBSD$ */

/*
 * Copyright (c) 2007 Nicholas Marriott <nicholas.marriott@gmail.com>
 * Copyright (c) 2026 Michael Grant <mgrant@grant.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF MIND, USE, DATA OR PROFITS, WHETHER
 * IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING
 * OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/mman.h>

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "tmux.h"

struct image_animation {
	u_int current, max_loops, loops;
	enum image_animation_state state;
	uint64_t shown_at;
};

struct image_frame {
	u_char *pixels;
	int gap;
	u_int gen;
};

struct image_data {
	struct image_payload payload;
	void *base;
	size_t length;
	u_int references;
};

/* An image. */
TAILQ_HEAD(image_list, image);
struct image {
	u_int			 id;
	enum image_type		 type;
	u_int			 references;
	int			 stored;
	struct image_geom	 geom;

	struct image		*parent;
	struct image_list	 children;
	u_int			 order_id;

	/*
	 * RGBA pixels use four bytes each. Stride is the number of bytes
	 * between the start of each row. Size is the number of bytes
	 * containing all of this image's pixels - it may include pixels that
	 * are part of the parent image but not part of this image.
	 */
	size_t			 stride;
	size_t			 size;
	u_char			*pixels;
	struct image_data	*data;
	struct image_payload	 edited;
	struct image_frame	*frames;
	u_int			 nframes, gen;
	int			 root_gap;
	struct image_animation	 animation;
	u_int			 next_gen;

	RB_ENTRY(image)		 entry;
	TAILQ_ENTRY(image)	 all_entry;
	TAILQ_ENTRY(image)	 child_entry;
};
RB_HEAD(images, image);
static struct images images = RB_INITIALIZER(&images);

/* All images, most recently used first. */
static struct image_list all_images = TAILQ_HEAD_INITIALIZER(all_images);

/* Number of images and size. */
static u_int image_count;
static size_t image_size;

static void image_compose(u_char *, u_int, u_int, const u_char *, u_int,
    u_int, u_int, u_int, int);
static void image_frame_reset(struct image *);
static u_char *image_frame_pixels(struct image *, u_int);

/* Next image ID. */
static u_int image_next_id;

/* Compare images by IDs. */
static int
image_cmp(struct image *a, struct image *b)
{
	if (a->id < b->id)
		return (-1);
	if (a->id > b->id)
		return (1);
	return (0);
}
RB_GENERATE_STATIC(images, image, entry, image_cmp);

/* Record use of an image and its parents. */
void
image_touch(struct image *im)
{
	for (; im != NULL; im = im->parent) {
		if (im->stored && TAILQ_FIRST(&all_images) != im) {
			TAILQ_REMOVE(&all_images, im, all_entry);
			TAILQ_INSERT_HEAD(&all_images, im, all_entry);
		}
	}
}

/* Find an image by its ID. */
struct image *
image_find(u_int id)
{
	struct image	find;

	find.id = id;
	return (RB_FIND(images, &images, &find));
}

/* Get the ID of an image. */
u_int
image_get_id(struct image *im)
{
	return (im->id);
}

/* Get order ID of an image. */
u_int
image_get_order_id(struct image *im)
{
	return (im->order_id);
}

/* Set the order ID on an image. */
void
image_set_order_id(struct image *im, u_int id)
{
	im->order_id = id;
}

/* Get the type of an image. */
enum image_type
image_get_type(struct image *im)
{
	return (im->type);
}

static void
image_unstore(struct image *im)
{
	if (!im->stored)
		return;
	RB_REMOVE(images, &images, im);
	TAILQ_REMOVE(&all_images, im, all_entry);
	image_count--;
	im->stored = 0;
}

/* Remove an image and children. */
static void
image_remove_all(struct image *im)
{
	struct image *child, *next;

	image_ref(im);
	TAILQ_FOREACH_SAFE(child, &im->children, child_entry, next)
		image_remove_all(child);
	image_grid_remove_all(im);
	image_unstore(im);
	image_free(im);
}

/* Find the oldest image. */
static struct image *
image_find_oldest(struct image *keep, size_t size)
{
	struct image	*im, *parent;
	size_t		 limit;

	limit = options_get_number(global_options, "image-total-size-limit");
	TAILQ_FOREACH_REVERSE(im, &all_images, image_list, all_entry) {
		if (im->parent != NULL &&
		    (image_size > limit || size > limit - image_size))
			continue;
		for (parent = keep; parent != NULL; parent = parent->parent) {
			if (parent == im)
				break;
		}
		if (parent == NULL)
			return (im);
	}
	return (NULL);
}

/* Is there enough space for this image? */
static int
image_can_add(size_t size, u_int add)
{
	u_int	count;
	size_t	limit;

	count = options_get_number(global_options, "image-total-count-limit");
	if ((uint64_t)image_count + add > count)
		return (0);
	limit = options_get_number(global_options, "image-total-size-limit");
	if (image_size > limit || size > limit - image_size)
		return (0);
	return (1);
}

/* Check limits and remove images if needed. */
static int
image_check_limits(struct image *keep, size_t size, u_int add)
{
	struct image	*oldest;
	struct client	*c;
	size_t		 limit;

	limit = options_get_number(global_options, "image-total-size-limit");
	if (size > limit)
		return (-1);
	if (image_can_add(size, add))
		return (0);

	TAILQ_FOREACH(c, &clients, entry)
		image_tty_free(&c->tty, 1);
	while (!image_can_add(size, add)) {
		oldest = image_find_oldest(keep, size);
		if (oldest == NULL)
			break;
		image_remove_all(oldest);
	}
	TAILQ_FOREACH(c, &clients, entry)
		server_redraw_client(c);
	if (!image_can_add(size, add))
		return (-1);
	return (0);
}

/* Allocate an image and assign a unique ID. */
static struct image *
image_alloc(enum image_type type, struct image_geom *ig, size_t stride,
    u_char *pixels)
{
	struct image	*im;
	u_int		 id;

	do {
		if (++image_next_id == 0)
			image_next_id++;
		id = image_next_id;
	} while (image_find(id) != NULL);

	im = xcalloc(1, sizeof *im);
	im->id = id;
	im->type = type;
	im->references = 1;
	im->stored = 1;
	im->order_id = id;
	im->geom = *ig;
	im->stride = stride;
	im->size = (ig->src_h - 1) * stride + ig->src_w * 4;
	im->pixels = pixels;
	im->animation.state = IMAGE_STOPPED;
	TAILQ_INIT(&im->children);
	RB_INSERT(images, &images, im);
	image_count++;
	return (im);
}

/* Create an image with the provided pixels. */
struct image *
image_create(enum image_type type, struct image_geom *ig, u_char *pixels)
{
	size_t	stride = ig->src_w * 4;

	return (image_create_with_parent(type, NULL, ig, stride, pixels));
}

/* Create an image based on a parent image. */
struct image *
image_create_with_parent(enum image_type type, struct image *parent,
    struct image_geom *ig, size_t stride, u_char *pixels)
{
	struct image	*im;
	size_t		 size = 0, limit;

	errno = EINVAL;
	limit = options_get_number(global_options, "image-size-limit");
	if (ig->src_w == 0 || ig->src_h == 0)
		return (NULL);
	if (ig->canvas_width == 0 || ig->canvas_height == 0)
		return (NULL);
	if (ig->sx == 0 || ig->sy == 0 || pixels == NULL)
		return (NULL);
	if ((uint64_t)ig->src_w * ig->src_h > limit / 4)
		return (NULL);
	if (ig->sx > USHRT_MAX || ig->sy > USHRT_MAX)
		return (NULL);
	if (ig->sx * ig->sy > limit || stride < ig->src_w * 4)
		return (NULL);
	if (ig->scaled_width <= 0 || ig->scaled_height <= 0)
		return (NULL);

	if (parent == NULL)
		size = (ig->src_h - 1) * stride + ig->src_w * 4;
	if (image_check_limits(parent, size, 1) != 0) {
		errno = ENOSPC;
		return (NULL);
	}

	im = image_alloc(type, ig, stride, pixels);
	im->parent = parent;
	if (parent == NULL)
		image_size += im->size;
	else {
		im->order_id = parent->order_id;
		TAILQ_INSERT_TAIL(&parent->children, im, child_entry);
		image_ref(parent);
	}
	TAILQ_INSERT_HEAD(&all_images, im, all_entry);
	image_touch(im);
	return (im);
}

/* Add a reference to an image. */
void
image_ref(struct image *im)
{
	if (im->references == UINT_MAX)
		fatalx("invalid image reference %u", im->id);
	im->references++;
}

static void
image_release_pixels(struct image *im)
{
	if (im->data == NULL || im->pixels != im->data->payload.data) {
		image_size -= im->size;
		free(im->pixels);
	}
	image_data_free(im->data);
	im->data = NULL;
}

/* Free an image. */
void
image_free(struct image *im)
{
	if (im->references == 0)
		fatalx("invalid image reference %u", im->id);
	if (--im->references == 0) {
		image_unstore(im);
		if (im->parent == NULL) {
			image_release_pixels(im);
			for (u_int i = 0; i < im->nframes; i++) {
				image_size -= im->size;
				free(im->frames[i].pixels);
			}
			free(im->frames);
		} else {
			TAILQ_REMOVE(&im->parent->children, im, child_entry);
			image_free(im->parent);
		}
		free(im);
	}
}

struct image *
image_get_parent(struct image *im)
{
	return (im->parent);
}

static void
image_rgba_payload(struct image *im, u_char *pixels, struct image_payload *payload)
{
	*payload = (struct image_payload) { pixels, im->size,
	    32, im->geom.src_w, im->geom.src_h, 'o' };
}

struct image_payload *
image_get_payload(struct image *im)
{
	if (im->data != NULL)
		return (&im->data->payload);
	image_rgba_payload(im, im->pixels, &im->edited);
	return (&im->edited);
}

static struct image_data *
image_data_alloc(struct image_payload *payload, void *base, size_t length)
{
	struct image_data *data = xcalloc(1, sizeof *data);

	data->references = 1;
	data->payload = *payload;
	data->base = base;
	data->length = length;
	image_size += length != 0 ? length : payload->size;
	return (data);
}

int
image_set_data(struct image *im, struct image_payload *payload,
    void *base, size_t length)
{
	int shared = im->pixels == payload->data, error;
	size_t size = length != 0 ? length : payload->size;

	error = image_check_limits(im, shared ? size - im->size : size, 0);
	im->data = image_data_alloc(payload, base, length);
	if (shared)
		image_size -= im->size;
	return (error);
}

struct image_data *
image_get_data(struct image *im)
{
	return (im->data);
}

struct image_data *
image_data_create(struct image *keep, struct image_payload *payload,
    void *base, size_t length)
{
	size_t size = length != 0 ? length : payload->size;

	if (image_check_limits(keep, size, 0) != 0)
		return (NULL);
	return (image_data_alloc(payload, base, length));
}

void
image_data_ref(struct image_data *data)
{
	data->references++;
}

struct image_payload *
image_data_payload(struct image_data *data)
{
	return (&data->payload);
}

void
image_data_free(struct image_data *data)
{
	if (data == NULL || --data->references != 0)
		return;
	image_size -= data->length != 0 ? data->length : data->payload.size;
	if (data->length != 0)
		munmap(data->base, data->length);
	else
		free(data->payload.data);
	free(data);
}

u_int
image_get_generation(struct image *im)
{
	if (im->parent != NULL)
		im = im->parent;
	return (image_frame_generation(im, im->animation.current));
}

static int
image_edit(struct image *im, const u_char *pixels, u_int w, u_int h,
    u_int x, u_int y, int overwrite)
{
	struct image *child;
	u_char *copy;
	size_t released = 0;
	int shared = im->data != NULL && im->pixels == im->data->payload.data;
	int error;

	if (shared && im->data->length == 0 && im->data->references == 1) {
		im->data->payload.data = NULL;
		im->data->payload.size = 0;
		shared = 0;
	}
	if (shared) {
		if (im->data->references == 1)
			released = im->data->length != 0 ? im->data->length : im->data->payload.size;
		image_size -= released;
		error = image_check_limits(im, im->size, 0);
		image_size += released;
		if (error != 0)
			return (-1);
		copy = xmalloc(im->size);
		memcpy(copy, im->pixels, im->size);
		im->pixels = copy;
		TAILQ_FOREACH(child, &im->children, child_entry)
			child->pixels = copy;
		image_size += im->size;
	}
	image_data_free(im->data);
	im->data = NULL;
	image_compose(im->pixels, im->geom.src_w, im->geom.src_h,
	    pixels, w, h, x, y, overwrite);
	im->gen = ++im->next_gen;
	return (0);
}

static void
image_compose(u_char *canvas, u_int cw, u_int ch, const u_char *pixels,
    u_int w, u_int h, u_int x, u_int y, int overwrite)
{
	u_char *dst;
	const u_char *src;
	u_int xx, yy, k, width, height;
	uint32_t a, b, alpha, value;

	width = x < cw ? cw - x : 0;
	height = y < ch ? ch - y : 0;
	if (width > w)
		width = w;
	if (height > h)
		height = h;
	for (yy = 0; width != 0 && yy < height; yy++) {
		dst = canvas + ((size_t)(y + yy) * cw + x) * 4;
		src = pixels + (size_t)yy * w * 4;
		for (xx = 0; xx < width; xx++, dst += 4, src += 4) {
			if (overwrite || dst[3] == 0) {
				memcpy(dst, src, 4);
				continue;
			}
			a = src[3] * 257;
			b = dst[3] * 257;
			alpha = a + b * (65535 - a) / 65535;
			for (k = 0; k < 3; k++) {
				value = dst[k] * 257 * b / 65535;
				value = (src[k] * 257 * a + value * (65535 - a)) / 65535;
				dst[k] = (value * 65535 / alpha) >> 8;
			}
			dst[3] = alpha >> 8;
		}
	}
}

u_int
image_frame_count(struct image *im)
{
	return (im->nframes + 1);
}

static u_char *
image_frame_pixels(struct image *im, u_int frame)
{
	if (frame > im->nframes)
		return (NULL);
	return (frame == 0 ? im->pixels : im->frames[frame - 1].pixels);
}

u_int
image_frame_generation(struct image *im, u_int frame)
{
	return (frame == 0 ? im->gen : im->frames[frame - 1].gen);
}

int
image_frame_gap(struct image *im, u_int frame)
{
	return (frame == 0 ? im->root_gap : im->frames[frame - 1].gap);
}

static void
image_frame_set_gap(struct image *im, u_int frame, int gap)
{
	if (frame == 0)
		im->root_gap = gap > 0 ? gap : 0;
	else
		im->frames[frame - 1].gap = gap > 0 ? gap : 0;
}

void
image_frame_payload(struct image *im, u_int frame, struct image_payload *payload)
{
	if (frame == 0)
		*payload = *image_get_payload(im);
	else
		image_rgba_payload(im, image_frame_pixels(im, frame), payload);
}

static void
image_frame_reset(struct image *im)
{
	im->animation.shown_at = get_timer();
}

int
image_frame_now(struct image *im)
{
	struct image_animation *a;
	uint64_t now, elapsed, duration = 0, cycles;
	u_int i, next, old;

	if (im->parent != NULL)
		im = im->parent;
	a = &im->animation;
	old = a->current;
	if (a->state == IMAGE_STOPPED || im->nframes == 0 ||
	    TAILQ_EMPTY(&im->children) ||
	    (a->max_loops != 0 && a->loops >= a->max_loops))
		return (0);
	for (i = 0; i <= im->nframes; i++)
		duration += image_frame_gap(im, i);
	if (duration == 0)
		return (0);
	now = get_timer();
	if (now < a->shown_at)
		a->shown_at = now;
	elapsed = now - a->shown_at;
	if (a->state == IMAGE_RUNNING && elapsed >= duration) {
		cycles = elapsed / duration;
		if (a->max_loops != 0 && cycles >= a->max_loops - a->loops)
			cycles = a->max_loops - a->loops - 1;
		a->loops += cycles;
		a->shown_at += cycles * duration;
		elapsed -= cycles * duration;
	}
	while (elapsed >= (u_int)image_frame_gap(im, a->current)) {
		next = a->current;
		do {
			next = (next + 1) % (im->nframes + 1);
			if (next == 0) {
				if (a->state == IMAGE_LOADING)
					goto done;
				a->loops++;
				if (a->max_loops != 0 && a->loops >= a->max_loops)
					goto done;
			}
		} while (image_frame_gap(im, next) == 0);
		elapsed -= image_frame_gap(im, a->current);
		a->shown_at += image_frame_gap(im, a->current);
		a->current = next;
	}
 done:
	return (old != a->current);
}

void
image_frames_now(void)
{
	struct image *im;

	TAILQ_FOREACH(im, &all_images, all_entry) {
		if (im->parent == NULL)
			image_frame_now(im);
	}
}

void
image_frame_control(struct image *im, u_int frame, int gap, u_int current,
    enum image_animation_state state, u_int loops)
{
	struct image_animation *a = &im->animation;

	image_frame_now(im);
	if (frame != 0 && frame <= im->nframes + 1 && gap != 0)
		image_frame_set_gap(im, frame - 1, gap);
	if (current != 0 && current <= im->nframes + 1 && current - 1 != a->current) {
		a->current = current - 1;
		image_frame_reset(im);
	}
	if (state != 0) {
		if (a->state == IMAGE_STOPPED && state != IMAGE_STOPPED)
			image_frame_reset(im);
		a->state = state;
		a->loops = 0;
	}
	if (loops != 0)
		a->max_loops = loops - 1;
}

int
image_frame_restore(struct image *im, struct image_frame_state *state)
{
	struct image_animation *a = &im->animation;
	int exhausted = a->max_loops != 0 && a->loops >= a->max_loops;

	*state = (struct image_frame_state) {
	    a->current + 1,
	    a->max_loops == 0 || exhausted ? 1 : a->max_loops - a->loops + 1,
	    exhausted ? IMAGE_STOPPED : a->state, im->root_gap };
	return (im->nframes != 0 || im->root_gap > 0 || a->state != IMAGE_STOPPED);
}

static int
image_frame_write(struct image *im, u_int frame, const u_char *pixels,
    u_int w, u_int h, u_int x, u_int y, int overwrite)
{
	if (frame == 1) {
		if (image_edit(im, pixels, w, h, x, y, overwrite) != 0) {
			errno = ENOSPC;
			return (-1);
		}
	} else {
		image_compose(image_frame_pixels(im, frame - 1), im->geom.src_w,
		    im->geom.src_h, pixels, w, h, x, y, overwrite);
		im->frames[frame - 2].gen = ++im->next_gen;
	}
	return (0);
}

int
image_frame_edit(struct image *im, u_int *number, u_int base, uint32_t bg,
    const u_char *pixels, u_int w, u_int h, u_int x, u_int y,
    int overwrite, int gap)
{
	u_int frame = *number, i;
	u_char *canvas;

	if (frame == 0 || frame > im->nframes + 1)
		frame = im->nframes + 2;
	if (frame == im->nframes + 2) {
		if (base > im->nframes + 1) {
			errno = EINVAL;
			return (-1);
		}
		if (image_check_limits(im, im->size, 0) != 0) {
			errno = ENOSPC;
			return (-1);
		}
		canvas = xmalloc(im->size);
		if (base != 0)
			memcpy(canvas, image_frame_pixels(im, base - 1), im->size);
		else {
			for (i = 0; i < im->size; i += 4) {
				canvas[i] = bg >> 24;
				canvas[i + 1] = bg >> 16;
				canvas[i + 2] = bg >> 8;
				canvas[i + 3] = bg;
			}
		}
		im->frames = xreallocarray(im->frames, im->nframes + 1,
		    sizeof *im->frames);
		im->frames[im->nframes++] = (struct image_frame) {
		    canvas, gap == 0 ? 40 : (gap > 0 ? gap : 0), ++im->next_gen };
		image_size += im->size;
	}
	if (image_frame_write(im, frame, pixels, w, h, x, y, overwrite) != 0)
		return (-1);
	if (gap != 0)
		image_frame_set_gap(im, frame - 1, gap);
	if (frame - 1 == im->animation.current)
		image_frame_reset(im);
	*number = frame;
	return (0);
}

void
image_frame_delete(struct image *im, u_int number)
{
	u_int removed;
	u_char *pixels;
	struct image *child;

	if (im->nframes == 0)
		return;
	if (number == 0)
		number = 1;
	if (number > im->nframes + 1)
		number = im->nframes + 1;
	if (number == 1) {
		pixels = im->frames[0].pixels;
		image_release_pixels(im);
		im->pixels = pixels;
		im->gen = im->frames[0].gen;
		im->root_gap = im->frames[0].gap;
		TAILQ_FOREACH(child, &im->children, child_entry)
			child->pixels = pixels;
		removed = 0;
	} else {
		removed = number - 2;
		free(im->frames[removed].pixels);
		image_size -= im->size;
	}
	memmove(im->frames + removed, im->frames + removed + 1,
	    (im->nframes - removed - 1) * sizeof *im->frames);
	im->nframes--;
	if (im->animation.current > im->nframes) {
		im->animation.current = im->nframes;
		image_frame_reset(im);
	} else if (removed == im->animation.current)
		image_frame_reset(im);
	else if (removed < im->animation.current)
		im->animation.current--;
}

int
image_frame_compose(struct image *im, u_int source, u_int dest,
    u_int x, u_int y, u_int w, u_int h, u_int left, u_int top, int overwrite)
{
	u_char *rect, *pixels = image_frame_pixels(im, source - 1);
	u_int row;
	int result;

	if (source == 0 || dest == 0 || pixels == NULL ||
	    dest > im->nframes + 1) {
		errno = ENOENT;
		return (-1);
	}
	if (w == 0)
		w = im->geom.src_w;
	if (h == 0)
		h = im->geom.src_h;
	if ((uint64_t)x + w > im->geom.src_w ||
	    (uint64_t)y + h > im->geom.src_h ||
	    (uint64_t)left + w > im->geom.src_w ||
	    (uint64_t)top + h > im->geom.src_h ||
	    (source == dest && left < (uint64_t)x + w && x < (uint64_t)left + w &&
	    top < (uint64_t)y + h && y < (uint64_t)top + h)) {
		errno = EINVAL;
		return (-1);
	}
	rect = xmalloc((size_t)w * h * 4);
	for (row = 0; row < h; row++)
		memcpy(rect + (size_t)row * w * 4,
		    pixels + ((size_t)(top + row) * im->geom.src_w + left) * 4,
		    (size_t)w * 4);
	result = image_frame_write(im, dest, rect, w, h, x, y, overwrite);
	free(rect);
	return (result);
}

/* Get the image geometry. */
struct image_geom *
image_get_geometry(struct image *im)
{
	return (&im->geom);
}

/* Get the image pixels. */
u_char *
image_get_pixels(struct image *im, size_t *stride, size_t *size)
{
	struct image *parent = im->parent == NULL ? im : im->parent;
	u_char *pixels;

	if (stride != NULL)
		*stride = im->stride;
	if (size != NULL)
		*size = im->size;

	pixels = image_frame_pixels(parent, parent->animation.current);
	return (pixels + im->geom.src_y * im->stride + im->geom.src_x * 4);
}

/* Convert an output X position to a source image position in pixels. */
static double
image_map_x(struct image_geom *ig, u_int x, u_int w)
{
	double	p;

	p = (double)x * ig->canvas_width;
	p -= (double)ig->canvas_xoff * w;
	p *= ig->src_w;
	return (p / (w * ig->scaled_width));
}

/* Convert an output Y position to a source image position in pixels. */
static double
image_map_y(struct image_geom *ig, u_int y, u_int h)
{
	double	p;

	p = ((double)y * ig->canvas_height) - ((double)ig->canvas_yoff * h);
	return ((p * ig->src_h) / (h * ig->scaled_height));
}

/* Find source image bounds for part of an output. */
int
image_get_bounds(struct image_geom *ig, u_int x, u_int y, u_int w, u_int h,
    u_int sx, u_int sy, struct image_bounds *ib)
{
	if (x >= sx || y >= sy || w == 0 || h == 0)
		return (0);
	if (w > sx - x)
		w = sx - x;
	if (h > sy - y)
		h = sy - y;

	ib->left = image_map_x(ig, x, sx);
	ib->top = image_map_y(ig, y, sy);
	ib->right = image_map_x(ig, x + w, sx);
	ib->bottom = image_map_y(ig, y + h, sy);
	if (ib->left >= ig->src_w ||
	    ib->top >= ig->src_h ||
	    ib->right <= 0 ||
	    ib->bottom <= 0)
		return (0);
	ib->area = (ib->right - ib->left) * (ib->bottom - ib->top);

	if (ib->left < 0)
		ib->left = 0;
	if (ib->top < 0)
		ib->top = 0;
	if (ib->right > ig->src_w)
		ib->right = ig->src_w;
	if (ib->bottom > ig->src_h)
		ib->bottom = ig->src_h;
	return (1);
}

/* Map a pixel in the full output canvas to the original source pixels. */
u_char *
image_get_pixel(struct image *im, u_int x, u_int y, u_int w, u_int h)
{
	static u_char		 empty[4];
	struct image_geom	*ig = &im->geom;
	double			 px, py;
	u_int			 sx, sy;

	px = image_map_x(ig, x, w);
	py = image_map_y(ig, y, h);
	if (px < 0 || py < 0 || px >= ig->src_w || py >= ig->src_h)
		return (empty);
	sx = px;
	sy = py;
	return (image_get_pixels(im, NULL, NULL) + sy * im->stride + sx * 4);
}

/* Calculate the cell size. */
void
image_size_in_cells(u_int w, u_int h, u_int xpixel, u_int ypixel, u_int *sx,
    u_int *sy)
{
	if (xpixel == 0)
		xpixel = 8;
	if (ypixel == 0)
		ypixel = 16;
	*sx = w / xpixel + (w % xpixel != 0);
	*sy = h / ypixel + (h % ypixel != 0);
}

/* Find the source pixels covered by a rectangle of placement cells. */
void
image_get_pixel_area(struct image *im, u_int x, u_int y, u_int w, u_int h,
    u_int *px, u_int *py, u_int *pw, u_int *ph)
{
	struct image_geom	*ig;
	struct image_bounds	 ib;

	*px = *py = *pw = *ph = 0;

	if (im != NULL) {
		ig = &im->geom;
		if (image_get_bounds(ig, x, y, w, h, ig->sx, ig->sy, &ib)) {
			*px = floor(ib.left);
			*py = floor(ib.top);
			*pw = ceil(ib.right) - *px;
			*ph = ceil(ib.bottom) - *py;
		}
	}
}
