#ifndef KVM__SDL_H
#define KVM__SDL_H

#include "kvm/util.h"

struct framebuffer;

#ifdef CONFIG_HAS_SDL
int sdl__init(struct kvm *kvm);
int sdl__exit(struct kvm *kvm);
#else
static inline int sdl__init(struct kvm *kvm)
{
	die("SDL support not compiled in. (install the SDL-dev[el] package)");
}
static inline int sdl__exit(struct kvm *kvm)
{
	die("SDL support not compiled in. (install the SDL-dev[el] package)");
}
#endif

#endif /* KVM__SDL_H */
