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

#ifndef TMUX_IMAGE_H
#define TMUX_IMAGE_H

#ifdef ENABLE_IMAGES

struct grid;
struct grid_cell;
struct grid_line;
struct image;
struct image_backend;
struct image_line;
struct image_saved;
struct image_store;
struct input_ctx;
struct screen;
struct screen_write_ctx;
struct sixel_image;
struct tty;
struct window_pane;

/* Backend draws images as graphics. */
#define IMAGE_BACKEND_GRAPHICAL 0x1

/* Backend images move with terminal scrolling. */
#define IMAGE_BACKEND_SCROLLS 0x2

/* Type of image. */
enum image_type {
	IMAGE_TYPE_SIXEL,
	IMAGE_TYPE_KITTY
};

/* Source image and placement geometry. */
struct image_geom {
	int	 z;
	/* Position and size within the source RGBA pixels. */
	u_int	 src_x;
	u_int	 src_y;
	u_int	 src_w;
	u_int	 src_h;

	/* Actual image size in pixels, including padding. */
	u_int	 canvas_width;
	u_int	 canvas_height;

	/* Content offset within the canvas, in pixels. */
	u_int	 canvas_xoff;
	u_int	 canvas_yoff;

	/* Scaled source size in pixels, excluding padding. */
	double	 scaled_width;
	double	 scaled_height;

	/* Actual image size in cells, including padding. */
	u_int	 sx;
	u_int	 sy;
};

/* Bounds within a source image, in pixels. */
struct image_bounds {
	double	 left;
	double	 top;
	double	 right;
	double	 bottom;
	double	 area;
};

/* A part of an image to draw. */
struct image_rect {
	struct image	*image;

	/* Size in cells. */
	u_int		 sx;
	u_int		 sy;

	/* Source in image in cells. */
	u_int		 src_x;
	u_int		 src_y;

	/* Destination on terminal in cells. */
	u_int		 dst_x;
	u_int		 dst_y;
};

struct image_payload {
	u_char	*data;
	size_t	 size;
	u_int	 format, width, height;
	char	 compression;
};

/* image.c */
struct image	*image_create(enum image_type, struct image_geom *, u_char *);
struct image	*image_create_with_parent(enum image_type, struct image *,
		     struct image_geom *, size_t, u_char *);
struct image	*image_find(u_int);
struct image	*image_get_parent(struct image *);
struct image_payload *image_get_payload(struct image *);
int		 image_set_data(struct image *, struct image_payload *, void *, size_t);
struct image_data;
struct image_data *image_get_data(struct image *);
struct image_data *image_data_create(struct image *, struct image_payload *,
		     void *, size_t);
void		 image_data_ref(struct image_data *);
struct image_payload *image_data_payload(struct image_data *);
void		 image_data_free(struct image_data *);
enum image_animation_state {
	IMAGE_STOPPED = 1,
	IMAGE_LOADING = 2,
	IMAGE_RUNNING = 3
};
struct image_frame_state {
	u_int current, loops;
	enum image_animation_state state;
	int root_gap;
};
int		 image_frame_restore(struct image *, struct image_frame_state *);
void		 image_frame_control(struct image *, u_int, int, u_int,
		     enum image_animation_state, u_int);
void		 image_frames_now(void);
u_int		 image_frame_count(struct image *);
u_int		 image_frame_generation(struct image *, u_int);
int		 image_frame_gap(struct image *, u_int);
void		 image_frame_payload(struct image *, u_int, struct image_payload *);
int		 image_frame_now(struct image *);
int		 image_frame_edit(struct image *, u_int *, u_int, uint32_t,
		     const u_char *, u_int, u_int, u_int, u_int, int, int);
void		 image_frame_delete(struct image *, u_int);
int		 image_frame_compose(struct image *, u_int, u_int, u_int, u_int,
		     u_int, u_int, u_int, u_int, int);
u_int		 image_get_generation(struct image *);
#define IMAGE_CHUNK_MAX (512 + 4096 + 3)
void		 image_transmit_chunk(struct image_data *, const char *, size_t *,
		     char *, size_t *);
struct image_shm;
void		 image_shm_ref(struct image_shm *);
void		 image_shm_unref(struct image_shm *);
u_int		 image_get_id(struct image *);
u_int		 image_get_order_id(struct image *);
void		 image_set_order_id(struct image *, u_int);
enum image_type	 image_get_type(struct image *);
void		 image_ref(struct image *);
void		 image_free(struct image *);
void		 image_touch(struct image *);
struct image_geom *image_get_geometry(struct image *);
u_char		*image_get_pixels(struct image *, size_t *, size_t *);
int		 image_get_bounds(struct image_geom *, u_int, u_int, u_int,
		     u_int, u_int, u_int, struct image_bounds *);
u_char		*image_get_pixel(struct image *, u_int, u_int, u_int, u_int);
void		 image_size_in_cells(u_int, u_int, u_int, u_int, u_int *,
		     u_int *);
void		 image_get_pixel_area(struct image *, u_int, u_int, u_int,
		     u_int, u_int *, u_int *, u_int *, u_int *);

