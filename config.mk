PREFIX    = /usr/local
MANPREFIX = $(PREFIX)/share/man

CPPFLAGS =
CFLAGS   = -std=c99 -Wall -Wextra $(CPPFLAGS)
LDFLAGS  = -lXext -lXdamage -lXfixes -lXcomposite -lXrender -lX11
