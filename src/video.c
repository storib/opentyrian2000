/*
 * OpenTyrian: A modern cross-platform port of Tyrian
 * Copyright (C) 2007-2009  The OpenTyrian Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
 */
#include "video.h"

#include "devscript.h"
#include "keyboard.h"
#include "opentyr.h"
#include "palette.h"
#include "video_scale.h"

#include <assert.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

const char *const scaling_mode_names[ScalingMode_MAX] = {
	"Center",
	"Integer",
	"Fit 8:5",
	"Fit 4:3",
	"Widescreen",
};

// gap between the playfield and the HUD panel when they are split apart, in game pixels
#define hud_panel_gap 8

int fullscreen_display;
ScalingMode scaling_mode = SCALE_WIDESCREEN;
bool video_hud_split = false;
static SDL_Rect last_output_rect = { 0, 0, vga_width, vga_height };

typedef struct
{
	SDL_Rect src;  // in game screen pixels
	SDL_Rect dst;  // in window pixels
} OutputRegion;

SDL_Surface *VGAScreen, *VGAScreenSeg;
SDL_Surface *VGAScreen2;
SDL_Surface *game_screen;

SDL_Window *main_window = NULL;
static SDL_Renderer *main_window_renderer = NULL;
SDL_PixelFormat *main_window_tex_format = NULL;
static SDL_Texture *main_window_texture = NULL;
static SDL_Texture *prescale_texture = NULL;  // render target for sharp scaling
static int prescale_w = 0, prescale_h = 0;

static ScalerFunction scaler_function;

static void init_renderer(void);
static void deinit_renderer(void);
static void init_texture(void);
static void deinit_texture(void);

static int window_get_display_index(void);
static void window_center_in_display(int display_index);
static void calc_dst_render_rect(SDL_Surface *src_surface, SDL_Rect *dst_rect);
static void scale_and_flip(SDL_Surface *);

