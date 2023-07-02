PREFIX    = /usr
MANPREFIX = $(PREFIX)/share/man

CC = c99

CPPFLAGS =
CFLAGS   = -Wall -Wextra
LDFLAGS  = -lXdamage -lXfixes -lXcomposite -lXrender -lX11
