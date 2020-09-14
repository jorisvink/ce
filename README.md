About
-----

ce is my minimalistic editor I use on a day-to-day basis.

License
-------
Coma Editor is licensed under the ISC license.

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

Linux:
```
$ env CFLAGS=-D_GNU_SOURCE make
$ sudo make install
```

Key bindings
------------
ce uses a modal approach to editing much like vi.

**normal mode key bindings**

C            = center view on line at cursor

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

[num]dd      = delete number of lines

[num]yy      = yank number of lines

[num]w       = jump to next word

[num]b       = jump to previous word

ds           = delete range start

de           = delete range end (deletes between ds<=>de)

cs           = yank range start (copy)

ce           = yank range end (yanks between cs<=>ce)

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

e            = open specified file

bc           = close current buffer