void init_video(void)
{
	if (SDL_WasInit(SDL_INIT_VIDEO))
		return;

	if (SDL_InitSubSystem(SDL_INIT_VIDEO) == -1)
	{
		fprintf(stderr, "error: failed to initialize SDL video: %s\n", SDL_GetError());
		exit(1);
	}

	// Create the software surfaces that the game renders to. These are all 320x200x8 regardless
	// of the window size or monitor resolution.
	VGAScreen = VGAScreenSeg = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	VGAScreen2 = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);
	game_screen = SDL_CreateRGBSurface(0, vga_width, vga_height, 8, 0, 0, 0, 0);

	// The game code writes to surface->pixels directly without locking, so make sure that we
	// indeed created software surfaces that support this.
	assert(!SDL_MUSTLOCK(VGAScreen));
	assert(!SDL_MUSTLOCK(VGAScreen2));
	assert(!SDL_MUSTLOCK(game_screen));

	JE_clr256(VGAScreen);

	// Create the window with a temporary initial size, hidden until we set up the
	// scaler and find the true window size
	main_window = SDL_CreateWindow(opentyrian_str,
		SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
		vga_width, vga_height, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIDDEN);

	if (main_window == NULL)
	{
		fprintf(stderr, "error: failed to create window: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}

	reinit_fullscreen(fullscreen_display);
	init_renderer();
	init_texture();
	init_scaler(scaler);

	SDL_ShowWindow(main_window);

	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	SDL_RenderPresent(main_window_renderer);
}

void deinit_video(void)
{
	deinit_texture();
	deinit_renderer();

	SDL_DestroyWindow(main_window);

	SDL_FreeSurface(VGAScreenSeg);
	SDL_FreeSurface(VGAScreen2);
	SDL_FreeSurface(game_screen);

	SDL_QuitSubSystem(SDL_INIT_VIDEO);
}

static void init_renderer(void)
{
	main_window_renderer = SDL_CreateRenderer(main_window, -1, 0);

	if (main_window_renderer == NULL)
	{
		fprintf(stderr, "error: failed to create renderer: %s\n", SDL_GetError());
		exit(EXIT_FAILURE);
	}
}

static void deinit_renderer(void)
{
	if (main_window_renderer != NULL)
	{
		SDL_DestroyRenderer(main_window_renderer);
		main_window_renderer = NULL;
	}
}

static void init_texture(void)
{
	assert(main_window_renderer != NULL);

	int bpp = 32; // TODOSDL2
	Uint32 format = bpp == 32 ? SDL_PIXELFORMAT_RGB888 : SDL_PIXELFORMAT_RGB565;
	int scaler_w = scalers[scaler].width;
	int scaler_h = scalers[scaler].height;

	main_window_tex_format = SDL_AllocFormat(format);

	main_window_texture = SDL_CreateTexture(main_window_renderer, format, SDL_TEXTUREACCESS_STREAMING, scaler_w, scaler_h);

	if (main_window_texture == NULL)
	{
		fprintf(stderr, "error: failed to create scaler texture %dx%dx%s: %s\n", scaler_w, scaler_h, SDL_GetPixelFormatName(format), SDL_GetError());
		exit(EXIT_FAILURE);
	}
}

static void deinit_texture(void)
{
	if (prescale_texture != NULL)
	{
		SDL_DestroyTexture(prescale_texture);
		prescale_texture = NULL;
		prescale_w = prescale_h = 0;
	}

	if (main_window_texture != NULL)
	{
		SDL_DestroyTexture(main_window_texture);
		main_window_texture = NULL;
	}

	if (main_window_tex_format != NULL)
	{
		SDL_FreeFormat(main_window_tex_format);
		main_window_tex_format = NULL;
	}
}

static int window_get_display_index(void)
{
	return SDL_GetWindowDisplayIndex(main_window);
}

static void window_center_in_display(int display_index)
{
	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	SDL_Rect bounds;
	SDL_GetDisplayBounds(display_index, &bounds);

	SDL_SetWindowPosition(main_window, bounds.x + (bounds.w - win_w) / 2, bounds.y + (bounds.h - win_h) / 2);
}

void reinit_fullscreen(int new_display)
{
	fullscreen_display = new_display;

	if (fullscreen_display >= SDL_GetNumVideoDisplays())
	{
		fullscreen_display = 0;
	}

	SDL_SetWindowFullscreen(main_window, SDL_FALSE);
	SDL_SetWindowSize(main_window, scalers[scaler].width, scalers[scaler].height);

	if (fullscreen_display == -1)
	{
		window_center_in_display(window_get_display_index());
	}
	else
	{
		window_center_in_display(fullscreen_display);

		if (SDL_SetWindowFullscreen(main_window, SDL_WINDOW_FULLSCREEN_DESKTOP) != 0)
		{
			reinit_fullscreen(-1);
			return;
		}
	}
}

void video_on_win_resize(void)
{
	int w, h;
	int scaler_w, scaler_h;

	// Tell video to reinit if the window was manually resized by the user.
	// Also enforce a minimum size on the window.

	SDL_GetWindowSize(main_window, &w, &h);
	scaler_w = scalers[scaler].width;
	scaler_h = scalers[scaler].height;

	if (w < scaler_w || h < scaler_h)
	{
		w = w < scaler_w ? scaler_w : w;
		h = h < scaler_h ? scaler_h : h;

		SDL_SetWindowSize(main_window, w, h);
	}
}

void toggle_fullscreen(void)
{
	if (fullscreen_display != -1)
		reinit_fullscreen(-1);
	else
		reinit_fullscreen(SDL_GetWindowDisplayIndex(main_window));
}

bool init_scaler(unsigned int new_scaler)
{
	int w = scalers[new_scaler].width,
	    h = scalers[new_scaler].height;
	int bpp = main_window_tex_format->BitsPerPixel; // TODOSDL2

	scaler = new_scaler;

	deinit_texture();
	init_texture();

	if (fullscreen_display == -1)
	{
		// Changing scalers, when not in fullscreen mode, forces the window
		// to resize to exactly match the scaler's output dimensions.
		SDL_SetWindowSize(main_window, w, h);
		window_center_in_display(window_get_display_index());
	}

	switch (bpp)
	{
	case 32:
		scaler_function = scalers[scaler].scaler32;
		break;
	case 16:
		scaler_function = scalers[scaler].scaler16;
		break;
	default:
		scaler_function = NULL;
		break;
	}

	if (scaler_function == NULL)
	{
		assert(false);
		return false;
	}

	return true;
}

bool set_scaling_mode_by_name(const char *name)
{
	for (int i = 0; i < ScalingMode_MAX; ++i)
	{
		 if (strcmp(name, scaling_mode_names[i]) == 0)
		 {
			 scaling_mode = i;
			 return true;
		 }
	}
	return false;
}

void JE_clr256(SDL_Surface *screen)
{
	SDL_FillRect(screen, NULL, 0);
}

void JE_showVGA(void) 
{ 
	scale_and_flip(VGAScreen); 
}

static void calc_dst_render_rect(SDL_Surface *const src_surface, SDL_Rect *const dst_rect)
{
	// Decides how the logical output texture (after software scaling applied) will fit
	// in the window.

	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	int maxh_width, maxw_height;

	switch (scaling_mode)
	{
	case SCALE_CENTER:
		SDL_QueryTexture(main_window_texture, NULL, NULL, &dst_rect->w, &dst_rect->h);
		break;
	case SCALE_INTEGER:
		dst_rect->w = src_surface->w;
		dst_rect->h = src_surface->h;
		while (dst_rect->w + src_surface->w <= win_w && dst_rect->h + src_surface->h <= win_h)
		{
			dst_rect->w += src_surface->w;
			dst_rect->h += src_surface->h;
		}
		break;
	case SCALE_ASPECT_8_5:
		maxh_width = win_h * (8.f / 5.f);
		maxw_height = win_w * (5.f / 8.f);

		if (maxh_width > win_w)
		{
			dst_rect->w = win_w;
			dst_rect->h = maxw_height;
		}
		else
		{
			dst_rect->w = maxh_width;
			dst_rect->h = win_h;
		}
		break;
	case SCALE_ASPECT_4_3:
		maxh_width = win_h * (4.f / 3.f);
		maxw_height = win_w * (3.f / 4.f);

		if (maxh_width > win_w)
		{
			dst_rect->w = win_w;
			dst_rect->h = maxw_height;
		}
		else
		{
			dst_rect->w = maxh_width;
			dst_rect->h = win_h;
		}
		break;
	case SCALE_WIDESCREEN:
	case ScalingMode_MAX:
		assert(false);
		break;
	}

	dst_rect->x = (win_w - dst_rect->w) / 2;
	dst_rect->y = (win_h - dst_rect->h) / 2;
}

/**
 * Lays the frame out across a wide window.  Game pixels keep their original 4:3 shape
 * (5:6, since 320x200 was shown at 4:3).  While the in-game layout is up and the window is
 * wide enough, the playfield is centered and the HUD panel moves out beside it; otherwise
 * this is the same as Fit 4:3.
 */
static int calc_widescreen_regions(SDL_Surface *const src_surface, OutputRegion *const regions, SDL_Rect *const frame_rect)
{
	int win_w, win_h;
	SDL_GetWindowSize(main_window, &win_w, &win_h);

	// content width in game pixels; space for a panel on each side keeps the playfield centered
	const int split_w = playfield_width + 2 * (hud_panel_width + hud_panel_gap);
	const bool split = video_hud_split && win_w * 240 >= split_w * win_h;

	const int content_w = split ? split_w : src_surface->w,
	          content_h = src_surface->h;

	float pixel_w = win_h * (5.f / 6.f) / content_h,
	      pixel_h = (float)win_h / content_h;
	if (pixel_w * content_w > win_w)
	{
		pixel_w = (float)win_w / content_w;
		pixel_h = pixel_w * (6.f / 5.f);
	}

	const float origin_x = (win_w - content_w * pixel_w) / 2,
	            origin_y = (win_h - content_h * pixel_h) / 2;

	// maps a rect of game pixels, placed at content_x, to window pixels
	#define TO_WINDOW(rect, content_x, src_rect) \
		do { \
			(rect).x = roundf(origin_x + (content_x) * pixel_w); \
			(rect).w = roundf(origin_x + ((content_x) + (src_rect).w) * pixel_w) - (rect).x; \
			(rect).y = roundf(origin_y + (src_rect).y * pixel_h); \
			(rect).h = roundf(origin_y + ((src_rect).y + (src_rect).h) * pixel_h) - (rect).y; \
		} while (false)

	const SDL_Rect frame = { 0, 0, src_surface->w, src_surface->h };

	if (!split)
	{
		regions[0].src = frame;
		TO_WINDOW(regions[0].dst, 0, frame);
		*frame_rect = regions[0].dst;
		return 1;
	}

	const int playfield_x = hud_panel_width + hud_panel_gap;

	regions[0].src = (SDL_Rect){ 0, 0, playfield_width, src_surface->h };
	TO_WINDOW(regions[0].dst, playfield_x, regions[0].src);

	regions[1].src = (SDL_Rect){ playfield_width, 0, hud_panel_width, src_surface->h };
	TO_WINDOW(regions[1].dst, playfield_x + playfield_width + hud_panel_gap, regions[1].src);

	// the mouse maps through the frame as if it were still in one piece, anchored at the playfield
	TO_WINDOW(*frame_rect, playfield_x, frame);

	#undef TO_WINDOW

	return 2;
}

/** Returns the parts of the frame to draw and where; frame_rect is where the whole frame maps to. */
static int calc_output_regions(SDL_Surface *const src_surface, OutputRegion *const regions, SDL_Rect *const frame_rect)
{
	if (scaling_mode == SCALE_WIDESCREEN)
		return calc_widescreen_regions(src_surface, regions, frame_rect);

	regions[0].src = (SDL_Rect){ 0, 0, src_surface->w, src_surface->h };
	calc_dst_render_rect(src_surface, &regions[0].dst);
	*frame_rect = regions[0].dst;
	return 1;
}

static bool prepare_prescale_texture(int w, int h)
{
	if (prescale_texture != NULL && prescale_w == w && prescale_h == h)
		return true;

	if (prescale_texture != NULL)
		SDL_DestroyTexture(prescale_texture);

	prescale_w = prescale_h = 0;

	if (!SDL_RenderTargetSupported(main_window_renderer))
	{
		prescale_texture = NULL;
		return false;
	}

	// the scale quality hint applies to textures as they are created
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
	prescale_texture = SDL_CreateTexture(main_window_renderer, main_window_tex_format->format, SDL_TEXTUREACCESS_TARGET, w, h);
	SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "nearest");

	if (prescale_texture == NULL)
		return false;

	prescale_w = w;
	prescale_h = h;
	return true;
}

