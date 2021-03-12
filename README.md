About
-----

ce is my minimalistic editor I use on a day-to-day basis.

Is this editor for you? Definitely not, it is highly opinionated
and tailored to my requirements.

Its highlighting works best on a light background terminal.

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

s            = selection

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

ctrl-d       = directory listing of directory of active buffer

ctrl-z       = suspend ce

[num]dd      = delete number of lines

[num]yy      = yank number of lines

[num]w       = jump to next word

[num]b       = jump to previous word

ai<char>     = alter inside string with given character (" or ').

di<char>     = delete inside string with given character (" or ').

ctrl-p-k     = kill active process

ctrl-w +     = increase console window by num rows

ctrl-w -     = decrease console window by num rows

**insert mode key bindings**

arrow keys   = navigate around

esc          = back to normal mode

**buffer list key bindings**

k            = move up one row

j            = move down one row

enter        = select buffer

**command mode**

l            = load directory listing

q            = quit ce

w            = write active buffer

e            = open specified file

bc           = close current buffer

Select-execute
--------------

The select-execute function of ce allows you to select a string inside
the editor and get it to the execute it as a command by hitting enter.

If the contents of the selection points to a file, the file will be opened.

If a command is executed, the output will be placed into the buffer from
which the command was run.

KNOWN ISSUES
------------

The fuzzy search is slow on large directory trees.
