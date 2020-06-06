About
-----

ce is my minimalistic editor I use on a day-to-day basis.

License
-------
Coma is licensed under the ISC license.

Building
--------

ce should build fine on MacOS, Linux and OpenBSD.

OpenBSD:
```
$ make
$ doas make install
```

MacOS:
```
$ make
$ sudo make install
```

Linux (requires libbsd):
```
$ env CFLAGS=-D_GNU_SOURCE make
$ sudo make install
```

Key bindings
------------
ce uses a modal approach to editing much like vi.

**normal mode key bindings**

k            = move up one row
j            = move down one row
h            = move left one byte
l            = move right one byte

$            = jump to end of line
0            = jump to start of line

ctrl-f       = page down
ctrl-b       = page up

x            = delete byte under cursor
n            = search forward for next occurance of search
N            = search backwards for previous occurance of search

i            = enter insert mode
o            = enter insert mode and add newline below cursor
O            = enter insert mode and add newline above cursor

:            = enter command mode
/            = enter search mode

ctrl-r       = show list of buffers
ctrl-z       = suspend ce

**insert mode key bindings**

arrow keys   = navigate around
esc          = back to normal mode

**buffer list key bindings**

k            = move up one row
j            = move down one row
enter        = select buffer

**command mode**

q            = quit ce
w            = write active buffer
d            = delete line under cursor
e            = open specified file
bc           = close current buffer