static void scale_and_flip(SDL_Surface *src_surface)
{
	assert(src_surface->format->BitsPerPixel == 8);

	// Do software scaling
	assert(scaler_function != NULL);
	scaler_function(src_surface, main_window_texture);

	OutputRegion regions[2];
	SDL_Rect frame_rect;
	const int region_count = calc_output_regions(src_surface, regions, &frame_rect);

	SDL_Texture *texture = main_window_texture;
	int texels_per_pixel_x = scalers[scaler].width / src_surface->w,
	    texels_per_pixel_y = scalers[scaler].height / src_surface->h;

	// Sharp scaling: for non-integer stretches, first enlarge by whole multiples without
	// filtering, then let only the small remaining stretch be filtered.  Every game pixel
	// comes out the same size, without the blur of filtering the whole stretch.
	if (scaling_mode != SCALE_CENTER && scaling_mode != SCALE_INTEGER)
	{
		const int multiple_x = MAX(1, regions[0].dst.w / (regions[0].src.w * texels_per_pixel_x)),
		          multiple_y = MAX(1, regions[0].dst.h / (regions[0].src.h * texels_per_pixel_y));

		if (prepare_prescale_texture(scalers[scaler].width * multiple_x, scalers[scaler].height * multiple_y))
		{
			SDL_SetRenderTarget(main_window_renderer, prescale_texture);
			SDL_RenderCopy(main_window_renderer, main_window_texture, NULL, NULL);
			SDL_SetRenderTarget(main_window_renderer, NULL);

			texture = prescale_texture;
			texels_per_pixel_x *= multiple_x;
			texels_per_pixel_y *= multiple_y;
		}
	}

	// Clear the window and blit the output texture to it
	SDL_SetRenderDrawColor(main_window_renderer, 0, 0, 0, 255);
	SDL_RenderClear(main_window_renderer);
	for (int i = 0; i < region_count; ++i)
	{
		const SDL_Rect src = {
			regions[i].src.x * texels_per_pixel_x,
			regions[i].src.y * texels_per_pixel_y,
			regions[i].src.w * texels_per_pixel_x,
			regions[i].src.h * texels_per_pixel_y,
		};
		SDL_RenderCopy(main_window_renderer, texture, &src, &regions[i].dst);
	}
	devscript_on_present(main_window_renderer);
	SDL_RenderPresent(main_window_renderer);

	// Save output rect to be used by mouse functions
	last_output_rect = frame_rect;
}

