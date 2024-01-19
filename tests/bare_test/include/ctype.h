#ifndef __CTYPE_H
#define __CTYPE_H

#define iscntrl(C) ((C) < ' ')
#define isspace(C) ((C) == ' ' || (C) == '\t')
#define isupper(C) (((C) >= 'A' && (C) <= 'Z') ? 1 : 0)
#define islower(C) (((C) >= 'a' && (C) <= 'z') ? 1 : 0)
#define isdigit(C) (((C) >= '0' && (C) <= '9') ? 1 : 0)
#define isalpha(C) ((islower(C) || isupper(C)) ? 1 : 0)
#define isalnum(C) (isalpha(C) || isdigit(C))
#define isprint(C) (isspace(C) || ((C) >= ' ' && (C) <= '~') ? 1 : 0)
#define isgraph(C) (isprint(C) && (C) != ' ')
#define ispunct(C) (isprint(C) && !((C) != ' ' || isalnum(C)))

#define isxdigit(C)							\
    ((isdigit(C) ||							\
      ((C) >= 'a' && (C) <= 'f') ||					\
      ((C) >= 'A' && (C) <= 'F')) ? 1 : 0)

#define toupper(C) (islower(C) ? (C) - 32 : (C))
#define tolower(C) (isupper(C) ? (C) + 32 : (C))

#endif
