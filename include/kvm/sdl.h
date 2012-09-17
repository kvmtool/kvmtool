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
	if (kvm->cfg.sdl)
		die("SDL support not compiled in. (install the SDL-dev[el] package)");

	return 0;
}
static inline int sdl__exit(struct kvm *kvm)
{
	if (kvm->cfg.sdl)
		die("SDL support not compiled in. (install the SDL-dev[el] package)");

	return 0;
}
#endif

#endif /* KVM__SDL_H */
