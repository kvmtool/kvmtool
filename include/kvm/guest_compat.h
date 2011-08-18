#ifndef KVM__GUEST_COMPAT_H
#define KVM__GUEST_COMPAT_H

int compat__print_all_messages(void);
int compat__remove_message(int id);
int compat__add_message(const char *title, const char *description);


#endif