/* image-decode.c */
u_char		*image_base64_decode(const char *, size_t, size_t, size_t *);
u_char		*image_png_decode(const u_char *, size_t, size_t, int, size_t,
		     u_int *, u_int *);
size_t		 image_raw_size(u_int, u_int, u_int, size_t);
u_char		*image_raw_decode(const u_char *, size_t, size_t, u_int, u_int,
		     u_int, int);

/* image-grid.c */
struct image_rect *image_grid_capture_line(struct grid *, u_int,
		     u_int *);
void		 image_grid_add(struct grid *, struct image *, u_int, u_int,
		     u_int, u_int, u_int, u_int);
void		 image_grid_overwrite(struct grid *, u_int, u_int, u_int,
		     u_int);
void		 image_grid_free_line(struct grid *, struct grid_line *);
void		 image_grid_free(struct grid *);
void		 image_grid_remove_all(struct image *);
void		 image_grid_move_cells(struct grid *, u_int, u_int, u_int,
		     u_int);
struct image_saved *image_grid_save_lines(struct grid *, u_int, u_int, u_int,
		     int);
void		 image_grid_restore_lines(struct grid *, struct image_saved *);
void		 image_grid_duplicate_lines(struct grid *, u_int, struct grid *,
		     u_int, u_int);
void		 image_grid_copy_area(struct grid *, u_int, u_int,
		     struct grid *, u_int, u_int, u_int, u_int);
void		 image_grid_resize_width(struct grid *, u_int);
int		 image_grid_line_has_images(struct grid_line *);
int		 image_grid_has_image(struct grid *, u_int);
u_int		 image_grid_clear(struct grid *, u_int);
u_int		 image_grid_clear_visible(struct grid *, enum image_type);
int		 image_grid_area_has_images(struct grid *, u_int, u_int, u_int,
		     u_int);
void		 image_redraw_area(struct screen_write_ctx *, u_int, u_int,
		     u_int, u_int);
void		 image_redraw_all(struct screen_write_ctx *);
void		 image_redraw_scroll(struct screen_write_ctx *);
void		 image_damage_screen(struct screen_write_ctx *, u_int, u_int,
		     u_int, u_int);
void		 image_write(struct screen_write_ctx *, struct image *, u_int,
		     int);
void		 image_clear(struct screen_write_ctx *, u_int);
int		 image_grid_get_at(struct grid *, u_int, u_int, struct image **,
		     u_int *, u_int *);
const struct grid_cell *image_map_cell(struct tty *, struct grid *, u_int,
		     u_int, const struct grid_cell *, struct grid_cell *, int *);
void		 image_draw_line(struct tty *, struct screen *, u_int, u_int,
		     u_int, u_int, u_int);
void		 image_redraw_start(struct tty *, u_int, u_int, u_int, u_int);
void		 image_draw_flush(struct tty *);
void		 image_redraw_finish(struct tty *);
void		 image_scroll(struct tty *, u_int, u_int, u_int, u_int, int);
int		 image_tty_update(struct tty *);
int		 image_backend_get_flags(struct tty *);
void		 image_tty_size_changed(struct tty *);
void		 image_tty_free(struct tty *, int);

/* image-fallback.c */
int		 fallback_select(struct tty *);
int		 fallback_get_flags(struct tty *);
const struct grid_cell *fallback_map_cell(struct tty *, struct grid *, u_int,
		     u_int, const struct grid_cell *, struct grid_cell *, int *);

/* image-sixel.c */
#define SIXEL_COLOUR_REGISTERS 1024
int		 sixel_select(struct tty *);
int		 sixel_get_flags(struct tty *);
struct sixel_image *sixel_parse(const char *, size_t, u_int, u_int, u_int);
void		 sixel_write(struct screen_write_ctx *, struct sixel_image *,
		     u_int);
void		 sixel_draw_rect(struct tty *, struct image_rect *);
void		 sixel_start_draw(struct tty *, u_int, u_int, u_int, u_int);
void		 sixel_free_pending(struct tty *, int);
void		 sixel_flush_pending(struct tty *);

/* image-kitty.c */
int		 kitty_select(struct tty *);
void		 kitty_parse_input(struct input_ctx *, struct window_pane *,
		     struct screen_write_ctx *, u_int, void **, const u_char *,
		     size_t, size_t);
void		 kitty_free_state(void *);
void		 kitty_remove_image(struct image *);
struct image_capture {
	struct image_rect rect;
	u_int		 id;
};
struct image_capture *image_capture_select(void *, struct grid *, u_int, u_int,
		     u_int *);
char		*image_capture_line(void *, struct grid *, struct screen *,
		     u_int, struct image_capture *, u_int, struct grid_cell **, int);
char		*image_capture_transmit(struct window_pane *, struct image_capture *, u_int,
		     const char *, size_t *, int);
char		*image_capture_pending(void *);
void		*input_kitty_state(struct input_ctx *);

#endif /* ENABLE_IMAGES */

#endif /* TMUX_IMAGE_H */
