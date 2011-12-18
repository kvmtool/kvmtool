#ifndef KVM_COMPILER_H_
#define KVM_COMPILER_H_

#ifndef __compiletime_error
# define __compiletime_error(message)
#endif

#define notrace __attribute__((no_instrument_function))

#endif /* KVM_COMPILER_H_ */
