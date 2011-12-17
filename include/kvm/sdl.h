#ifndef KVM__SDL_H
#define KVM__SDL_H

#include "kvm/util.h"

struct framebuffer;

#ifdef CONFIG_HAS_SDL
int sdl__init(struct framebuffer *fb);
int sdl__exit(struct framebuffer *fb);
#else
static inline void sdl__init(struct framebuffer *fb)
{
	die("SDL support not compiled in. (install the SDL-dev[el] package)");
}
static inline void sdl__exit(struct framebuffer *fb)
{
	die("SDL support not compiled in. (install the SDL-dev[el] package)");
}
#endif

#endif /* KVM__SDL_H */
