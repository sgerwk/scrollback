Scrollback
==========

A scrollback buffer for the linux console

How to run it
-------------

Calling ``scrollback /bin/bash`` in a linux console makes the shell run with a
scrollback buffer.

It can be called from ``.bashrc`` as well:

```
! $SCROLLBACK false && scrollback -c /bin/bash && exec scrollback /bin/bash
```

Keys
----

The keys for scrolling up and down are F11 and F12. The old combinations
shift-pageup and shift-pagedown can be used instead by calling ``loadkeys`` on
the included file ``keys.txt`` at least once, as root:

``
loadkeys keys.txt
``