/** Maps a specified point in game screen coordinates to window coordinates. */
void mapScreenPointToWindow(Sint32 *const inout_x, Sint32 *const inout_y)
{
	*inout_x = (2 * *inout_x + 1) * last_output_rect.w / (2 * VGAScreen->w) + last_output_rect.x;
	*inout_y = (2 * *inout_y + 1) * last_output_rect.h / (2 * VGAScreen->h) + last_output_rect.y;
}

/** Maps a specified point in window coordinates to game screen coordinates. */
void mapWindowPointToScreen(Sint32 *const inout_x, Sint32 *const inout_y)
{
	*inout_x = (2 * (*inout_x - last_output_rect.x) + 1) * VGAScreen->w / (2 * last_output_rect.w);
	*inout_y = (2 * (*inout_y - last_output_rect.y) + 1) * VGAScreen->h / (2 * last_output_rect.h);
}

/** Scales a distance in window coordinates to game screen coordinates. */
void scaleWindowDistanceToScreen(Sint32 *const inout_x, Sint32 *const inout_y)
{
	*inout_x = (2 * *inout_x + 1) * VGAScreen->w / (2 * last_output_rect.w);
	*inout_y = (2 * *inout_y + 1) * VGAScreen->h / (2 * last_output_rect.h);
}
