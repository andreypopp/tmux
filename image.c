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

/* Free an image. */
void
image_free(struct image *im)
{
	if (im->references == 0)
		fatalx("invalid image reference %u", im->id);
	if (--im->references == 0) {
		image_unstore(im);
		if (im->parent == NULL) {
			if (im->data == NULL || im->pixels != im->data->payload.data) {
				image_size -= im->size;
				free(im->pixels);
			}
			image_data_free(im->data);
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

struct image_payload *
image_get_payload(struct image *im)
{
	return (im->data == NULL ? &im->edited : &im->data->payload);
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
	return (im->parent == NULL ? im->gen : im->parent->gen);
}

int
image_edit(struct image *im, const u_char *pixels, u_int w, u_int h,
    u_int x, u_int y, int overwrite, int gap)
{
	struct image *child;
	u_char *copy, *dst;
	const u_char *src;
	u_int xx, yy, k, width, height;
	size_t released = 0;
	uint32_t a, b, alpha, value;
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
	im->edited = (struct image_payload) { im->pixels, im->size,
	    32, im->geom.src_w, im->geom.src_h, 'o' };
	width = x < im->geom.src_w ? im->geom.src_w - x : 0;
	height = y < im->geom.src_h ? im->geom.src_h - y : 0;
	if (width > w)
		width = w;
	if (height > h)
		height = h;
	for (yy = 0; width != 0 && yy < height; yy++) {
		dst = im->pixels + (size_t)(y + yy) * im->stride + (size_t)x * 4;
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
	if (gap != 0)
		im->root_gap = gap > 0 ? gap : 0;
	im->gen++;
	return (0);
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
	if (stride != NULL)
		*stride = im->stride;
	if (size != NULL)
		*size = im->size;
	return (im->pixels + im->geom.src_y * im->stride + im->geom.src_x * 4);
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
